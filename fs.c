#include "system.h"

typedef struct {
    uint8_t  jump[3];
    char     oem[8];
    uint16_t bytes_per_sector;
    uint8_t  sectors_per_cluster;
    uint16_t reserved_sectors;
    uint8_t  fat_count;
    uint16_t dir_entries;        /* Must be 0 for FAT32 */
    uint16_t total_sectors_16;   /* 0 for FAT32 */
    uint8_t  media_descriptor;
    uint16_t sectors_per_fat_16; /* 0 for FAT32 */
    uint16_t sectors_per_track;
    uint16_t num_heads;
    uint32_t hidden_sectors;
    uint32_t total_sectors_32;

    /* FAT32 Extended Fields */
    uint32_t sectors_per_fat_32;
    uint16_t ext_flags;
    uint16_t fs_version;
    uint32_t root_cluster;       /* Starting cluster of the root directory (usually 2) */
    uint16_t fs_info;
    uint16_t backup_boot_sector;
    uint8_t  reserved[12];
    uint8_t  drive_number;
    uint8_t  reserved1;
    uint8_t  boot_sig;
    uint32_t volume_id;
    char     volume_label[11];
    char     fs_type[8];
} __attribute__((packed)) fat32_bpb_t;

/* Helper macros for 32-bit cluster combining and setting */
#define get_cluster(entry) (((uint32_t)(entry).first_cluster_high << 16) | (entry).first_cluster_low)
#define set_cluster(entry, clus) do { \
    (entry).first_cluster_low = (uint16_t)((clus) & 0xFFFF); \
    (entry).first_cluster_high = (uint16_t)(((clus) >> 16) & 0xFFFF); \
} while(0)

typedef struct {
    char name[11]; uint8_t attributes; uint8_t sec_byte; /* <-- This is OUR security byte (formerly reserved) */
    uint8_t creation_time_tenths;
    uint16_t creation_time; uint16_t creation_date; uint16_t last_access_date;
    uint16_t first_cluster_high; uint16_t last_mod_time; uint16_t last_mod_date;
    uint16_t first_cluster_low; uint32_t size;
} __attribute__((packed)) fat_dir_entry_t;

/* --- INTERNAL FUNCTION PROTOTYPES --- */
uint32_t read_fat_entry(uint32_t cluster);
void write_fat_entry(uint32_t cluster, uint32_t value);
void format_fat_name(const char* fat_name, char* out_name);
void to_fat_name(const char* input, char* fat_name);
int find_entry_in(uint32_t dir_cluster, const char* target_name, fat_dir_entry_t* out_entry, uint32_t* out_sec, int* out_idx);
int find_entry(const char* target_name, fat_dir_entry_t* out_entry, uint32_t* out_sec, int* out_idx);
int find_free_entry_in(uint32_t dir_cluster, uint32_t* out_sec, int* out_idx);
int find_free_entry(uint32_t* out_sec, int* out_idx);
int check_current_dir_permission_in(uint32_t dir_cluster, int is_write);
void free_fat_chain(uint32_t cluster);

uint32_t root_dir_start;
uint32_t data_start;
uint32_t fat_start;
uint32_t root_cluster;
uint32_t sectors_per_fat;
uint32_t total_clusters;
uint8_t  sectors_per_cluster;
uint8_t  fat_count = 2;

uint32_t current_dir_sector;
uint32_t current_dir_cluster = 2; /* In FAT32, root starts at root_cluster (typically 2) */
static volatile int bypass_dir_permission = 0;

int fs_error = 0;

char current_path[256] = "/";

const char* get_current_path() {
    return current_path;
}

/* Extracts the next path component (e.g., "user" from "/user/app.sos") */
static const char* next_component(const char* path, char* out_comp) {
    while (*path == '/') path++;
    if (*path == '\0') {
        out_comp[0] = '\0';
        return NULL;
    }
    int i = 0;
    while (*path && *path != '/') {
        if (i < 63) out_comp[i++] = *path;
        path++;
    }
    out_comp[i] = '\0';
    return path;
}

/*
 * Resolves an absolute or relative path.
 * out_parent_cluster: receives the cluster of the parent directory containing the final element.
 * out_parent_sector : receives the sector of the parent directory.<br> * out_fat_name : receives the final FAT-formatted name (11 characters).
 * Returns 1 on success, 0 if the path is not found or invalid.
*/
int resolve_path(const char* path, uint32_t* out_parent_cluster, uint32_t* out_parent_sector, char* out_fat_name) {
    uint32_t cluster = current_dir_cluster;
    uint32_t sector = current_dir_sector;

    const char* p = path;
    /* If the path starts with '/', start the search from the root */
    if (p[0] == '/') {
        cluster = root_cluster;
        sector = data_start + ((root_cluster - 2) * sectors_per_cluster);
        p++;
    }

    char comp[64];
    char next_comp[64];
    
    p = next_component(p, comp);
    if (comp[0] == '\0') {
        /* The path was simply "/" */
        *out_parent_cluster = root_cluster;
        *out_parent_sector = data_start + ((root_cluster - 2) * sectors_per_cluster);
        out_fat_name[0] = '\0'; 
        return 1;
    }

    while (p != NULL) {
        const char* next_p = next_component(p, next_comp);
        if (next_comp[0] == '\0') {
            /* "comp" is the final element of the path */
            break;
        }

        /* "comp" is an intermediate directory; we must traverse into it */
        char comp_fat[11];
        to_fat_name(comp, comp_fat);

        fat_dir_entry_t entry;
        int found = find_entry_in(cluster, comp_fat, &entry, NULL, NULL);

        if (!found || !(entry.attributes & 0x10)) {
            return 0; /* Dossier intermédiaire introuvable ou invalide */
        }

        /* Prevents traversing a protected intermediate directory */
        if (comp_fat[0] != '.') {
            if (!check_permission_byte(entry.sec_byte, 0)) {
                return 0; /* Access to intermediate directory denied */
            }
        }

        /* Transition to the intermediate directory */
        cluster = get_cluster(entry);
        if (cluster == 0) {
            cluster = root_cluster;
        }
        sector = data_start + ((cluster - 2) * sectors_per_cluster);

        /* The next component becomes the current component */
        for (int i = 0; i < 64; i++) comp[i] = next_comp[i];
        p = next_p;
    }

    /* "comp" is the final element. Its parent directory is described by "cluster" and "sector" */
    *out_parent_cluster = cluster;
    *out_parent_sector = sector;
    to_fat_name(comp, out_fat_name);
    return 1;
}

/* Updates the current_path string when cd succeeds */
void update_path(const char* new_dir) {
    if (new_dir[0] == '.' && new_dir[1] == '\0') return; /* "." do nothing */
    
    int len = 0; 
    while (current_path[len]) len++;

    if (new_dir[0] == '.' && new_dir[1] == '.') { /* ".." traverse up */
        if (len <= 1) return; /* Already at the root */
        len--;
        while (len > 0 && current_path[len] != '/') len--;
        if (len == 0) current_path[1] = '\0';
        else current_path[len] = '\0';
        return;
    }

    /* Append the new directory */
    if (len > 1 && current_path[len - 1] != '/') current_path[len++] = '/';
    int i = 0;
    while (new_dir[i] && len < 254) {
        current_path[len++] = new_dir[i++];
    }
    current_path[len] = '\0';
}

/* Returns 1 if allowed, 0 if denied */
int check_permission_byte(uint8_t sec_byte, int is_write) {
    uint8_t active_uid = get_active_uid();
    if (active_uid == 0) return 1; 
    
    uint8_t owner_uid = sec_byte & 0x0F;
    if (active_uid == owner_uid) {
        return is_write ? (sec_byte & 0x20) : (sec_byte & 0x10);
    } else {
        return is_write ? (sec_byte & 0x80) : (sec_byte & 0x40);
    }
}

int check_current_dir_permission_in(uint32_t dir_cluster, int is_write) {
    if (bypass_dir_permission) return 1; /* Allows the kernel to write its own system files */
    uint8_t active_uid = get_active_uid();
    if (active_uid == 0) return 1; 
    if (dir_cluster == root_cluster || dir_cluster == 0) {
        return is_write ? 0 : 1;
    }
    
    /* Read the first sector of the target directory */
    uint32_t dir_sec = data_start + ((dir_cluster - 2) * sectors_per_cluster);
    uint8_t* sector = (uint8_t*)kmalloc(512);
    ata_read_sector(dir_sec, sector);
    fat_dir_entry_t* dir = (fat_dir_entry_t*)sector;
    
    int allowed = 0;
    /* The first entry of a FAT16 subdirectory is ALWAYS "." (itself) */
    if (dir[0].name[0] == '.') {
        allowed = check_permission_byte(dir[0].sec_byte, is_write);
    } else {
        allowed = 1; /* Fallback safety measure */
    }
    kfree(sector);
    return allowed;
}

int check_current_dir_permission(int is_write) {
    return check_current_dir_permission_in(current_dir_cluster, is_write);
}

void print_perms(uint8_t sec_byte) {
    kprintf("[%d|", sec_byte & 0x0F);
    kprintf((sec_byte & 0x10) ? "R" : "-");
    kprintf((sec_byte & 0x20) ? "W" : "-");
    kprintf((sec_byte & 0x40) ? "r" : "-");
    kprintf((sec_byte & 0x80) ? "w" : "-");
    kprintf("]    | ");
}

void fs_chown(const char* filename, uint8_t new_uid) {
    /* WILDCARD (*) HANDLING */
    if (strcmp(filename, "*") == 0) {
        uint32_t cluster = (current_dir_cluster == 0) ? root_cluster : current_dir_cluster;
        uint32_t sec = data_start + ((cluster - 2) * sectors_per_cluster);
        uint32_t sec_offset = 0;
        uint8_t* sector = (uint8_t*)kmalloc(512);
        char entry_name[13];
        int end_of_dir = 0; /* Clean end flag */

        while (1) {
            ata_read_sector(sec, sector);
            fat_dir_entry_t* dir = (fat_dir_entry_t*)sector;
            int modified = 0;

            for (int i = 0; i < 16; i++) {
                if (dir[i].name[0] == 0x00) {
                    end_of_dir = 1;
                    break; /* Stops the evaluation loop */
                }
                if (dir[i].name[0] == (char)0xE5 || dir[i].attributes == 0x0F) continue;

                format_fat_name(dir[i].name, entry_name);
                if (entry_name[0] == '.') continue; /* Ignore '.' and '..' */

                uint8_t active_uid = get_active_uid();
                if (active_uid == 0 || (dir[i].sec_byte & 0x0F) == active_uid) {
                    dir[i].sec_byte = (dir[i].sec_byte & 0xF0) | (new_uid & 0x0F);
                    modified = 1;
                    
                    if (dir[i].attributes & 0x10) {
                        uint32_t sub_cluster = get_cluster(dir[i]);
                        uint32_t sub_sec = data_start + ((sub_cluster - 2) * sectors_per_cluster);
                        uint8_t* sub_sector = (uint8_t*)kmalloc(512);
                        ata_read_sector(sub_sec, sub_sector);
                        fat_dir_entry_t* sub_dir = (fat_dir_entry_t*)sub_sector;
                        if (sub_dir[0].name[0] == '.') {
                            sub_dir[0].sec_byte = (sub_dir[0].sec_byte & 0xF0) | (new_uid & 0x0F);
                            ata_write_sector(sub_sec, sub_sector);
                        }
                        kfree(sub_sector);
                    }
                }
            }
            
            /* The modified sector is saved correctly */
            if (modified) {
                ata_write_sector(sec, sector);
            }

            if (end_of_dir) {
                break; /* Cleanly exits the main loop */
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
        kprintf("Ownership updated for all files.\n");
        return;
    }

    /* Standard processing of a single file */
    uint32_t parent_cluster, parent_sector;
    char fat_name[11];
    if (!resolve_path(filename, &parent_cluster, &parent_sector, fat_name) || fat_name[0] == '\0') {
        kprintf("Error: '%s' not found.\n", filename); return;
    }

    fat_dir_entry_t entry; uint32_t dir_sec; int dir_idx;
    if (!find_entry_in(parent_cluster, fat_name, &entry, &dir_sec, &dir_idx)) {
        kprintf("Error: '%s' not found.\n", filename); return;
    }
    uint8_t active_uid = get_active_uid();
    if (active_uid != 0 && (entry.sec_byte & 0x0F) != active_uid) {
        kprintf("Error: Operation not permitted.\n"); return;
    }
    
    uint8_t* dir_sector = (uint8_t*)kmalloc(512);
    ata_read_sector(dir_sec, dir_sector);
    fat_dir_entry_t* dir = (fat_dir_entry_t*)dir_sector;
    
    dir[dir_idx].sec_byte = (dir[dir_idx].sec_byte & 0xF0) | (new_uid & 0x0F);
    ata_write_sector(dir_sec, dir_sector);
    
    if (dir[dir_idx].attributes & 0x10) {
        uint32_t sub_cluster = get_cluster(dir[dir_idx]);
        uint32_t sub_sec = data_start + ((sub_cluster - 2) * sectors_per_cluster);
        uint8_t* sub_sector = (uint8_t*)kmalloc(512);
        ata_read_sector(sub_sec, sub_sector);
        fat_dir_entry_t* sub_dir = (fat_dir_entry_t*)sub_sector;
        if (sub_dir[0].name[0] == '.') {
            sub_dir[0].sec_byte = (sub_dir[0].sec_byte & 0xF0) | (new_uid & 0x0F);
            ata_write_sector(sub_sec, sub_sector);
        }
        kfree(sub_sector);
    }
    
    kfree(dir_sector);
}

void fs_chmod(const char* filename, uint8_t new_perms) {
    /* WILDCARD (*) HANDLING */
    if (strcmp(filename, "*") == 0) {
        uint32_t cluster = (current_dir_cluster == 0) ? root_cluster : current_dir_cluster;
        uint32_t sec = data_start + ((cluster - 2) * sectors_per_cluster);
        uint32_t sec_offset = 0;
        uint8_t* sector = (uint8_t*)kmalloc(512);
        char entry_name[13];
        int end_of_dir = 0; /* Clean end flag */

        while (1) {
            ata_read_sector(sec, sector);
            fat_dir_entry_t* dir = (fat_dir_entry_t*)sector;
            int modified = 0;

            for (int i = 0; i < 16; i++) {
                if (dir[i].name[0] == 0x00) {
                    end_of_dir = 1;
                    break; /* Stops the evaluation loop */
                }
                if (dir[i].name[0] == (char)0xE5 || dir[i].attributes == 0x0F) continue;

                format_fat_name(dir[i].name, entry_name);
                if (entry_name[0] == '.') continue; /* Ignore '.' and '..' */

                uint8_t active_uid = get_active_uid();
                if (active_uid == 0 || (dir[i].sec_byte & 0x0F) == active_uid) {
                    dir[i].sec_byte = (dir[i].sec_byte & 0x0F) | (new_perms & 0xF0);
                    modified = 1;
                    
                    if (dir[i].attributes & 0x10) {
                        uint32_t sub_cluster = get_cluster(dir[i]);
                        uint32_t sub_sec = data_start + ((sub_cluster - 2) * sectors_per_cluster);
                        uint8_t* sub_sector = (uint8_t*)kmalloc(512);
                        ata_read_sector(sub_sec, sub_sector);
                        fat_dir_entry_t* sub_dir = (fat_dir_entry_t*)sub_sector;
                        if (sub_dir[0].name[0] == '.') {
                            sub_dir[0].sec_byte = (sub_dir[0].sec_byte & 0x0F) | (new_perms & 0xF0);
                            ata_write_sector(sub_sec, sub_sector);
                        }
                        kfree(sub_sector);
                    }
                }
            }
            
            /* The modified sector is saved correctly */
            if (modified) {
                ata_write_sector(sec, sector);
            }

            if (end_of_dir) {
                break; /* Cleanly exits the main loop */
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
        kprintf("Permissions updated for all files.\n");
        return;
    }

    /* Standard processing of a single file */
    uint32_t parent_cluster, parent_sector;
    char fat_name[11];
    if (!resolve_path(filename, &parent_cluster, &parent_sector, fat_name) || fat_name[0] == '\0') {
        kprintf("Error: '%s' not found.\n", filename); return;
    }

    fat_dir_entry_t entry; uint32_t dir_sec; int dir_idx;
    if (!find_entry_in(parent_cluster, fat_name, &entry, &dir_sec, &dir_idx)) {
        kprintf("Error: '%s' not found.\n", filename); return;
    }
    uint8_t active_uid = get_active_uid();
    if (active_uid != 0 && (entry.sec_byte & 0x0F) != active_uid) {
        kprintf("Error: Operation not permitted.\n"); return;
    }
    uint8_t* dir_sector = (uint8_t*)kmalloc(512);
    ata_read_sector(dir_sec, dir_sector);
    fat_dir_entry_t* dir = (fat_dir_entry_t*)dir_sector;
    
    dir[dir_idx].sec_byte = (dir[dir_idx].sec_byte & 0x0F) | (new_perms & 0xF0);
    ata_write_sector(dir_sec, dir_sector);
    
    if (dir[dir_idx].attributes & 0x10) {
        uint32_t sub_cluster = get_cluster(dir[dir_idx]);
        uint32_t sub_sec = data_start + ((sub_cluster - 2) * sectors_per_cluster);
        uint8_t* sub_sector = (uint8_t*)kmalloc(512);
        ata_read_sector(sub_sec, sub_sector);
        fat_dir_entry_t* sub_dir = (fat_dir_entry_t*)sub_sector;
        if (sub_dir[0].name[0] == '.') {
            sub_dir[0].sec_byte = (sub_dir[0].sec_byte & 0x0F) | (new_perms & 0xF0);
            ata_write_sector(sub_sec, sub_sector);
        }
        kfree(sub_sector);
    }
    
    kfree(dir_sector);
}

void init_fs() {
    uint8_t* boot_sector = (uint8_t*)kmalloc(512);
    ata_read_sector(0, boot_sector);
    fat32_bpb_t* bpb = (fat32_bpb_t*)boot_sector;
    
    fat_start = bpb->reserved_sectors;
    sectors_per_fat = bpb->sectors_per_fat_32;
    sectors_per_cluster = bpb->sectors_per_cluster;
    fat_count = bpb->fat_count ? bpb->fat_count : 2;
    root_cluster = bpb->root_cluster;
    
    /* In FAT32, data_start begins immediately after the reserved sectors and all FAT copies */
    data_start = bpb->reserved_sectors + (bpb->fat_count * sectors_per_fat);
    
    /* Calculate total clusters available on the drive */
    uint32_t total_sectors = (bpb->total_sectors_16 != 0) ? bpb->total_sectors_16 : bpb->total_sectors_32;
    uint32_t data_sectors = total_sectors - data_start;
    total_clusters = data_sectors / sectors_per_cluster;

    /* Root directory is located in the data area at root_cluster */
    root_dir_start = data_start + ((root_cluster - 2) * sectors_per_cluster);
    current_dir_cluster = root_cluster;
    current_dir_sector = root_dir_start;
    
    kprintf("[INFO] FS FAT32 OK (Root cluster: %d, Total clusters: %d).\n", root_cluster, total_clusters);
    kfree(boot_sector);
}

/* FAT32 reads: 128 entries per sector (4 bytes per entry), top 4 bits reserved */
uint32_t read_fat_entry(uint32_t cluster) {
    uint8_t* sector = (uint8_t*)kmalloc(512);
    uint32_t sector_lba = fat_start + (cluster / 128);
    uint32_t offset = cluster % 128;
    
    ata_read_sector(sector_lba, sector);
    uint32_t entry = ((uint32_t*)sector)[offset] & 0x0FFFFFFF;
    kfree(sector);
    return entry;
}

/* FAT32 writes: Preserves top 4 reserved bits */
void write_fat_entry(uint32_t cluster, uint32_t value) {
    uint8_t* sector = (uint8_t*)kmalloc(512);
    uint32_t sector_lba = fat_start + (cluster / 128);
    uint32_t offset = cluster % 128;
    
    ata_read_sector(sector_lba, sector);
    uint32_t old_entry = ((uint32_t*)sector)[offset];
    ((uint32_t*)sector)[offset] = (old_entry & 0xF0000000) | (value & 0x0FFFFFFF);
    for (uint8_t f = 0; f < fat_count; f++) {
        ata_write_sector(sector_lba + (f * sectors_per_fat), sector);
    }
    kfree(sector);
}

void format_fat_name(const char* fat_name, char* out_name) {
    int out_idx = 0;
    for(int i=0; i<8; i++) {
        if(fat_name[i] != ' ') out_name[out_idx++] = (fat_name[i] >= 'A' && fat_name[i] <= 'Z') ? fat_name[i] + 32 : fat_name[i];
    }
    if(fat_name[8] != ' ') {
        out_name[out_idx++] = '.';
        for(int i=8; i<11; i++) {
            if(fat_name[i] != ' ') out_name[out_idx++] = (fat_name[i] >= 'A' && fat_name[i] <= 'Z') ? fat_name[i] + 32 : fat_name[i];
        }
    }
    out_name[out_idx] = '\0';
}

/* Convert "test.txt" to "TEST    TXT", and handle "." and ".." */
void to_fat_name(const char* input, char* fat_name) {
    for(int i=0; i<11; i++) fat_name[i] = ' ';
    
    /* Very special cases for dot/dot-dot (navigation) directories */
    if (input[0] == '.' && input[1] == '\0') {
        fat_name[0] = '.'; return;
    }
    if (input[0] == '.' && input[1] == '.' && input[2] == '\0') {
        fat_name[0] = '.'; fat_name[1] = '.'; return;
    }

    int i=0, j=0;
    while(input[i] != '\0' && input[i] != '.' && j < 8) {
        fat_name[j++] = (input[i] >= 'a' && input[i] <= 'z') ? input[i] - 32 : input[i];
        i++;
    }
    if(input[i] == '.') {
        i++; j = 8;
        while(input[i] != '\0' && j < 11) {
            fat_name[j++] = (input[i] >= 'a' && input[i] <= 'z') ? input[i] - 32 : input[i];
            i++;
        }
    }
}

int fat_strcmp(const char* fat_name, const char* str) {
    for (int i = 0; i < 11; i++) if (fat_name[i] != str[i]) return 0;
    return 1;
}

/* Searches for a file in the specified directory (traverses all directory sectors) */
int find_entry_in(uint32_t dir_cluster, const char* target_name, fat_dir_entry_t* out_entry, uint32_t* out_sec, int* out_idx) {
    uint32_t cluster = (dir_cluster == 0) ? root_cluster : dir_cluster;
    uint32_t sec = data_start + ((cluster - 2) * sectors_per_cluster);
    uint32_t sec_offset = 0;
    uint8_t* sector = (uint8_t*)kmalloc(512);
    
    while (1) {
        ata_read_sector(sec, sector);
        fat_dir_entry_t* dir = (fat_dir_entry_t*)sector;
        
        for (int i = 0; i < 16; i++) {
            if (dir[i].name[0] == 0x00) goto not_found; /* End of directory */
            if (dir[i].name[0] != (char)0xE5 && target_name != NULL) {
                if (fat_strcmp(dir[i].name, target_name)) {
                    if (out_entry) *out_entry = dir[i];
                    if (out_sec) *out_sec = sec;
                    if (out_idx) *out_idx = i;
                    kfree(sector); return 1; /* Found! */
                }
            }
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
not_found:
    kfree(sector); return 0;
}

int find_entry(const char* target_name, fat_dir_entry_t* out_entry, uint32_t* out_sec, int* out_idx) {
    return find_entry_in(current_dir_cluster, target_name, out_entry, out_sec, out_idx);
}

/* Searches for a free entry in the specified directory, expanding the directory if needed */
int find_free_entry_in(uint32_t dir_cluster, uint32_t* out_sec, int* out_idx) {
    uint32_t cluster = (dir_cluster == 0) ? root_cluster : dir_cluster;
    uint32_t sec = data_start + ((cluster - 2) * sectors_per_cluster);
    uint32_t sec_offset = 0;
    uint8_t* sector = (uint8_t*)kmalloc(512);
    uint32_t prev_cluster = cluster;
    
    while (1) {
        ata_read_sector(sec, sector);
        fat_dir_entry_t* dir = (fat_dir_entry_t*)sector;
        for (int i = 0; i < 16; i++) {
            if (dir[i].name[0] == 0x00 || dir[i].name[0] == (char)0xE5) {
                *out_sec = sec; *out_idx = i;
                kfree(sector); return 1;
            }
        }

        sec_offset++;
        if (sec_offset % sectors_per_cluster == 0) {
            prev_cluster = cluster;
            cluster = read_fat_entry(cluster);
            if (cluster < 2 || cluster >= 0x0FFFFFF8) {
                /* Directory is full: dynamically allocate another cluster for it */
                uint32_t new_cluster = 0;
                for (uint32_t i = 2; i < total_clusters + 2; i++) {
                    if (read_fat_entry(i) == 0x00000000) {
                        new_cluster = i;
                        write_fat_entry(i, 0x0FFFFFFF);
                        break;
                    }
                }
                if (new_cluster == 0) {
                    kfree(sector);
                    return 0; /* Disk full */
                }
                write_fat_entry(prev_cluster, new_cluster);
                
                /* Zero out the new directory cluster */
                for (int b = 0; b < 512; b++) sector[b] = 0;
                uint32_t new_sec_lba = data_start + ((new_cluster - 2) * sectors_per_cluster);
                for (int s = 0; s < sectors_per_cluster; s++) {
                    ata_write_sector(new_sec_lba + s, sector);
                }
                
                *out_sec = new_sec_lba;
                *out_idx = 0;
                kfree(sector);
                return 1;
            }
            sec = data_start + ((cluster - 2) * sectors_per_cluster);
        } else {
            sec++;
        }
    }
    kfree(sector); return 0;
}

int find_free_entry(uint32_t* out_sec, int* out_idx) {
    return find_free_entry_in(current_dir_cluster, out_sec, out_idx);
}

/* Small utility function to format the size with one decimal place */
static void print_human_size(uint32_t size) {
    if (size < 1024) {
        kprintf("%d B", size);
    } else if (size < 1048576) { /* Less than 1 MB (1024 * 1024) */
        uint32_t kb = size / 1024;
        uint32_t kb_dec = (size % 1024) * 10 / 1024; /* Calculate the first decimal digit */
        kprintf("%d.%d KB", kb, kb_dec);
    } else { /* 1 MB or more */
        uint32_t mb = size / 1048576;
        uint32_t mb_dec = (size % 1048576) * 10 / 1048576;
        kprintf("%d.%d MB", mb, mb_dec);
    }
}

void fs_ls() {
    /* [SECURITY] Check if the user has permission to read the current directory */
    if (!check_current_dir_permission(0)) {
        kprintf("Error: Permission denied.\n");
        return;
    }

    vfs_node_t* dir_node = vfs_open(get_current_path(), O_RDONLY);
    if (!dir_node) {
        kprintf("Error: Permission denied.\n");
        return;
    }
    
    kprintf("Permissions | Name           | Type      | Size\n");
    kprintf("-------------------------------------------------\n");
    
    vfs_dirent_t dirent;
    uint32_t idx = 0;
    while (vfs_readdir(dir_node, idx++, &dirent)) {
        print_perms(dirent.sec_byte);
        kprintf("%s", dirent.name);
        
        int len = 0; while (dirent.name[len] != '\0') len++;
        for (int s = len; s < 15; s++) kprintf(" ");
        
        if (dirent.type == VFS_DIRECTORY) {
            kprintf("| Directory | -\n");
        } else {
            char type[4] = {' ', ' ', ' ', '\0'};
            int dot = -1;
            for (int k = 0; dirent.name[k] != '\0'; k++) {
                if (dirent.name[k] == '.') dot = k;
            }
            if (dot >= 0 && dirent.name[dot + 1] != '\0') {
                for (int k = 0; k < 3 && dirent.name[dot + 1 + k] != '\0'; k++) {
                    char c = dirent.name[dot + 1 + k];
                    type[k] = (c >= 'a' && c <= 'z') ? (char)(c - 32) : c;
                }
            }
            int is_sos = (type[0] == 'S' && type[1] == 'O' && type[2] == 'S');
            if (is_sos) {
                kprintf("| App       | ");
                print_human_size(dirent.size);
                kprintf("\n");
            } else {
                kprintf("| File: %s | ", type);
                print_human_size(dirent.size);
                kprintf("\n");
            }
        }
    }

    vfs_close(dir_node);
}

static void print_right_aligned_num(uint32_t val, int width) {
    char buf[16];
    int len = 0;
    if (val == 0) {
        buf[len++] = '0';
    } else {
        char temp[16];
        int tlen = 0;
        uint32_t v = val;
        while (v > 0) {
            temp[tlen++] = '0' + (v % 10);
            v /= 10;
        }
        for (int i = tlen - 1; i >= 0; i--) buf[len++] = temp[i];
    }
    buf[len] = '\0';
    for (int i = len; i < width; i++) kputc(' ');
    kprintf("%s", buf);
}

static void print_right_aligned_str(const char* str, int width) {
    int len = 0;
    while (str[len]) len++;
    for (int i = len; i < width; i++) kputc(' ');
    kprintf("%s", str);
}

static void format_human_size(uint32_t kb, char* out) {
    if (kb < 1024) {
        char temp[16]; int tlen = 0; uint32_t v = kb;
        if (v == 0) temp[tlen++] = '0';
        else { while (v > 0) { temp[tlen++] = '0' + (v % 10); v /= 10; } }
        int idx = 0;
        for (int i = tlen - 1; i >= 0; i--) out[idx++] = temp[i];
        out[idx++] = 'K';
        out[idx] = '\0';
    } else if (kb < 1048576) {
        uint32_t mb = kb / 1024;
        uint32_t dec = ((kb % 1024) * 10) / 1024;
        char temp[16]; int tlen = 0; uint32_t v = mb;
        if (v == 0) temp[tlen++] = '0';
        else { while (v > 0) { temp[tlen++] = '0' + (v % 10); v /= 10; } }
        int idx = 0;
        for (int i = tlen - 1; i >= 0; i--) out[idx++] = temp[i];
        out[idx++] = '.';
        out[idx++] = '0' + dec;
        out[idx++] = 'M';
        out[idx] = '\0';
    } else {
        uint32_t gb = kb / 1048576;
        uint32_t dec = ((kb % 1048576) * 10) / 1048576;
        char temp[16]; int tlen = 0; uint32_t v = gb;
        if (v == 0) temp[tlen++] = '0';
        else { while (v > 0) { temp[tlen++] = '0' + (v % 10); v /= 10; } }
        int idx = 0;
        for (int i = tlen - 1; i >= 0; i--) out[idx++] = temp[i];
        out[idx++] = '.';
        out[idx++] = '0' + dec;
        out[idx++] = 'G';
        out[idx] = '\0';
    }
}

void fs_df(const char* arg) {
    if (arg && (strcmp(arg, "--help") == 0 || strcmp(arg, "-help") == 0)) {
        kprintf("Usage: df [OPTION]...\n");
        kprintf("Show information about file system disk space.\n\n");
        kprintf("  -h, --human-readable  print sizes in powers of 1024 (e.g., 64M)\n");
        kprintf("  -k                    like default, use 1024-byte blocks\n");
        kprintf("  -m                    use 1MB blocks\n");
        kprintf("  -i, --inodes          list inode/cluster usage instead of block usage\n");
        return;
    }

    uint8_t* sector = (uint8_t*)kmalloc(512);
    uint32_t free_clusters = 0;
    uint32_t clusters_checked = 0;
    
    for (uint32_t s = 0; s < sectors_per_fat && clusters_checked < total_clusters; s++) {
        ata_read_sector(fat_start + s, sector);
        uint32_t* fat_entries = (uint32_t*)sector;
        for (int i = 0; i < 128 && clusters_checked < total_clusters; i++) {
            uint32_t cluster_idx = s * 128 + i;
            if (cluster_idx >= 2 && cluster_idx < total_clusters + 2) {
                if ((fat_entries[i] & 0x0FFFFFFF) == 0x00000000) {
                    free_clusters++;
                }
                clusters_checked++;
            }
        }
    }
    kfree(sector);
    
    uint32_t total_kb = (total_clusters * sectors_per_cluster) >> 1;
    uint32_t free_kb = (free_clusters * sectors_per_cluster) >> 1;
    uint32_t used_kb = total_kb >= free_kb ? total_kb - free_kb : 0;
    uint32_t pct = total_kb > 0 ? (used_kb * 100) / total_kb : 0;

    int is_inodes = (arg && (strcmp(arg, "-i") == 0 || strcmp(arg, "--inodes") == 0));
    int is_human = (arg && (strcmp(arg, "-h") == 0 || strcmp(arg, "--human-readable") == 0));
    int is_mb = (arg && strcmp(arg, "-m") == 0);

    if (is_inodes) {
        uint32_t used_clusters = total_clusters >= free_clusters ? total_clusters - free_clusters : 0;
        uint32_t ipct = total_clusters > 0 ? (used_clusters * 100) / total_clusters : 0;
        kprintf("Filesystem        Inodes     IUsed     IFree  IUse%% Mounted on\n");
        kprintf("/dev/ata0   ");
        print_right_aligned_num(total_clusters, 10);
        print_right_aligned_num(used_clusters, 10);
        print_right_aligned_num(free_clusters, 10);
        if (ipct < 10) kprintf("     %d%% /\n", ipct);
        else if (ipct < 100) kprintf("    %d%% /\n", ipct);
        else kprintf("   %d%% /\n", ipct);
        return;
    }

    if (is_human) {
        char total_str[16], used_str[16], free_str[16];
        format_human_size(total_kb, total_str);
        format_human_size(used_kb, used_str);
        format_human_size(free_kb, free_str);

        kprintf("Filesystem          Size      Used     Avail  Use%% Mounted on\n");
        kprintf("/dev/ata0   ");
        print_right_aligned_str(total_str, 10);
        print_right_aligned_str(used_str, 10);
        print_right_aligned_str(free_str, 10);
        if (pct < 10) kprintf("     %d%% /\n", pct);
        else if (pct < 100) kprintf("    %d%% /\n", pct);
        else kprintf("   %d%% /\n", pct);
        return;
    }

    if (is_mb) {
        uint32_t total_mb = total_kb / 1024;
        uint32_t used_mb = (used_kb + 1023) / 1024;
        uint32_t free_mb = free_kb / 1024;
        kprintf("Filesystem     1M-blocks      Used Available  Use%% Mounted on\n");
        kprintf("/dev/ata0   ");
        print_right_aligned_num(total_mb, 12);
        print_right_aligned_num(used_mb, 10);
        print_right_aligned_num(free_mb, 10);
        if (pct < 10) kprintf("     %d%% /\n", pct);
        else if (pct < 100) kprintf("    %d%% /\n", pct);
        else kprintf("   %d%% /\n", pct);
        return;
    }

    /* Standard 1K-blocks (POSIX default) */
    kprintf("Filesystem     1K-blocks      Used Available  Use%% Mounted on\n");
    kprintf("/dev/ata0   ");
    print_right_aligned_num(total_kb, 12);
    print_right_aligned_num(used_kb, 10);
    print_right_aligned_num(free_kb, 10);
    if (pct < 10) kprintf("     %d%% /\n", pct);
    else if (pct < 100) kprintf("    %d%% /\n", pct);
    else kprintf("   %d%% /\n", pct);
}

void fs_cd(const char* path) {
    if (strcmp(path, "/") == 0 || strcmp(path, "") == 0) {
        current_dir_cluster = root_cluster;
        current_dir_sector = data_start + ((root_cluster - 2) * sectors_per_cluster);
        current_path[0] = '/';
        current_path[1] = '\0';
        return;
    }

    uint32_t parent_cluster, parent_sector;
    char target_fat[11];
    if (!resolve_path(path, &parent_cluster, &parent_sector, target_fat)) {
        kprintf("Error: '%s': No such file or directory\n", path);
        return;
    }

    if (target_fat[0] == '\0') {
        current_dir_cluster = root_cluster;
        current_dir_sector = data_start + ((root_cluster - 2) * sectors_per_cluster);
        current_path[0] = '/';
        current_path[1] = '\0';
        return;
    }

    /* Search for the final directory in its resolved parent directory */
    fat_dir_entry_t entry;
    int found = find_entry_in(parent_cluster, target_fat, &entry, NULL, NULL);

    if (!found) {
        kprintf("Error: '%s': No such directory\n", path);
        return;
    }
    if (!(entry.attributes & 0x10)) {
        kprintf("Error: '%s' is a file, not a directory\n", path);
        return;
    }

    /* Verification of read permissions */
    /* [SECURITY FIX] Ignore permission checks for "." and ".." */
    if (target_fat[0] != '.') {
        if (!check_permission_byte(entry.sec_byte, 0)) {
            kprintf("Error: Permission denied.\n");
            return;
        }
    }

    uint32_t cluster = get_cluster(entry);
    if (cluster == 0) {
        cluster = root_cluster;
    }
    current_dir_cluster = cluster;
    current_dir_sector = data_start + ((cluster - 2) * sectors_per_cluster);

    /* Dynamic and robust reconstruction of the command prompt path */
    if (path[0] == '/') {
        current_path[0] = '/';
        current_path[1] = '\0';
    }
    const char* track_p = path;
    char track_comp[64];
    while ((track_p = next_component(track_p, track_comp)) != NULL) {
        update_path(track_comp);
    }
}

void fs_mkdir(const char* dirname) {
    uint32_t parent_cluster, parent_sector;
    char fat_name[11];
    if (!resolve_path(dirname, &parent_cluster, &parent_sector, fat_name) || fat_name[0] == '\0' || fat_name[0] == '.') {
        kprintf("Error: Invalid path '%s'.\n", dirname);
        return;
    }

    int end = 0;
    while (dirname[end] != '\0') end++;
    while (end > 0 && dirname[end - 1] == '/') end--;
    int start = end;
    while (start > 0 && dirname[start - 1] != '/') start--;
    int len = end - start;
    if (len == 0 || len > 8) { kprintf("Error: Directory name too long.\n"); return; }

    if (find_entry_in(parent_cluster, fat_name, NULL, NULL, NULL)) {
        kprintf("Error: '%s' already exists.\n", dirname);
        return;
    }

    /* [SECURITY] Check if the user has permission to write to the parent directory */
    if (!check_current_dir_permission_in(parent_cluster, 1)) {
        kprintf("Error: Permission denied.\n");
        return;
    }

    /* 1. Find free cluster in FAT32 */
    uint32_t free_cluster = 0;
    for (uint32_t i = 2; i < total_clusters + 2; i++) {
        if (read_fat_entry(i) == 0x00000000) {
            free_cluster = i;
            write_fat_entry(i, 0x0FFFFFFF);
            break;
        }
    }
    if (!free_cluster) {
        kprintf("Error: No space left on device.\n");
        return;
    }

    /* 2. Save the directory name in the parent directory */
    uint32_t target_sec;
    int free_entry;
    if (!find_free_entry_in(parent_cluster, &target_sec, &free_entry)) {
        write_fat_entry(free_cluster, 0x00000000);
        kprintf("Error: No space left in parent folder.\n");
        return;
    }

    uint8_t* dir_sector = (uint8_t*)kmalloc(512);
    ata_read_sector(target_sec, dir_sector);
    fat_dir_entry_t* dir = (fat_dir_entry_t*)dir_sector;

    for(int i=0; i<11; i++) dir[free_entry].name[i] = fat_name[i];
    dir[free_entry].attributes = 0x10;
    set_cluster(dir[free_entry], free_cluster);
    dir[free_entry].size = 0;
    /* By default: Creator = owner. Permissions: Owner R+W (0x30), Others none -> Total 0x30 */
    dir[free_entry].sec_byte = (get_active_uid() & 0x0F) | 0x30;
    
    ata_write_sector(target_sec, dir_sector); /* Save to disk */
    kfree(dir_sector);

    /* 3. Initialize the new directory (Create "." and ".." links) */
    uint8_t* new_dir_sec = (uint8_t*)kmalloc(512);
    for(int i=0; i<512; i++) new_dir_sec[i] = 0; 
    fat_dir_entry_t* new_dir = (fat_dir_entry_t*)new_dir_sec;

    /* Directory "." (Itself) */
    for(int i=0; i<11; i++) new_dir[0].name[i] = ' ';
    new_dir[0].name[0] = '.';
    new_dir[0].attributes = 0x10;
    set_cluster(new_dir[0], free_cluster);
    new_dir[0].sec_byte = (get_active_uid() & 0x0F) | 0x30; 

    /* Directory ".." (Parent - In FAT32 spec, points to 0 if parent is root) */
    for(int i=0; i<11; i++) new_dir[1].name[i] = ' ';
    new_dir[1].name[0] = '.'; new_dir[1].name[1] = '.';
    new_dir[1].attributes = 0x10;
    uint32_t dotdot_cluster = (parent_cluster == root_cluster) ? 0 : parent_cluster;
    set_cluster(new_dir[1], dotdot_cluster);

    /* Save this new directory to its new disk space */
    uint32_t new_sec_lba = data_start + ((free_cluster - 2) * sectors_per_cluster);
    ata_write_sector(new_sec_lba, new_dir_sec);
    kfree(new_dir_sec);
}

void fs_cat(const char* filename) {
    vfs_node_t* node = vfs_open(filename, O_RDONLY);
    if (!node) {
        if (fs_error == FS_ERR_PERMISSION) {
            kprintf("Error: Permission denied.\n");
        } else {
            kprintf("Error: '%s': No such file.\n", filename);
        }
        return;
    }

    if (node->type == VFS_DIRECTORY) {
        kprintf("Error: '%s' is a directory.\n", filename);
        vfs_close(node);
        return;
    }
    
    kprintf("\n--- %s ---\n", filename);
    
    uint8_t* sector = (uint8_t*)kmalloc(512);
    uint32_t offset = 0;
    int bytes_read;
    
    while ((bytes_read = vfs_read(node, offset, 512, sector)) > 0) {
        for (int j = 0; j < bytes_read; j++) {
            kprintf("%c", sector[j]);
        }
        offset += (uint32_t)bytes_read;
    }
    
    kprintf("\n----------------\n");
    kfree(sector);
    vfs_close(node);
}

void free_fat_chain(uint32_t cluster) {
    if (cluster < 2) return;
    while (cluster >= 2 && cluster < 0x0FFFFFF8) {
        uint32_t next = read_fat_entry(cluster);
        write_fat_entry(cluster, 0x00000000);
        cluster = next;
    }
}

void fs_rm(const char* arg1, const char* arg2) {
    int recursive = 0;
    const char* filename = arg1;

    if (arg1 != NULL && arg1[0] == '-' && arg1[1] == 'r' && arg1[2] == '\0') {
        recursive = 1; filename = arg2;
    }
    if (filename == NULL) { kprintf("Usage: rm [-r] <file or directory>\n"); return; }

    /* WILDCARD (*) HANDLING */
    if (strcmp(filename, "*") == 0) {
        if (!check_current_dir_permission(1)) {
            kprintf("Error: Permission denied.\n");
            return;
        }

        if ((current_dir_cluster == root_cluster || current_dir_cluster == 0) && get_active_uid() != 0) {
            kprintf("Error: Permission denied.\n");
            return;
        }

        uint32_t cluster = (current_dir_cluster == 0) ? root_cluster : current_dir_cluster;
        uint32_t sec = data_start + ((cluster - 2) * sectors_per_cluster);
        uint32_t sec_offset = 0;
        uint8_t* sector = (uint8_t*)kmalloc(512);
        char entry_name[13];
        int end_of_dir = 0; /* Clean end flag */

        while (1) {
            ata_read_sector(sec, sector);
            fat_dir_entry_t* dir = (fat_dir_entry_t*)sector;
            int modified = 0;

            for (int i = 0; i < 16; i++) {
                if (dir[i].name[0] == 0x00) {
                    end_of_dir = 1;
                    break; /* Stops the evaluation loop */
                }
                if (dir[i].name[0] == (char)0xE5 || dir[i].attributes == 0x0F) continue;

                format_fat_name(dir[i].name, entry_name);
                if (entry_name[0] == '.') continue; /* Ignore '.' and '..' */

                /* --- KERNEL.BIN PROTECTION --- */
                if (entry_name[0]=='k' && entry_name[1]=='e' && entry_name[2]=='r' && entry_name[3]=='n' && 
                    entry_name[4]=='e' && entry_name[5]=='l' && entry_name[6]=='.' && entry_name[7]=='b' && 
                    entry_name[8]=='i' && entry_name[9]=='n' && entry_name[10]=='\0') {
                    kprintf("\n[WARNING] You are about to delete the system kernel (kernel.bin)!\n");
                    kprintf("This is irreversible and will brick the OS upon reboot.\n");
                    kprintf("Proceed? [y/N]: ");
                    extern char wait_key(void);
                    char choice = wait_key();
                    kprintf("%c\n", choice);
                    if (choice != 'y' && choice != 'Y') {
                        kprintf("Skipped kernel.bin.\n");
                        continue;
                    }
                }
                /* -------------------------------------- */

                if (dir[i].attributes & 0x10) {
                    uint32_t sub_cluster = get_cluster(dir[i]);
                    uint32_t target_sector = data_start + ((sub_cluster - 2) * sectors_per_cluster);
                    uint8_t* sub_sector = (uint8_t*)kmalloc(512);
                    ata_read_sector(target_sector, sub_sector);
                    fat_dir_entry_t* sub_dir = (fat_dir_entry_t*)sub_sector;
                    
                    int is_empty = 1;
                    for(int j = 0; j < 16; j++) {
                        if (sub_dir[j].name[0] == 0x00) break;
                        if (sub_dir[j].name[0] == (char)0xE5 || sub_dir[j].name[0] == '.') continue;
                        is_empty = 0; break;
                    }

                    if (!is_empty) {
                        if (!recursive) {
                            kprintf("Skipping directory '%s' (not empty). Use 'rm -r *' to force.\n", entry_name);
                            kfree(sub_sector);
                            continue;
                        }
                        /* Empty the directory recursively */
                        for(int j = 0; j < 16; j++) {
                            if (sub_dir[j].name[0] == 0x00) break;
                            if (sub_dir[j].name[0] == (char)0xE5 || sub_dir[j].name[0] == '.') continue;
                            free_fat_chain(get_cluster(sub_dir[j]));
                        }
                    }
                    kfree(sub_sector);
                }

                free_fat_chain(get_cluster(dir[i]));
                dir[i].name[0] = (char)0xE5;
                modified = 1;
            }

            /* Save the sector if it has been modified */
            if (modified) {
                ata_write_sector(sec, sector);
            }

            if (end_of_dir) {
                break; /* Cleanly exits the main loop */
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
        return;
    }

    /* Standard processing of a single file */
    uint32_t parent_cluster, parent_sector;
    char target_fat[11];
    if (!resolve_path(filename, &parent_cluster, &parent_sector, target_fat)) {
        kprintf("Error: Invalid path '%s'.\n", filename);
        return;
    }

    if (target_fat[0] == '.') {
        kprintf("Error: Unable to delete '.' or '..'\n");
        return;
    }

    /* --- KERNEL.BIN PROTECTION --- */
    if (target_fat[0]=='K' && target_fat[1]=='E' && target_fat[2]=='R' && target_fat[3]=='N' && 
        target_fat[4]=='E' && target_fat[5]=='L' && target_fat[6]==' ' && target_fat[7]==' ' && 
        target_fat[8]=='B' && target_fat[9]=='I' && target_fat[10]=='N') {
        kprintf("\n[WARNING] You are about to delete the system kernel (kernel.bin)!\n");
        kprintf("This is irreversible and will brick the OS upon reboot.\n");
        kprintf("Proceed? [y/N]: ");
        extern char wait_key(void);
        char choice = wait_key();
        kprintf("%c\n", choice);
        if (choice != 'y' && choice != 'Y') {
            kprintf("Aborted.\n");
            return;
        }
    }
    /* -------------------------------------- */

    if (!check_current_dir_permission_in(parent_cluster, 1)) {
        kprintf("Error: Permission denied.\n");
        return;
    }

    if ((parent_cluster == root_cluster || parent_cluster == 0) && get_active_uid() != 0) {
        kprintf("Error: Permission denied.\n");
        return;
    }

    fat_dir_entry_t entry;
    uint32_t dir_sec;
    int dir_idx;
    if (!find_entry_in(parent_cluster, target_fat, &entry, &dir_sec, &dir_idx)) {
        kprintf("Error: '%s': No such file or directory.\n", filename);
        return;
    }

    uint8_t* dir_sector = (uint8_t*)kmalloc(512);
    ata_read_sector(dir_sec, dir_sector);
    fat_dir_entry_t* dir = (fat_dir_entry_t*)dir_sector;
    uint32_t cluster = get_cluster(dir[dir_idx]);

    if (dir[dir_idx].attributes & 0x10) {
        uint32_t target_sector = data_start + ((cluster - 2) * sectors_per_cluster);
        uint8_t* sub_sector = (uint8_t*)kmalloc(512);
        ata_read_sector(target_sector, sub_sector);
        fat_dir_entry_t* sub_dir = (fat_dir_entry_t*)sub_sector;
        
        int is_empty = 1;
        for(int j = 0; j < 16; j++) {
            if (sub_dir[j].name[0] == 0x00) break;
            if (sub_dir[j].name[0] == (char)0xE5 || sub_dir[j].name[0] == '.') continue;
            is_empty = 0; break;
        }

        if (!is_empty) {
            if (!recursive) {
                kprintf("Error: Directory is not empty. Use 'rm -r' to force deletion.\n");
                kfree(sub_sector); kfree(dir_sector);
                return;
            }
            for(int j = 0; j < 16; j++) {
                if (sub_dir[j].name[0] == 0x00) break;
                if (sub_dir[j].name[0] == (char)0xE5 || sub_dir[j].name[0] == '.') continue;
                free_fat_chain(get_cluster(sub_dir[j]));
            }
        }
        kfree(sub_sector);
    }

    free_fat_chain(cluster);
    dir[dir_idx].name[0] = (char)0xE5;
    ata_write_sector(dir_sec, dir_sector);
    
    kfree(dir_sector);
}

/* Create an empty file (0 bytes) */
void fs_touch(const char* filename) {
    uint32_t parent_cluster, parent_sector;
    char target_fat[11];
    if (!resolve_path(filename, &parent_cluster, &parent_sector, target_fat)) {
        kprintf("Error: Invalid path '%s'.\n", filename);
        return;
    }

    const char* base = filename;
    for (int i = 0; filename[i] != '\0'; i++) {
        if (filename[i] == '/') base = &filename[i + 1];
    }
    int len = 0;
    while (base[len] != '\0' && base[len] != '/') len++;
    if (len == 0 || len > 12) { kprintf("Error: Filename too long.\n"); return; }

    if (find_entry_in(parent_cluster, target_fat, NULL, NULL, NULL)) { 
        kprintf("Error: '%s' already exists.\n", filename); 
        return; 
    }

    if (!check_current_dir_permission_in(parent_cluster, 1)) {
        kprintf("Error: Permission denied.\n");
        return;
    }

    uint32_t target_sec;
    int free_entry;
    if (!find_free_entry_in(parent_cluster, &target_sec, &free_entry)) {
        kprintf("Error: No space left in directory.\n"); 
        return;
    }

    uint8_t* dir_sector = (uint8_t*)kmalloc(512);
    ata_read_sector(target_sec, dir_sector);
    fat_dir_entry_t* dir = (fat_dir_entry_t*)dir_sector;

    for(int i=0; i<11; i++) dir[free_entry].name[i] = target_fat[i];
    dir[free_entry].attributes = 0x20; 
    dir[free_entry].first_cluster_low = 0; 
    dir[free_entry].first_cluster_high = 0; 
    dir[free_entry].size = 0;
    dir[free_entry].sec_byte = (get_active_uid() & 0x0F) | 0x30;

    ata_write_sector(target_sec, dir_sector);
    kfree(dir_sector);
}

/* Write text to a file via VFS (replaces the content) */
int fs_write(const char* filename, const char* text) {
    if (!filename || !text) return 0;
    uint32_t text_len = 0;
    while (text[text_len]) text_len++;

    vfs_node_t* node = vfs_open(filename, O_WRONLY | O_CREAT | O_TRUNC);
    if (!node) return 0;

    if (text_len > 0) {
        int written = vfs_write(node, 0, text_len, (const uint8_t*)text);
        if (written < 0 || (uint32_t)written != text_len) {
            vfs_close(node);
            return 0;
        }
    }
    vfs_close(node);
    return 1;
}

/* Robust binary file copy via VFS */
int fs_cp(const char* src_name, const char* dest_name) {
    uint32_t src_parent_cluster, src_parent_sector;
    char src_fat[11];
    if (!resolve_path(src_name, &src_parent_cluster, &src_parent_sector, src_fat) || src_fat[0] == '\0') {
        kprintf("Error: Invalid source path '%s'.\n", src_name);
        return 0;
    }

    uint32_t dest_parent_cluster, dest_parent_sector;
    char dest_fat[11];
    if (!resolve_path(dest_name, &dest_parent_cluster, &dest_parent_sector, dest_fat)) {
        kprintf("Error: Invalid destination path '%s'.\n", dest_name);
        return 0;
    }

    fat_dir_entry_t dest_dir_entry;
    int dest_exists = (dest_fat[0] != '\0') ? find_entry_in(dest_parent_cluster, dest_fat, &dest_dir_entry, NULL, NULL) : 0;

    /* UNIX BEHAVIOR: If the destination is an existing directory, copy THE FILE INSIDE IT */
    char actual_dest[256];
    if (dest_fat[0] == '\0' || (dest_exists && (dest_dir_entry.attributes & 0x10))) {
        char src_printable[13];
        format_fat_name(src_fat, src_printable);
        int d_idx = 0;
        while (dest_name[d_idx] && d_idx < 240) {
            actual_dest[d_idx] = dest_name[d_idx];
            d_idx++;
        }
        if (d_idx > 0 && actual_dest[d_idx - 1] != '/') {
            actual_dest[d_idx++] = '/';
        }
        int s_idx = 0;
        while (src_printable[s_idx] && d_idx < 255) {
            actual_dest[d_idx++] = src_printable[s_idx++];
        }
        actual_dest[d_idx] = '\0';
        if (!resolve_path(actual_dest, &dest_parent_cluster, &dest_parent_sector, dest_fat)) {
            kprintf("Error: Invalid destination path '%s'.\n", actual_dest);
            return 0;
        }
    } else {
        int d_idx = 0;
        while (dest_name[d_idx] && d_idx < 255) {
            actual_dest[d_idx] = dest_name[d_idx];
            d_idx++;
        }
        actual_dest[d_idx] = '\0';
    }

    /* 1. Open and validate the source file via VFS */
    vfs_node_t* src_node = vfs_open(src_name, O_RDONLY);
    if (!src_node) {
        if (fs_error == FS_ERR_PERMISSION) {
            kprintf("Error: Permission denied.\n");
        } else {
            kprintf("Error: Source file '%s' not found.\n", src_name);
        }
        return 0;
    }
    if (src_node->type == VFS_DIRECTORY) {
        kprintf("Error: '%s' is a directory.\n", src_name);
        vfs_close(src_node);
        return 0;
    }

    /* 2. Check if the destination file already exists in the target directory */
    if (find_entry_in(dest_parent_cluster, dest_fat, NULL, NULL, NULL)) {
        kprintf("Error: Destination file already exists.\n");
        vfs_close(src_node);
        return 0;
    }

    /* 3. Create and open destination file via VFS */
    vfs_node_t* dest_node = vfs_open(actual_dest, O_WRONLY | O_CREAT | O_TRUNC);
    if (!dest_node) {
        if (fs_error == FS_ERR_PERMISSION) {
            kprintf("Error: Permission denied.\n");
        } else {
            kprintf("Error: No space left in destination directory.\n");
        }
        vfs_close(src_node);
        return 0;
    }

    /* 4. Stream data across VFS nodes */
    uint32_t total_size = src_node->size;
    if (total_size > 0) {
        uint32_t buf_size = 512 * sectors_per_cluster;
        uint8_t* cluster_buf = (uint8_t*)kmalloc(buf_size);
        if (!cluster_buf) {
            vfs_close(src_node);
            vfs_close(dest_node);
            return 0;
        }

        uint32_t offset = 0;
        while (offset < total_size) {
            int r = vfs_read(src_node, offset, buf_size, cluster_buf);
            if (r <= 0) break;
            int w = vfs_write(dest_node, offset, (uint32_t)r, cluster_buf);
            if (w != r) {
                kprintf("Error: Disk full during copy.\n");
                kfree(cluster_buf);
                vfs_close(src_node);
                vfs_close(dest_node);
                fs_rm(actual_dest, NULL);
                return 0;
            }
            offset += (uint32_t)w;
        }
        kfree(cluster_buf);
    }

    vfs_close(src_node);
    vfs_close(dest_node);
    return 1;
}

/* Secure file movement */
void fs_mv(const char* src_name, const char* dest_name) {
    /* The source file is only deleted if the copy succeeded (returns 1) */
    if (fs_cp(src_name, dest_name)) {
        fs_rm(src_name, NULL); /* Delete the original using its standard path */
    } else {
        kprintf("Error: Move failed.\n");
    }
}

int fs_load_file(const char* filename, uint8_t** buffer_out, uint32_t* size_out) {
    fs_error = FS_ERR_NONE; /* Reset the error */

    vfs_node_t* node = vfs_open(filename, O_RDONLY);
    if (!node) {
        return 0;
    }
    if (node->type == VFS_DIRECTORY) {
        vfs_close(node);
        fs_error = FS_ERR_NOT_FOUND;
        return 0;
    }

    *size_out = node->size;
    *buffer_out = (uint8_t*)kmalloc(node->size ? node->size : 1);
    if (!*buffer_out) {
        vfs_close(node);
        return 0;
    }

    if (node->size > 0) {
        int bytes_read = vfs_read(node, 0, node->size, *buffer_out);
        if (bytes_read < 0) {
            kfree(*buffer_out);
            *buffer_out = NULL;
            vfs_close(node);
            return 0;
        }
    }

    vfs_close(node);
    return 1;
}

int fs_suggest_file(const char* target_name, char* out_suggestion) {
    uint32_t cluster = current_dir_cluster;
    uint32_t sec = current_dir_sector;
    uint32_t sec_offset = 0;
    uint8_t* sector = (uint8_t*)kmalloc(512);
    uint8_t* fat_sec = (uint8_t*)kmalloc(512);
    char printable_name[13];
    extern int edit_distance_is_one(const char* s1, const char* s2);

    while (1) {
        ata_read_sector(sec, sector);
        fat_dir_entry_t* dir = (fat_dir_entry_t*)sector;
        
        for (int i = 0; i < 16; i++) {
            if (dir[i].name[0] == 0x00) goto end_suggest;
            if (dir[i].name[0] == (char)0xE5 || dir[i].attributes == 0x0F) continue;
            
            /* Formats the FAT name (e.g., "APP     SOS" -> "app.sos") */
            format_fat_name(dir[i].name, printable_name);
            
            if (edit_distance_is_one(target_name, printable_name)) {
                /* Found! Copy the suggestion */
                int s = 0;
                while (printable_name[s]) {
                    out_suggestion[s] = printable_name[s];
                    s++;
                }
                out_suggestion[s] = '\0';
                kfree(sector); kfree(fat_sec);
                return 1; /* Suggestion available */
            }
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
end_suggest:
    kfree(sector); kfree(fat_sec);
    return 0; /* No close suggestions */
}

/* Scans the current directory to find files/folders starting with "prefix" */
/* Adaptive autocomplete version supporting complex paths (FAT16) */
int fs_complete(const char* prefix, char* out_match, int* out_match_count, int print_matches) {
    /* 1. Separate the path from the file prefix (looks for the last '/') */
    int last_slash = -1;
    for (int i = 0; prefix[i] != '\0'; i++) {
        if (prefix[i] == '/') last_slash = i;
    }

    char dir_path[256];
    char file_prefix[64];
    
    if (last_slash != -1) {
        if (last_slash == 0) {
            dir_path[0] = '/';
            dir_path[1] = '\0';
        } else {
            for (int i = 0; i < last_slash; i++) dir_path[i] = prefix[i];
            dir_path[last_slash] = '\0';
        }
        int j = 0;
        for (int i = last_slash + 1; prefix[i] != '\0'; i++) {
            file_prefix[j++] = prefix[i];
        }
        file_prefix[j] = '\0';
    } else {
        dir_path[0] = '\0';
        int j = 0;
        while (prefix[j]) {
            file_prefix[j] = prefix[j];
            j++;
        }
        file_prefix[j] = '\0';
    }

    /* 2. Temporarily resolve the target directory to scan by following the FAT */
    uint32_t target_cluster = current_dir_cluster;
    uint32_t target_sector = current_dir_sector;

    if (dir_path[0] != '\0') {
            uint32_t parent_cluster, parent_sector;
            char target_fat[11];
            if (!resolve_path(dir_path, &parent_cluster, &parent_sector, target_fat)) {
                *out_match_count = 0;
                return 0;
            }

            if (target_fat[0] == '\0') {
                target_cluster = root_cluster;
                target_sector = data_start + ((root_cluster - 2) * sectors_per_cluster);
            } else {
                fat_dir_entry_t entry;
                int found = find_entry_in(parent_cluster, target_fat, &entry, NULL, NULL);

                if (!found || !(entry.attributes & 0x10)) {
                    *out_match_count = 0;
                    return 0;
                }

                /* [SECURITY FIX] Block autocomplete if read is forbidden on the target directory */
                if (target_fat[0] != '.') {
                    if (!check_permission_byte(entry.sec_byte, 0)) {
                        *out_match_count = 0;
                        return 0; /* Access denied: Unable to list files */
                    }
                }

                target_cluster = get_cluster(entry);
                if (target_cluster == 0) target_cluster = root_cluster;
                target_sector = data_start + ((target_cluster - 2) * sectors_per_cluster);
            }
        }

    /* 3. Scan the resolved directory looking for matches */
    int file_prefix_len = 0;
    while (file_prefix[file_prefix_len]) file_prefix_len++;

    int match_count = 0;
    char printable_name[13];
    uint32_t cluster = target_cluster;
    uint32_t sec = target_sector;
    uint32_t sec_offset = 0;
    uint8_t* sector = (uint8_t*)kmalloc(512);
    
    while (1) {
        ata_read_sector(sec, sector);
        fat_dir_entry_t* dir = (fat_dir_entry_t*)sector;

        for (int i = 0; i < 16; i++) {
            if (dir[i].name[0] == 0x00) goto end_complete;
            if (dir[i].name[0] == (char)0xE5 || dir[i].attributes == 0x0F) continue;

            format_fat_name(dir[i].name, printable_name);
            
            if (printable_name[0] == '.' && (printable_name[1] == '\0' || (printable_name[1] == '.' && printable_name[2] == '\0'))) {
                continue;
            }

            int is_match = 1;
            for (int j = 0; j < file_prefix_len; j++) {
                if (printable_name[j] != file_prefix[j]) {
                    is_match = 0;
                    break;
                }
            }

            if (is_match) {
                match_count++;
                if (print_matches) {
                    /* Display the original path prefix for clarity */
                    if (dir_path[0] != '\0') {
                        if (dir_path[0] == '/' && dir_path[1] == '\0') {
                            kprintf("/");
                        } else {
                            kprintf("%s/", dir_path);
                        }
                    }
                    if (dir[i].attributes & 0x10) {
                        kprintf("%s/  ", printable_name);
                    } else {
                        kprintf("%s  ", printable_name);
                    }
                }
                if (match_count == 1) {
                    /* Reconstructing the full path for user autocomplete */
                    int idx = 0;
                    if (dir_path[0] != '\0') {
                        while (dir_path[idx] && idx < 240) {
                            out_match[idx] = dir_path[idx];
                            idx++;
                        }
                        if (idx > 0 && dir_path[idx - 1] != '/' && idx < 240) {
                            out_match[idx++] = '/';
                        }
                    }
                    int f_idx = 0;
                    while (printable_name[f_idx] && idx < 250) {
                        out_match[idx++] = printable_name[f_idx++];
                    }
                    if (dir[i].attributes & 0x10) {
                        if (idx < 254) out_match[idx++] = '/';
                    } else {
                        if (idx < 254) out_match[idx++] = ' ';
                    }
                    out_match[idx] = '\0';
                }
            }
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
end_complete:
    kfree(sector);
    *out_match_count = match_count;
    return match_count;
}

/* Helper to scan a directory cluster for executable ELF files (.sos) matching prefix */
static int scan_dir_for_executables(uint32_t dir_cluster, const char* prefix, int prefix_len,
                                    char matches[][32], int count, int max_matches) {
    uint32_t cluster = (dir_cluster == 0) ? root_cluster : dir_cluster;
    uint32_t sec = data_start + ((cluster - 2) * sectors_per_cluster);
    uint32_t sec_offset = 0;
    uint8_t* sector = (uint8_t*)kmalloc(512);
    char printable_name[13];

    while (1) {
        ata_read_sector(sec, sector);
        fat_dir_entry_t* dir = (fat_dir_entry_t*)sector;

        for (int i = 0; i < 16; i++) {
            if (dir[i].name[0] == 0x00) goto done_scan;
            if (dir[i].name[0] == (char)0xE5 || dir[i].attributes == 0x0F) continue;
            if (dir[i].attributes & 0x10) continue; /* Skip subdirectories */

            format_fat_name(dir[i].name, printable_name);

            /* Check for .sos extension */
            int name_len = 0;
            while (printable_name[name_len]) name_len++;
            if (name_len < 4) continue;
            if (printable_name[name_len - 4] != '.' ||
                printable_name[name_len - 3] != 's' ||
                printable_name[name_len - 2] != 'o' ||
                printable_name[name_len - 1] != 's') {
                continue;
            }

            /* Check prefix match */
            int is_match = 1;
            for (int j = 0; j < prefix_len; j++) {
                if (printable_name[j] != prefix[j]) {
                    is_match = 0;
                    break;
                }
            }
            if (!is_match) continue;

            /* Check for duplicates in matches list */
            int dup = 0;
            for (int k = 0; k < count; k++) {
                if (strcmp(matches[k], printable_name) == 0) {
                    dup = 1;
                    break;
                }
            }
            if (dup) continue;

            if (count < max_matches) {
                int mi = 0;
                while (printable_name[mi] && mi < 31) {
                    matches[count][mi] = printable_name[mi];
                    mi++;
                }
                matches[count][mi] = '\0';
                count++;
            }
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

done_scan:
    kfree(sector);
    return count;
}

/* Scans current directory and /bin for executable files (.sos) matching prefix */
int fs_get_matching_executables(const char* prefix, char matches[][32], int max_matches) {
    int prefix_len = 0;
    while (prefix[prefix_len]) prefix_len++;

    int count = 0;

    /* 1. Scan current directory if readable */
    if (check_current_dir_permission_in(current_dir_cluster, 0)) {
        count = scan_dir_for_executables(current_dir_cluster, prefix, prefix_len, matches, count, max_matches);
    }

    /* 2. Scan /bin directory (if it exists and is not the current directory) */
    uint32_t bin_parent_cluster, bin_parent_sector;
    char bin_fat[11];
    if (resolve_path("/bin", &bin_parent_cluster, &bin_parent_sector, bin_fat)) {
        if (bin_fat[0] != '\0') {
            fat_dir_entry_t bin_entry;
            if (find_entry_in(bin_parent_cluster, bin_fat, &bin_entry, NULL, NULL) && (bin_entry.attributes & 0x10)) {
                if (check_permission_byte(bin_entry.sec_byte, 0)) {
                    uint32_t bin_cluster = get_cluster(bin_entry);
                    if (bin_cluster == 0) bin_cluster = root_cluster;
                    if (bin_cluster != current_dir_cluster) {
                        count = scan_dir_for_executables(bin_cluster, prefix, prefix_len, matches, count, max_matches);
                    }
                }
            }
        }
    }

    return count;
}

/* Allocate a free cluster and zero its sectors on disk */
static uint32_t alloc_cluster(void) {
    for (uint32_t i = 2; i < total_clusters + 2; i++) {
        if (read_fat_entry(i) == 0x00000000) {
            write_fat_entry(i, 0x0FFFFFFF);
            uint8_t* zero_sec = (uint8_t*)kmalloc(512);
            if (zero_sec) {
                for (int b = 0; b < 512; b++) zero_sec[b] = 0;
                uint32_t sec_lba = data_start + ((i - 2) * sectors_per_cluster);
                for (int s = 0; s < sectors_per_cluster; s++) {
                    ata_write_sector(sec_lba + s, zero_sec);
                }
                kfree(zero_sec);
            }
            return i;
        }
    }
    return 0;
}

/* Offset-based FAT32 read */
int fat32_read_at(uint32_t first_cluster, uint32_t file_size, uint32_t offset, uint32_t count, uint8_t* buffer) {
    if (!buffer || offset >= file_size || count == 0) return 0;
    if (count > file_size - offset) {
        count = file_size - offset;
    }
    if (first_cluster < 2 || first_cluster >= 0x0FFFFFF8) return 0;

    uint32_t cluster_size = (uint32_t)sectors_per_cluster * 512;
    uint32_t cluster_skip = offset / cluster_size;
    uint32_t cluster = first_cluster;

    for (uint32_t i = 0; i < cluster_skip; i++) {
        cluster = read_fat_entry(cluster);
        if (cluster < 2 || cluster >= 0x0FFFFFF8) return 0;
    }

    uint8_t* sec_buf = (uint8_t*)kmalloc(512);
    if (!sec_buf) return -1;

    uint32_t bytes_read = 0;
    uint32_t in_cluster_offset = offset % cluster_size;
    uint32_t start_sec_in_cluster = in_cluster_offset / 512;
    uint32_t start_byte_in_sec = in_cluster_offset % 512;

    while (bytes_read < count && cluster >= 2 && cluster < 0x0FFFFFF8) {
        uint32_t cluster_lba = data_start + ((cluster - 2) * sectors_per_cluster);
        for (uint32_t s = start_sec_in_cluster; s < sectors_per_cluster && bytes_read < count; s++) {
            ata_read_sector(cluster_lba + s, sec_buf);
            uint32_t avail_in_sec = 512 - start_byte_in_sec;
            uint32_t remaining = count - bytes_read;
            uint32_t chunk = (remaining < avail_in_sec) ? remaining : avail_in_sec;
            for (uint32_t b = 0; b < chunk; b++) {
                buffer[bytes_read + b] = sec_buf[start_byte_in_sec + b];
            }
            bytes_read += chunk;
            start_byte_in_sec = 0;
        }
        start_sec_in_cluster = 0;
        if (bytes_read < count) {
            cluster = read_fat_entry(cluster);
        }
    }

    kfree(sec_buf);
    return (int)bytes_read;
}

/* Offset-based FAT32 write */
int fat32_write_at(uint32_t dir_sec, int dir_idx, uint32_t offset, uint32_t count, const uint8_t* buffer) {
    if (!buffer) return -1;
    if (count == 0) return 0;

    uint8_t* dir_sector = (uint8_t*)kmalloc(512);
    if (!dir_sector) return -1;
    ata_read_sector(dir_sec, dir_sector);
    fat_dir_entry_t* dir = (fat_dir_entry_t*)dir_sector;
    uint32_t first_cluster = get_cluster(dir[dir_idx]);
    uint32_t old_size = dir[dir_idx].size;
    kfree(dir_sector);

    if (first_cluster == 0) {
        first_cluster = alloc_cluster();
        if (first_cluster == 0) return -1;
    }

    uint32_t cluster_size = (uint32_t)sectors_per_cluster * 512;
    uint32_t cluster_skip = offset / cluster_size;
    uint32_t cluster = first_cluster;

    for (uint32_t i = 0; i < cluster_skip; i++) {
        uint32_t next = read_fat_entry(cluster);
        if (next < 2 || next >= 0x0FFFFFF8) {
            next = alloc_cluster();
            if (next == 0) return -1;
            write_fat_entry(cluster, next);
        }
        cluster = next;
    }

    uint8_t* sec_buf = (uint8_t*)kmalloc(512);
    if (!sec_buf) return -1;

    uint32_t bytes_written = 0;
    uint32_t in_cluster_offset = offset % cluster_size;
    uint32_t start_sec_in_cluster = in_cluster_offset / 512;
    uint32_t start_byte_in_sec = in_cluster_offset % 512;

    while (bytes_written < count) {
        uint32_t cluster_lba = data_start + ((cluster - 2) * sectors_per_cluster);
        for (uint32_t s = start_sec_in_cluster; s < sectors_per_cluster && bytes_written < count; s++) {
            uint32_t avail_in_sec = 512 - start_byte_in_sec;
            uint32_t remaining = count - bytes_written;
            uint32_t chunk = (remaining < avail_in_sec) ? remaining : avail_in_sec;

            if (start_byte_in_sec != 0 || chunk < 512) {
                ata_read_sector(cluster_lba + s, sec_buf);
            }
            for (uint32_t b = 0; b < chunk; b++) {
                sec_buf[start_byte_in_sec + b] = buffer[bytes_written + b];
            }
            ata_write_sector(cluster_lba + s, sec_buf);

            bytes_written += chunk;
            start_byte_in_sec = 0;
        }
        start_sec_in_cluster = 0;

        if (bytes_written < count) {
            uint32_t next = read_fat_entry(cluster);
            if (next < 2 || next >= 0x0FFFFFF8) {
                next = alloc_cluster();
                if (next == 0) break;
                write_fat_entry(cluster, next);
            }
            cluster = next;
        }
    }

    kfree(sec_buf);

    dir_sector = (uint8_t*)kmalloc(512);
    if (dir_sector) {
        ata_read_sector(dir_sec, dir_sector);
        dir = (fat_dir_entry_t*)dir_sector;
        set_cluster(dir[dir_idx], first_cluster);
        if (offset + bytes_written > old_size) {
            dir[dir_idx].size = offset + bytes_written;
        }
        ata_write_sector(dir_sec, dir_sector);
        kfree(dir_sector);
    }

    return (bytes_written > 0) ? (int)bytes_written : -1;
}

/* Append raw bytes to a file via VFS */
void fs_append_bytes(const char* filename, const uint8_t* data, uint32_t len) {
    if (!filename || !data || len == 0) return;
    bypass_dir_permission = 1; /* Enable temporary write permission in the parent directory */

    vfs_node_t* node = vfs_open(filename, O_WRONLY | O_CREAT | O_APPEND);
    if (node) {
        vfs_write(node, node->size, len, data);
        vfs_close(node);
    }

    bypass_dir_permission = 0; /* Immediately restore default security */
}

/* Append text to an existing file */
void fs_append(const char* filename, const char* text) {
    if (!filename || !text) return;
    uint32_t len = 0;
    while (text[len]) len++;
    fs_append_bytes(filename, (const uint8_t*)text, len);
}

int fs_write_bin(const char* filename, uint8_t* data, uint32_t size) {
    if (!filename) return 0;

    vfs_node_t* node = vfs_open(filename, O_WRONLY | O_CREAT | O_TRUNC);
    if (!node) return 0;

    if (size > 0) {
        int written = vfs_write(node, 0, size, data);
        if (written < 0 || (uint32_t)written != size) {
            vfs_close(node);
            return 0;
        }
    }

    vfs_close(node);
    return 1;
}