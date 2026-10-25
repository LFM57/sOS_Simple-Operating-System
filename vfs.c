#include "system.h"
#include "vfs.h"
#include "net.h"

/* FAT32 Directory Entry structure & internals from fs.c */
typedef struct {
    char name[11];
    uint8_t attributes;
    uint8_t sec_byte;
    uint8_t creation_time_tenths;
    uint16_t creation_time;
    uint16_t creation_date;
    uint16_t last_access_date;
    uint16_t first_cluster_high;
    uint16_t last_mod_time;
    uint16_t last_mod_date;
    uint16_t first_cluster_low;
    uint32_t size;
} __attribute__((packed)) fat_dir_entry_t;

#define fat_get_cluster(entry) (((uint32_t)(entry).first_cluster_high << 16) | (entry).first_cluster_low)
#define fat_set_cluster(entry, clus) do { \
    (entry).first_cluster_low = (uint16_t)((clus) & 0xFFFF); \
    (entry).first_cluster_high = (uint16_t)(((clus) >> 16) & 0xFFFF); \
} while(0)

extern uint32_t root_cluster;
extern uint32_t data_start;
extern uint8_t  sectors_per_cluster;

extern int resolve_path(const char* path, uint32_t* out_parent_cluster, uint32_t* out_parent_sector, char* out_fat_name);
extern int find_entry_in(uint32_t dir_cluster, const char* target_name, fat_dir_entry_t* out_entry, uint32_t* out_sec, int* out_idx);
extern int find_free_entry_in(uint32_t dir_cluster, uint32_t* out_sec, int* out_idx);
extern int check_current_dir_permission_in(uint32_t dir_cluster, int is_write);
extern void free_fat_chain(uint32_t cluster);
extern void format_fat_name(const char* fat_name, char* out_name);
extern void to_fat_name(const char* input, char* fat_name);
extern uint32_t read_fat_entry(uint32_t cluster);

extern task_t tasks[MAX_TASKS];
extern int current_task;
extern char wait_key(void);

/* --- TTY Character Device VFS Operations --- */

static int tty_vfs_read(struct vfs_node* node, uint32_t offset, uint32_t size, uint8_t* buffer) {
    (void)node;
    (void)offset;
    if (!buffer || size == 0) return 0;

    __asm__ volatile ("sti");
    buffer[0] = (uint8_t)wait_key();
    return 1;
}

static int tty_vfs_write(struct vfs_node* node, uint32_t offset, uint32_t size, const uint8_t* buffer) {
    (void)node;
    (void)offset;
    if (!buffer) return -1;
    if (size == 0) return 0;

    if (current_task > 0 && tasks[current_task].is_background) {
        char log_name[32] = "/task_0.log";
        log_name[6] = '0' + (char)tasks[current_task].id;
        fs_append_bytes(log_name, buffer, size);
    } else {
        for (uint32_t i = 0; i < size; i++) {
            kputc((char)buffer[i]);
        }
    }
    return (int)size;
}

static vfs_ops_t tty_vfs_ops = {
    .read = tty_vfs_read,
    .write = tty_vfs_write,
    .open = NULL,
    .close = NULL,
    .readdir = NULL,
    .finddir = NULL
};

static vfs_node_t tty_vfs_node;

/* --- FAT32 File/Directory VFS Operations --- */

static vfs_ops_t fat32_vfs_ops;

static int fat32_vfs_read(struct vfs_node* node, uint32_t offset, uint32_t size, uint8_t* buffer) {
    if (!node || node->type != VFS_FILE) return -1;
    return fat32_read_at(node->inode, node->size, offset, size, buffer);
}

static int fat32_vfs_write(struct vfs_node* node, uint32_t offset, uint32_t size, const uint8_t* buffer) {
    if (!node || node->type != VFS_FILE || node->dir_sec == 0) return -1;
    int written = fat32_write_at(node->dir_sec, (int)node->dir_idx, offset, size, buffer);
    if (written > 0) {
        if (offset + (uint32_t)written > node->size) {
            node->size = offset + (uint32_t)written;
        }
        if (node->inode == 0) {
            uint8_t* dir_sector = (uint8_t*)kmalloc(512);
            if (dir_sector) {
                ata_read_sector(node->dir_sec, dir_sector);
                fat_dir_entry_t* dir = (fat_dir_entry_t*)dir_sector;
                node->inode = fat_get_cluster(dir[node->dir_idx]);
                kfree(dir_sector);
            }
        }
    }
    return written;
}

static int fat32_vfs_open(struct vfs_node* node, uint32_t flags) {
    (void)node;
    (void)flags;
    return 0;
}

static int fat32_vfs_close(struct vfs_node* node) {
    (void)node;
    return 0;
}

static int fat32_vfs_readdir(struct vfs_node* node, uint32_t index, vfs_dirent_t* out_dirent) {
    if (!node || node->type != VFS_DIRECTORY || !out_dirent) return 0;

    uint32_t cluster = (node->inode == 0) ? root_cluster : node->inode;
    uint32_t sec = data_start + ((cluster - 2) * sectors_per_cluster);
    uint32_t sec_offset = 0;
    uint32_t cur_idx = 0;
    uint8_t* sector = (uint8_t*)kmalloc(512);
    if (!sector) return 0;

    while (1) {
        ata_read_sector(sec, sector);
        fat_dir_entry_t* dir = (fat_dir_entry_t*)sector;

        for (int i = 0; i < 16; i++) {
            if (dir[i].name[0] == 0x00) {
                kfree(sector);
                return 0;
            }
            if (dir[i].name[0] == (char)0xE5 || dir[i].attributes == 0x0F) continue;

            if (cur_idx == index) {
                for (int k = 0; k < 64; k++) out_dirent->name[k] = 0;
                format_fat_name(dir[i].name, out_dirent->name);
                out_dirent->size = dir[i].size;
                out_dirent->type = (dir[i].attributes & 0x10) ? VFS_DIRECTORY : VFS_FILE;
                out_dirent->sec_byte = dir[i].sec_byte;
                kfree(sector);
                return 1;
            }
            cur_idx++;
        }

        sec_offset++;
        if (sec_offset % sectors_per_cluster == 0) {
            cluster = read_fat_entry(cluster);
            if (cluster < 2 || cluster >= 0x0FFFFFF8) break;
            sec = data_start + ((cluster - 2) * sectors_per_cluster);
        } else {
            sec++;
        }
    }

    kfree(sector);
    return 0;
}

static struct vfs_node* fat32_vfs_finddir(struct vfs_node* node, const char* name) {
    if (!node || node->type != VFS_DIRECTORY || !name || name[0] == '\0') return NULL;

    char fat_name[11];
    to_fat_name(name, fat_name);

    fat_dir_entry_t entry;
    uint32_t dir_sec = 0;
    int dir_idx = 0;
    uint32_t dir_cluster = (node->inode == 0) ? root_cluster : node->inode;

    if (!find_entry_in(dir_cluster, fat_name, &entry, &dir_sec, &dir_idx)) {
        return NULL;
    }

    vfs_node_t* child = (vfs_node_t*)kmalloc(sizeof(vfs_node_t));
    if (!child) return NULL;

    for (int i = 0; i < 64; i++) child->name[i] = 0;
    format_fat_name(entry.name, child->name);
    child->type = (entry.attributes & 0x10) ? VFS_DIRECTORY : VFS_FILE;
    child->sec_byte = entry.sec_byte;
    child->size = entry.size;
    child->inode = fat_get_cluster(entry);
    child->dir_sec = dir_sec;
    child->dir_idx = (uint32_t)dir_idx;
    child->ref_count = 1;
    child->ops = &fat32_vfs_ops;
    child->private_data = NULL;
    return child;
}

static vfs_ops_t fat32_vfs_ops = {
    .read = fat32_vfs_read,
    .write = fat32_vfs_write,
    .open = fat32_vfs_open,
    .close = fat32_vfs_close,
    .readdir = fat32_vfs_readdir,
    .finddir = fat32_vfs_finddir
};

/* --- Socket VFS Operations --- */

extern socket_t sockets[MAX_SOCKETS];
extern void net_send_tcp_raw(int sock_idx, uint8_t flags, uint8_t* payload, uint16_t len);

static int socket_vfs_read(struct vfs_node* node, uint32_t offset, uint32_t size, uint8_t* buffer) {
    (void)offset;
    if (!node || node->type != VFS_SOCKET || !buffer) return -1;
    if (size == 0) return 0;

    int sock_idx = (int)(uintptr_t)node->private_data;
    if (sock_idx < 0 || sock_idx >= MAX_SOCKETS || !sockets[sock_idx].is_used) return -1;

    if (sockets[sock_idx].type == 1) {
        return net_recv_udp(sock_idx, buffer, size);
    } else if (sockets[sock_idx].type == 2) {
        uint32_t eflags;
        __asm__ volatile("pushf; pop %0; cli" : "=r"(eflags));

        if (!sockets[sock_idx].rx_ready || sockets[sock_idx].rx_len == 0) {
            __asm__ volatile("push %0; popf" : : "r"(eflags));
            return 0;
        }

        uint32_t old_len = sockets[sock_idx].rx_len;
        uint32_t copy_len = (old_len < size) ? old_len : size;
        for (uint32_t i = 0; i < copy_len; i++) {
            buffer[i] = sockets[sock_idx].rx_buffer[i];
        }

        if (copy_len < old_len) {
            uint32_t rem = old_len - copy_len;
            for (uint32_t i = 0; i < rem; i++) {
                sockets[sock_idx].rx_buffer[i] = sockets[sock_idx].rx_buffer[copy_len + i];
            }
            sockets[sock_idx].rx_len = rem;
            sockets[sock_idx].rx_ready = 1;
        } else {
            sockets[sock_idx].rx_len = 0;
            sockets[sock_idx].rx_ready = 0;
        }

        __asm__ volatile("push %0; popf" : : "r"(eflags));

        if (old_len >= SOCK_BUF_SIZE - 4096 && sockets[sock_idx].tcp_state == TCP_ESTABLISHED) {
            net_send_tcp_raw(sock_idx, 0x10, NULL, 0);
        }

        return (int)copy_len;
    }

    return -1;
}

static int socket_vfs_write(struct vfs_node* node, uint32_t offset, uint32_t size, const uint8_t* buffer) {
    (void)offset;
    if (!node || node->type != VFS_SOCKET || !buffer) return -1;
    if (size == 0) return 0;

    int sock_idx = (int)(uintptr_t)node->private_data;
    if (sock_idx < 0 || sock_idx >= MAX_SOCKETS || !sockets[sock_idx].is_used) return -1;

    if (sockets[sock_idx].type == 1) {
        uint16_t send_len = (size > 1472) ? 1472 : (uint16_t)size;
        net_send_udp(sock_idx, sockets[sock_idx].remote_ip, sockets[sock_idx].remote_port, (uint8_t*)buffer, send_len);
        return (int)send_len;
    } else if (sockets[sock_idx].type == 2) {
        uint16_t send_len = (size > 1460) ? 1460 : (uint16_t)size;
        net_send_tcp_raw(sock_idx, 0x18, (uint8_t*)buffer, send_len);
        sockets[sock_idx].seq_num += send_len;
        return (int)send_len;
    }

    return -1;
}

static int socket_vfs_close(struct vfs_node* node) {
    if (!node || node->type != VFS_SOCKET) return -1;
    if (node->ref_count > 0) return 0;

    int sock_idx = (int)(uintptr_t)node->private_data;
    if (sock_idx >= 0 && sock_idx < MAX_SOCKETS && sockets[sock_idx].is_used) {
        if (sockets[sock_idx].type == 2 && sockets[sock_idx].tcp_state == TCP_ESTABLISHED) {
            net_send_tcp_raw(sock_idx, 0x11, NULL, 0);
        }
        sockets[sock_idx].is_used = 0;
        sockets[sock_idx].tcp_state = TCP_CLOSED;
        sockets[sock_idx].rx_len = 0;
        sockets[sock_idx].rx_ready = 0;
    }
    return 0;
}

static vfs_ops_t socket_vfs_ops = {
    .read = socket_vfs_read,
    .write = socket_vfs_write,
    .open = NULL,
    .close = socket_vfs_close,
    .readdir = NULL,
    .finddir = NULL
};

vfs_node_t* vfs_create_socket_node(int sock_idx) {
    if (sock_idx < 0 || sock_idx >= MAX_SOCKETS) return NULL;
    vfs_node_t* node = (vfs_node_t*)kmalloc(sizeof(vfs_node_t));
    if (!node) return NULL;

    for (int i = 0; i < 64; i++) node->name[i] = 0;
    const char* sname = "socket";
    for (int i = 0; sname[i]; i++) node->name[i] = sname[i];

    node->type = VFS_SOCKET;
    node->sec_byte = 0xF0;
    node->size = 0;
    node->inode = 0;
    node->dir_sec = 0;
    node->dir_idx = 0;
    node->ref_count = 1;
    node->ops = &socket_vfs_ops;
    node->private_data = (void*)(uintptr_t)sock_idx;
    return node;
}

/* --- Anonymous Pipe VFS Operations --- */

typedef struct {
    uint8_t buffer[PIPE_BUF_SIZE];
    uint32_t read_pos;
    uint32_t write_pos;
    uint32_t count;
    int readers;
    int writers;
    wait_queue_t read_queue;
    wait_queue_t write_queue;
} pipe_t;

static vfs_ops_t pipe_read_ops;
static vfs_ops_t pipe_write_ops;

static int pipe_has_other_task_writer(pipe_t* pipe) {
    for (int t = 0; t < MAX_TASKS; t++) {
        if (t == current_task) continue;
        if (tasks[t].state == 0 || tasks[t].state == 3) continue;
        for (int f = 0; f < MAX_OPEN_FILES; f++) {
            if (tasks[t].open_files[f].is_used &&
                tasks[t].open_files[f].node != NULL &&
                tasks[t].open_files[f].node->ops == &pipe_write_ops &&
                tasks[t].open_files[f].node->private_data == (void*)pipe) {
                return 1;
            }
        }
    }
    return 0;
}

static int pipe_has_other_task_reader(pipe_t* pipe) {
    for (int t = 0; t < MAX_TASKS; t++) {
        if (t == current_task) continue;
        if (tasks[t].state == 0 || tasks[t].state == 3) continue;
        for (int f = 0; f < MAX_OPEN_FILES; f++) {
            if (tasks[t].open_files[f].is_used &&
                tasks[t].open_files[f].node != NULL &&
                tasks[t].open_files[f].node->ops == &pipe_read_ops &&
                tasks[t].open_files[f].node->private_data == (void*)pipe) {
                return 1;
            }
        }
    }
    return 0;
}

static int pipe_vfs_read(struct vfs_node* node, uint32_t offset, uint32_t size, uint8_t* buffer) {
    (void)offset;
    if (!node || !node->private_data || !buffer) return -1;
    if (size == 0) return 0;

    pipe_t* pipe = (pipe_t*)node->private_data;

    while (pipe->count == 0) {
        if (pipe->writers == 0 || !pipe_has_other_task_writer(pipe)) {
            return 0;
        }
        sleep_on(&pipe->read_queue);
    }

    uint32_t bytes_to_read = (size < pipe->count) ? size : pipe->count;
    for (uint32_t i = 0; i < bytes_to_read; i++) {
        buffer[i] = pipe->buffer[pipe->read_pos];
        pipe->read_pos = (pipe->read_pos + 1) % PIPE_BUF_SIZE;
    }
    pipe->count -= bytes_to_read;
    node->size = pipe->count;

    wake_up(&pipe->write_queue);
    return (int)bytes_to_read;
}

static int pipe_vfs_write(struct vfs_node* node, uint32_t offset, uint32_t size, const uint8_t* buffer) {
    (void)offset;
    if (!node || !node->private_data || !buffer) return -1;
    if (size == 0) return 0;

    pipe_t* pipe = (pipe_t*)node->private_data;

    if (pipe->readers == 0) {
        return -1;
    }

    uint32_t written = 0;
    while (written < size) {
        while (pipe->count == PIPE_BUF_SIZE) {
            if (pipe->readers == 0) {
                return (written > 0) ? (int)written : -1;
            }
            if (!pipe_has_other_task_reader(pipe)) {
                return (int)written;
            }
            sleep_on(&pipe->write_queue);
        }
        while (written < size && pipe->count < PIPE_BUF_SIZE) {
            pipe->buffer[pipe->write_pos] = buffer[written++];
            pipe->write_pos = (pipe->write_pos + 1) % PIPE_BUF_SIZE;
            pipe->count++;
        }
        wake_up(&pipe->read_queue);
    }
    node->size = pipe->count;

    return (int)written;
}

static int pipe_read_close(struct vfs_node* node) {
    if (!node || !node->private_data) return -1;
    if (node->ref_count > 0) return 0;

    pipe_t* pipe = (pipe_t*)node->private_data;
    if (pipe->readers > 0) pipe->readers--;
    wake_up(&pipe->write_queue);
    if (pipe->readers <= 0 && pipe->writers <= 0) {
        kfree(pipe);
    }
    node->private_data = NULL;
    return 0;
}

static int pipe_write_close(struct vfs_node* node) {
    if (!node || !node->private_data) return -1;
    if (node->ref_count > 0) return 0;

    pipe_t* pipe = (pipe_t*)node->private_data;
    if (pipe->writers > 0) pipe->writers--;
    wake_up(&pipe->read_queue);
    if (pipe->readers <= 0 && pipe->writers <= 0) {
        kfree(pipe);
    }
    node->private_data = NULL;
    return 0;
}

static vfs_ops_t pipe_read_ops = {
    .read = pipe_vfs_read,
    .write = NULL,
    .open = NULL,
    .close = pipe_read_close,
    .readdir = NULL,
    .finddir = NULL
};

static vfs_ops_t pipe_write_ops = {
    .read = NULL,
    .write = pipe_vfs_write,
    .open = NULL,
    .close = pipe_write_close,
    .readdir = NULL,
    .finddir = NULL
};

int vfs_create_pipe(vfs_node_t** out_read_node, vfs_node_t** out_write_node) {
    if (!out_read_node || !out_write_node) return -1;

    pipe_t* pipe = (pipe_t*)kmalloc(sizeof(pipe_t));
    if (!pipe) return -1;

    vfs_node_t* r_node = (vfs_node_t*)kmalloc(sizeof(vfs_node_t));
    if (!r_node) {
        kfree(pipe);
        return -1;
    }

    vfs_node_t* w_node = (vfs_node_t*)kmalloc(sizeof(vfs_node_t));
    if (!w_node) {
        kfree(r_node);
        kfree(pipe);
        return -1;
    }

    for (uint32_t i = 0; i < PIPE_BUF_SIZE; i++) pipe->buffer[i] = 0;
    pipe->read_pos = 0;
    pipe->write_pos = 0;
    pipe->count = 0;
    pipe->readers = 1;
    pipe->writers = 1;
    pipe->read_queue.head = NULL;
    pipe->write_queue.head = NULL;

    for (int i = 0; i < 64; i++) {
        r_node->name[i] = 0;
        w_node->name[i] = 0;
    }
    const char* rname = "pipe_r";
    const char* wname = "pipe_w";
    for (int i = 0; rname[i]; i++) r_node->name[i] = rname[i];
    for (int i = 0; wname[i]; i++) w_node->name[i] = wname[i];

    r_node->type = VFS_PIPE;
    r_node->sec_byte = 0xF0;
    r_node->size = 0;
    r_node->inode = 0;
    r_node->dir_sec = 0;
    r_node->dir_idx = 0;
    r_node->ref_count = 1;
    r_node->ops = &pipe_read_ops;
    r_node->private_data = (void*)pipe;

    w_node->type = VFS_PIPE;
    w_node->sec_byte = 0xF0;
    w_node->size = 0;
    w_node->inode = 0;
    w_node->dir_sec = 0;
    w_node->dir_idx = 0;
    w_node->ref_count = 1;
    w_node->ops = &pipe_write_ops;
    w_node->private_data = (void*)pipe;

    *out_read_node = r_node;
    *out_write_node = w_node;
    return 0;
}

/* --- Public VFS API --- */

void init_vfs(void) {
    const char* name = "tty";
    int i = 0;
    while (name[i]) {
        tty_vfs_node.name[i] = name[i];
        i++;
    }
    tty_vfs_node.name[i] = '\0';
    tty_vfs_node.type = VFS_CHARDEV;
    tty_vfs_node.sec_byte = 0xF0;
    tty_vfs_node.size = 0;
    tty_vfs_node.inode = 0;
    tty_vfs_node.dir_sec = 0;
    tty_vfs_node.dir_idx = 0;
    tty_vfs_node.ref_count = 1;
    tty_vfs_node.ops = &tty_vfs_ops;
    tty_vfs_node.private_data = NULL;

    kprintf("[INFO] VFS Layer OK.\n");
}

vfs_node_t* vfs_get_tty_node(void) {
    return &tty_vfs_node;
}

vfs_node_t* vfs_open(const char* path, uint32_t flags) {
    if (!path || path[0] == '\0') return NULL;
    fs_error = FS_ERR_NONE;

    uint32_t parent_cluster = 0, parent_sector = 0;
    char target_fat[11];
    if (!resolve_path(path, &parent_cluster, &parent_sector, target_fat)) {
        fs_error = FS_ERR_NOT_FOUND;
        return NULL;
    }

    /* Root directory special case ("/") */
    if (target_fat[0] == '\0') {
        if ((flags & 0x03) != O_RDONLY || (flags & (O_CREAT | O_TRUNC | O_APPEND))) {
            fs_error = FS_ERR_PERMISSION;
            return NULL;
        }
        vfs_node_t* root_node = (vfs_node_t*)kmalloc(sizeof(vfs_node_t));
        if (!root_node) return NULL;
        for (int i = 0; i < 64; i++) root_node->name[i] = 0;
        root_node->name[0] = '/';
        root_node->type = VFS_DIRECTORY;
        root_node->sec_byte = 0x70;
        root_node->size = 0;
        root_node->inode = root_cluster;
        root_node->dir_sec = 0;
        root_node->dir_idx = 0;
        root_node->ref_count = 1;
        root_node->ops = &fat32_vfs_ops;
        root_node->private_data = NULL;
        return root_node;
    }

    fat_dir_entry_t entry;
    uint32_t dir_sec = 0;
    int dir_idx = 0;
    int found = find_entry_in(parent_cluster, target_fat, &entry, &dir_sec, &dir_idx);

    if (!found) {
        if (!(flags & O_CREAT)) {
            fs_error = FS_ERR_NOT_FOUND;
            return NULL;
        }
        if (target_fat[0] == '.') {
            fs_error = FS_ERR_NOT_FOUND;
            return NULL;
        }

        const char* base = path;
        for (int i = 0; path[i] != '\0'; i++) {
            if (path[i] == '/') base = &path[i + 1];
        }
        int len = 0;
        while (base[len] != '\0' && base[len] != '/') len++;
        if (len == 0 || len > 12) {
            fs_error = FS_ERR_NOT_FOUND;
            return NULL;
        }

        if (!check_current_dir_permission_in(parent_cluster, 1)) {
            fs_error = FS_ERR_PERMISSION;
            return NULL;
        }

        if (!find_free_entry_in(parent_cluster, &dir_sec, &dir_idx)) {
            return NULL;
        }

        uint8_t* dir_sector = (uint8_t*)kmalloc(512);
        if (!dir_sector) return NULL;
        ata_read_sector(dir_sec, dir_sector);
        fat_dir_entry_t* dir = (fat_dir_entry_t*)dir_sector;

        for (int i = 0; i < 11; i++) dir[dir_idx].name[i] = target_fat[i];
        dir[dir_idx].attributes = 0x20;
        dir[dir_idx].first_cluster_low = 0;
        dir[dir_idx].first_cluster_high = 0;
        dir[dir_idx].size = 0;
        dir[dir_idx].sec_byte = (get_active_uid() & 0x0F) | 0x30;

        ata_write_sector(dir_sec, dir_sector);
        entry = dir[dir_idx];
        kfree(dir_sector);
    }

    uint32_t acc_mode = flags & 0x03;
    int want_read = (acc_mode == O_RDONLY || acc_mode == O_RDWR);
    int want_write = (acc_mode == O_WRONLY || acc_mode == O_RDWR || (flags & O_TRUNC) || (flags & O_APPEND));

    if ((entry.attributes & 0x10) && want_write) {
        fs_error = FS_ERR_PERMISSION;
        return NULL;
    }

    if (want_read && !check_permission_byte(entry.sec_byte, 0)) {
        fs_error = FS_ERR_PERMISSION;
        return NULL;
    }
    if (want_write && !check_permission_byte(entry.sec_byte, 1)) {
        fs_error = FS_ERR_PERMISSION;
        return NULL;
    }

    /* Truncate existing file if O_TRUNC and write access requested */
    if ((flags & O_TRUNC) && (acc_mode == O_WRONLY || acc_mode == O_RDWR) && !(entry.attributes & 0x10)) {
        uint32_t prev_cluster = fat_get_cluster(entry);
        if (prev_cluster != 0) {
            free_fat_chain(prev_cluster);
        }
        uint8_t* dir_sector = (uint8_t*)kmalloc(512);
        if (dir_sector) {
            ata_read_sector(dir_sec, dir_sector);
            fat_dir_entry_t* dir = (fat_dir_entry_t*)dir_sector;
            fat_set_cluster(dir[dir_idx], 0);
            dir[dir_idx].size = 0;
            ata_write_sector(dir_sec, dir_sector);
            entry = dir[dir_idx];
            kfree(dir_sector);
        }
    }

    vfs_node_t* node = (vfs_node_t*)kmalloc(sizeof(vfs_node_t));
    if (!node) return NULL;

    for (int i = 0; i < 64; i++) node->name[i] = 0;
    format_fat_name(entry.name, node->name);
    node->type = (entry.attributes & 0x10) ? VFS_DIRECTORY : VFS_FILE;
    node->sec_byte = entry.sec_byte;
    node->size = entry.size;
    node->inode = fat_get_cluster(entry);
    node->dir_sec = dir_sec;
    node->dir_idx = (uint32_t)dir_idx;
    node->ref_count = 1;
    node->ops = &fat32_vfs_ops;
    node->private_data = NULL;

    if (node->ops && node->ops->open) {
        node->ops->open(node, flags);
    }

    return node;
}

int vfs_read(vfs_node_t* node, uint32_t offset, uint32_t size, uint8_t* buffer) {
    if (!node || !node->ops || !node->ops->read) return -1;
    return node->ops->read(node, offset, size, buffer);
}

int vfs_write(vfs_node_t* node, uint32_t offset, uint32_t size, const uint8_t* buffer) {
    if (!node || !node->ops || !node->ops->write) return -1;
    return node->ops->write(node, offset, size, buffer);
}

void vfs_close(vfs_node_t* node) {
    if (!node || node == vfs_get_tty_node()) return;
    if (node->ref_count > 0) {
        node->ref_count--;
    }
    if (node->ref_count == 0) {
        if (node->ops && node->ops->close) {
            node->ops->close(node);
        }
        kfree(node);
    }
}

int vfs_readdir(vfs_node_t* node, uint32_t index, vfs_dirent_t* out_dirent) {
    if (!node || !node->ops || !node->ops->readdir) return 0;
    return node->ops->readdir(node, index, out_dirent);
}

vfs_node_t* vfs_finddir(vfs_node_t* node, const char* name) {
    if (!node || !node->ops || !node->ops->finddir) return NULL;
    return node->ops->finddir(node, name);
}
