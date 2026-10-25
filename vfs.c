#include "system.h"
#include "vfs.h"

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
    if (!node) return;
    if (node->ref_count > 0) {
        node->ref_count--;
    }
    if (node->ops && node->ops->close) {
        node->ops->close(node);
    }
    if (node->ref_count == 0 && node != vfs_get_tty_node()) {
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
