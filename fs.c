#include "system.h"

typedef struct {
    uint8_t  jump[3]; char oem[8]; uint16_t bytes_per_sector; uint8_t sectors_per_cluster;
    uint16_t reserved_sectors; uint8_t fat_count; uint16_t dir_entries;
    uint16_t total_sectors; uint8_t media_descriptor; uint16_t sectors_per_fat;
} __attribute__((packed)) fat16_bpb_t;

typedef struct {
    char name[11]; uint8_t attributes; uint8_t sec_byte; /* <-- This is OUR security byte (formerly reserved) */
    uint8_t creation_time_tenths;
    uint16_t creation_time; uint16_t creation_date; uint16_t last_access_date;
    uint16_t first_cluster_high; uint16_t last_mod_time; uint16_t last_mod_date;
    uint16_t first_cluster_low; uint32_t size;
} __attribute__((packed)) fat_dir_entry_t;

/* --- INTERNAL FUNCTION PROTOTYPES (Avoids implicit declaration type conflicts) --- */
uint16_t read_fat_entry(uint32_t cluster);
void write_fat_entry(uint32_t cluster, uint16_t value);
void format_fat_name(const char* fat_name, char* out_name);
void to_fat_name(const char* input, char* fat_name);
int find_entry(const char* target_name, fat_dir_entry_t* out_entry, uint32_t* out_sec, int* out_idx);
int find_free_entry(uint32_t* out_sec, int* out_idx);
void free_fat_chain(uint32_t cluster);

uint32_t root_dir_start;
uint32_t data_start;
uint32_t fat_start; /* Where the sector map begins */
uint8_t  sectors_per_cluster;
uint32_t root_dir_entries;

uint32_t current_dir_sector;
uint32_t current_dir_cluster = 0; /* Stores the cluster (0 = Root) */
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
        cluster = 0;
        sector = root_dir_start;
        p++;
    }

    char comp[64];
    char next_comp[64];
    
    p = next_component(p, comp);
    if (comp[0] == '\0') {
        /* The path was simply "/" */
        *out_parent_cluster = 0;
        *out_parent_sector = root_dir_start;
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

        /* Temporary save to use find_entry() within the intermediate context */
        uint32_t saved_cluster = current_dir_cluster;
        uint32_t saved_sector = current_dir_sector;
        current_dir_cluster = cluster;
        current_dir_sector = sector;

        fat_dir_entry_t entry;
        int found = find_entry(comp_fat, &entry, NULL, NULL);

        current_dir_cluster = saved_cluster;
        current_dir_sector = saved_sector;

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
        cluster = entry.first_cluster_low;
        if (cluster == 0) {
            sector = root_dir_start;
        } else {
            sector = data_start + ((cluster - 2) * sectors_per_cluster);
        }

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

int check_current_dir_permission(int is_write) {
    if (bypass_dir_permission) return 1; /* Allows the kernel to write its own system files */
    uint8_t active_uid = get_active_uid();
    if (active_uid == 0) return 1; 
    if (current_dir_cluster == 0) {
        return is_write ? 0 : 1;
    }
    
    /* Read the first sector of the current directory */
    uint8_t* sector = (uint8_t*)kmalloc(512);
    ata_read_sector(current_dir_sector, sector);
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
        uint32_t cluster = current_dir_cluster;
        uint32_t sec = current_dir_sector;
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

                if (current_uid == 0 || (dir[i].sec_byte & 0x0F) == current_uid) {
                    dir[i].sec_byte = (dir[i].sec_byte & 0xF0) | (new_uid & 0x0F);
                    modified = 1;
                    
                    if (dir[i].attributes & 0x10) {
                        uint32_t sub_cluster = dir[i].first_cluster_low;
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
            if (cluster == 0) { 
                if (sec_offset >= (root_dir_entries * 32 / 512)) break; 
                sec = root_dir_start + sec_offset; 
            } 
            else {
                if (sec_offset % sectors_per_cluster == 0) {
                    cluster = read_fat_entry(cluster);
                    if (cluster < 2 || cluster >= 0xFFF8) break; 
                    sec = data_start + ((cluster - 2) * sectors_per_cluster);
                } else sec++;
            }
        }
    chown_wildcard_end:
        kfree(sector);
        kprintf("Ownership updated for all files.\n");
        return;
    }

    /* Standard processing of a single file */
    char fat_name[11]; to_fat_name(filename, fat_name);
    fat_dir_entry_t entry; uint32_t dir_sec; int dir_idx;
    
    if (!find_entry(fat_name, &entry, &dir_sec, &dir_idx)) {
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
        uint32_t sub_cluster = dir[dir_idx].first_cluster_low;
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
        uint32_t cluster = current_dir_cluster;
        uint32_t sec = current_dir_sector;
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

                if (current_uid == 0 || (dir[i].sec_byte & 0x0F) == current_uid) {
                    dir[i].sec_byte = (dir[i].sec_byte & 0x0F) | (new_perms & 0xF0);
                    modified = 1;
                    
                    if (dir[i].attributes & 0x10) {
                        uint32_t sub_cluster = dir[i].first_cluster_low;
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
            if (cluster == 0) { 
                if (sec_offset >= (root_dir_entries * 32 / 512)) break; 
                sec = root_dir_start + sec_offset; 
            } 
            else {
                if (sec_offset % sectors_per_cluster == 0) {
                    cluster = read_fat_entry(cluster);
                    if (cluster < 2 || cluster >= 0xFFF8) break; 
                    sec = data_start + ((cluster - 2) * sectors_per_cluster);
                } else sec++;
            }
        }
        kfree(sector);
        kprintf("Permissions updated for all files.\n");
        return;
    }

    /* Standard processing of a single file */
    char fat_name[11]; to_fat_name(filename, fat_name);
    fat_dir_entry_t entry; uint32_t dir_sec; int dir_idx;
    
    if (!find_entry(fat_name, &entry, &dir_sec, &dir_idx)) { kprintf("Error: '%s' not found.\n", filename); return; }
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
        uint32_t sub_cluster = dir[dir_idx].first_cluster_low;
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
    fat16_bpb_t* bpb = (fat16_bpb_t*)boot_sector;
    
    fat_start = bpb->reserved_sectors;
    root_dir_start = bpb->reserved_sectors + (bpb->fat_count * bpb->sectors_per_fat);
    data_start = root_dir_start + ((bpb->dir_entries * 32 + 511) / 512);
    sectors_per_cluster = bpb->sectors_per_cluster;
    root_dir_entries = bpb->dir_entries;
    
    current_dir_sector = root_dir_start;
    current_dir_cluster = 0;
    
    kprintf("[INFO] FS FAT16 OK.\n");
    kfree(boot_sector);
}

/* FAT16 sectors secured against out-of-bounds reads/writes */
uint16_t read_fat_entry(uint32_t cluster) {
    uint8_t* sector = (uint8_t*)kmalloc(512);
    uint32_t sector_lba = fat_start + (cluster / 256);
    uint32_t offset = cluster % 256;
    
    ata_read_sector(sector_lba, sector);
    uint16_t entry = ((uint16_t*)sector)[offset];
    kfree(sector);
    return entry;
}

void write_fat_entry(uint32_t cluster, uint16_t value) {
    uint8_t* sector = (uint8_t*)kmalloc(512);
    uint32_t sector_lba = fat_start + (cluster / 256);
    uint32_t offset = cluster % 256;
    
    ata_read_sector(sector_lba, sector);
    ((uint16_t*)sector)[offset] = value;
    ata_write_sector(sector_lba, sector);
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

/* Searches for a file in the current directory (traverses all directory sectors) */
int find_entry(const char* target_name, fat_dir_entry_t* out_entry, uint32_t* out_sec, int* out_idx) {
    uint32_t cluster = current_dir_cluster;
    uint32_t sec = current_dir_sector;
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
        
        /* Move to the next sector of the directory */
        sec_offset++;
        if (cluster == 0) { /* Root directory */
            if (sec_offset >= (root_dir_entries * 32 / 512)) break;
            sec = root_dir_start + sec_offset;
        } else { /* Subdirectory (follow the FAT) */
            if (sec_offset % sectors_per_cluster == 0) {
                cluster = read_fat_entry(cluster);
                if (cluster < 2 || cluster >= 0xFFF8) break;
                sec = data_start + ((cluster - 2) * sectors_per_cluster);
            } else sec++;
        }
    }
not_found:
    kfree(sector); return 0;
}

/* Searches for a free entry in the current directory */
int find_free_entry(uint32_t* out_sec, int* out_idx) {
    uint32_t cluster = current_dir_cluster;
    uint32_t sec = current_dir_sector;
    uint32_t sec_offset = 0;
    uint8_t* sector = (uint8_t*)kmalloc(512);
    
    while (1) {
        ata_read_sector(sec, sector);
        fat_dir_entry_t* dir = (fat_dir_entry_t*)sector;
        for (int i = 0; i < 16; i++) {
            if (dir[i].name[0] == 0x00 || dir[i].name[0] == (char)0xE5) {
                *out_sec = sec; *out_idx = i;
                kfree(sector); return 1;
            }
        }
        /* Same traversal logic as above... */
        sec_offset++;
        if (cluster == 0) {
            if (sec_offset >= (root_dir_entries * 32 / 512)) break;
            sec = root_dir_start + sec_offset;
        } else {
            if (sec_offset % sectors_per_cluster == 0) {
                cluster = read_fat_entry(cluster);
                if (cluster < 2 || cluster >= 0xFFF8) break;
                sec = data_start + ((cluster - 2) * sectors_per_cluster);
            } else sec++;
        }
    }
    kfree(sector); return 0;
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
        kprintf("Error: Permission denied (Read access required).\n");
        return;
    }
    
    kprintf("Permissions | Name           | Type      | Size\n");
    kprintf("-------------------------------------------------\n");
    
    char printable_name[13];
    uint32_t cluster = current_dir_cluster;
    uint32_t sec = current_dir_sector;
    uint32_t sec_offset = 0;
    uint8_t* sector = (uint8_t*)kmalloc(512);
    
    while (1) {
        ata_read_sector(sec, sector);
        fat_dir_entry_t* dir = (fat_dir_entry_t*)sector;

        for (int i = 0; i < 16; i++) {
            if (dir[i].name[0] == 0x00) goto ls_end;
            if (dir[i].name[0] == (char)0xE5 || dir[i].attributes == 0x0F) continue;

            format_fat_name(dir[i].name, printable_name);
            print_perms(dir[i].sec_byte);
            kprintf("%s", printable_name);
            
            int len = 0; while(printable_name[len] != '\0') len++;
            for(int s = len; s < 15; s++) kprintf(" "); 
            
            if (dir[i].attributes & 0x10) {
                kprintf("| Directory | -\n");
            } else {
                /* Dynamic detection of the ".SOS" extension */
                int is_sos = (dir[i].name[8] == 'S' && dir[i].name[9] == 'O' && dir[i].name[10] == 'S');
                char type[4];
                type[0] = dir[i].name[8];
                type[1] = dir[i].name[9];
                type[2] = dir[i].name[10];
                type[3] = '\0';
                
                if (is_sos) {
                    kprintf("| App       | ");
                    print_human_size(dir[i].size);
                    kprintf("\n");
                } else {
                    kprintf("| File: %s | ", type);
                    print_human_size(dir[i].size);
                    kprintf("\n");
                }
            }
        }
        
        sec_offset++;
        if (cluster == 0) { 
            if (sec_offset >= (root_dir_entries * 32 / 512)) break; 
            sec = root_dir_start + sec_offset; 
        } 
        else {
            if (sec_offset % sectors_per_cluster == 0) {
                cluster = read_fat_entry(cluster);
                if (cluster < 2 || cluster >= 0xFFF8) break; 
                sec = data_start + ((cluster - 2) * sectors_per_cluster);
            } else sec++;
        }
    }
ls_end:
    kfree(sector);
}

void fs_df() {
    uint8_t* sector = (uint8_t*)kmalloc(512);
    uint32_t free_clusters = 0;
    
    /* Size of the first FAT table */
    uint32_t fat1_sectors = (root_dir_start - fat_start) / 2;
    
    /* Scan the FAT to count clusters marked as 0x0000 (Free) */
    for(uint32_t s = 0; s < fat1_sectors; s++) {
        ata_read_sector(fat_start + s, sector);
        uint16_t* fat_entries = (uint16_t*)sector;
        for(int i = 0; i < 256; i++) {
            if (fat_entries[i] == 0x0000) {
                free_clusters++;
            }
        }
    }
    kfree(sector);
    
    /* Conversion to kilobytes (Each cluster is 'sectors_per_cluster' * 512 bytes) */
    uint32_t free_kb = (free_clusters * sectors_per_cluster * 512) / 1024;
    
    kprintf("Free storage space: %d KB (%d.%d MB)\n", 
            free_kb, 
            free_kb / 1024, 
            (free_kb % 1024) * 10 / 1024);
}

void fs_cd(const char* path) {
    if (strcmp(path, "/") == 0 || strcmp(path, "") == 0) {
        current_dir_sector = root_dir_start;
        current_dir_cluster = 0;
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
        current_dir_sector = root_dir_start;
        current_dir_cluster = 0;
        current_path[0] = '/';
        current_path[1] = '\0';
        return;
    }

    /* Search for the final directory in its resolved parent directory */
    uint32_t saved_cluster = current_dir_cluster;
    uint32_t saved_sector = current_dir_sector;
    current_dir_cluster = parent_cluster;
    current_dir_sector = parent_sector;

    fat_dir_entry_t entry;
    int found = find_entry(target_fat, &entry, NULL, NULL);

    current_dir_cluster = saved_cluster;
    current_dir_sector = saved_sector;

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
            kprintf("Error: Permission denied (Read access required).\n");
            return;
        }
    }

    uint32_t cluster = entry.first_cluster_low;
    if (cluster == 0) {
        current_dir_sector = root_dir_start;
        current_dir_cluster = 0;
    } else {
        current_dir_sector = data_start + ((cluster - 2) * sectors_per_cluster);
        current_dir_cluster = cluster;
    }

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
    int len = 0; while(dirname[len] != '\0') len++;
    if (len > 8) { kprintf("Error: Directory name too long.\n"); return; }

    char fat_name[11]; to_fat_name(dirname, fat_name);
    if (find_entry(fat_name, NULL, NULL, NULL)) { kprintf("Error: '%s' already exists.\n", dirname); return; }

    /* [SECURITY] Check if the user has permission to write to the current directory */
    if (!check_current_dir_permission(1)) {
        kprintf("Error: Permission denied (Write access required).\n");
        return;
    }

    /* 1. Find free space on the disk (Reading the FAT table) */
    uint32_t free_cluster = 0;
    for (int i = 2; i < 256; i++) {
        if (read_fat_entry(i) == 0x0000) {
            free_cluster = i;
            write_fat_entry(i, 0xFFFF);
            break;
        }
    }
    if (!free_cluster) { kprintf("Error: No space left on device.\n"); return; }

    /* 2. Save the directory name in the current directory */
    uint32_t target_sec;
    int free_entry;
    if (!find_free_entry(&target_sec, &free_entry)) {
        kprintf("Error: No space left in parent folder.\n");
        return;
    }

    uint8_t* dir_sector = (uint8_t*)kmalloc(512);
    ata_read_sector(target_sec, dir_sector);
    fat_dir_entry_t* dir = (fat_dir_entry_t*)dir_sector;

    for(int i=0; i<11; i++) dir[free_entry].name[i] = fat_name[i];
    dir[free_entry].attributes = 0x10; /* Is a directory */
    dir[free_entry].first_cluster_low = (uint16_t)free_cluster;
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
    new_dir[0].first_cluster_low = (uint16_t)free_cluster;
    /* [SECURITY] Apply the creator's UID and 0x30 permissions (Owner R/W, none for others) */
    new_dir[0].sec_byte = (current_uid & 0x0F) | 0x30; 

    /* Directory ".." (Parent) */
    for(int i=0; i<11; i++) new_dir[1].name[i] = ' ';
    new_dir[1].name[0] = '.'; new_dir[1].name[1] = '.';
    new_dir[1].attributes = 0x10;
    new_dir[1].first_cluster_low = (uint16_t)current_dir_cluster;

    /* Save this new directory to its new disk space */
    uint32_t new_sec_lba = data_start + ((free_cluster - 2) * sectors_per_cluster);
    ata_write_sector(new_sec_lba, new_dir_sec);
    kfree(new_dir_sec);
}

void fs_cat(const char* filename) {
    uint32_t parent_cluster, parent_sector;
    char target_fat[11];
    if (!resolve_path(filename, &parent_cluster, &parent_sector, target_fat)) {
        kprintf("Error: '%s': No such file.\n", filename); 
        return;
    }

    uint32_t old_cluster = current_dir_cluster;
    uint32_t old_sector = current_dir_sector;
    current_dir_cluster = parent_cluster;
    current_dir_sector = parent_sector;

    fat_dir_entry_t entry;
    int found = find_entry(target_fat, &entry, NULL, NULL);

    if (!found) {
        kprintf("Error: '%s': No such file.\n", filename);
        current_dir_cluster = old_cluster;
        current_dir_sector = old_sector;
        return;
    }
    if (entry.attributes & 0x10) {
        kprintf("Error: '%s' is a directory.\n", filename);
        current_dir_cluster = old_cluster;
        current_dir_sector = old_sector;
        return;
    }

    if (!check_permission_byte(entry.sec_byte, 0)) {
        kprintf("Error: Permission denied (Read access required).\n");
        current_dir_cluster = old_cluster;
        current_dir_sector = old_sector;
        return;
    }
    
    kprintf("\n--- %s ---\n", filename);
    
    uint32_t bytes_left = entry.size;
    uint32_t cluster = entry.first_cluster_low;
    uint8_t* sector = (uint8_t*)kmalloc(512);
    
    while (bytes_left > 0 && cluster >= 2 && cluster < 0xFFF8) {
        uint32_t f_sec = data_start + ((cluster - 2) * sectors_per_cluster);
        for (int s = 0; s < sectors_per_cluster && bytes_left > 0; s++) {
            ata_read_sector(f_sec + s, sector);
            uint32_t chunk = (bytes_left > 512) ? 512 : bytes_left;
            for (uint32_t j = 0; j < chunk; j++) {
                kprintf("%c", sector[j]);
            }
            bytes_left -= chunk;
        }
        cluster = read_fat_entry(cluster);
    }
    
    kprintf("\n----------------\n");
    kfree(sector);

    current_dir_cluster = old_cluster;
    current_dir_sector = old_sector;
}

void free_fat_chain(uint32_t cluster) {
    if (cluster < 2) return;
    while (cluster >= 2 && cluster < 0xFFF8) {
        uint16_t next = read_fat_entry(cluster);
        write_fat_entry(cluster, 0x0000);
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
            kprintf("Error: Permission denied (Write access required).\n");
            return;
        }

        if (current_dir_cluster == 0 && current_uid != 0) {
            kprintf("Error: Permission denied (Write access to '/' required).\n");
            return;
        }

        uint32_t cluster = current_dir_cluster;
        uint32_t sec = current_dir_sector;
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

                if (dir[i].attributes & 0x10) {
                    uint32_t sub_cluster = dir[i].first_cluster_low;
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
                            free_fat_chain(sub_dir[j].first_cluster_low);
                        }
                    }
                    kfree(sub_sector);
                }

                free_fat_chain(dir[i].first_cluster_low);
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
            if (cluster == 0) { 
                if (sec_offset >= (root_dir_entries * 32 / 512)) break; 
                sec = root_dir_start + sec_offset; 
            } 
            else {
                if (sec_offset % sectors_per_cluster == 0) {
                    cluster = read_fat_entry(cluster);
                    if (cluster < 2 || cluster >= 0xFFF8) break; 
                    sec = data_start + ((cluster - 2) * sectors_per_cluster);
                } else sec++;
            }
        }
        kfree(sector);
        kprintf("All matched files deleted.\n");
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

    uint32_t old_cluster = current_dir_cluster;
    uint32_t old_sector = current_dir_sector;
    current_dir_cluster = parent_cluster;
    current_dir_sector = parent_sector;

    if (!check_current_dir_permission(1)) {
        kprintf("Error: Permission denied (Write access required).\n");
        current_dir_cluster = old_cluster;
        current_dir_sector = old_sector;
        return;
    }

    if (current_dir_cluster == 0 && current_uid != 0) {
        kprintf("Error: Permission denied (Write access to '/' required).\n");
        current_dir_cluster = old_cluster;
        current_dir_sector = old_sector;
        return;
    }

    uint8_t* dir_sector = (uint8_t*)kmalloc(512);
    ata_read_sector(current_dir_sector, dir_sector);
    fat_dir_entry_t* dir = (fat_dir_entry_t*)dir_sector;
    
    for (int i = 0; i < 16; i++) {
        if (dir[i].name[0] == 0x00) break;
        if (dir[i].name[0] == (char)0xE5) continue;
        
        if (fat_strcmp(dir[i].name, target_fat)) {
            uint32_t cluster = dir[i].first_cluster_low;
            
            if (dir[i].attributes & 0x10) {
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
                        current_dir_cluster = old_cluster;
                        current_dir_sector = old_sector;
                        return;
                    }
                    for(int j = 0; j < 16; j++) {
                        if (sub_dir[j].name[0] == 0x00) break;
                        if (sub_dir[j].name[0] == (char)0xE5 || sub_dir[j].name[0] == '.') continue;
                        free_fat_chain(sub_dir[j].first_cluster_low);
                    }
                }
                kfree(sub_sector);
            }

            free_fat_chain(cluster);
            dir[i].name[0] = (char)0xE5;
            ata_write_sector(current_dir_sector, dir_sector);
            
            kprintf("'%s' has been deleted.\n", filename);
            kfree(dir_sector);
            current_dir_cluster = old_cluster;
            current_dir_sector = old_sector;
            return;
        }
    }
    
    kprintf("Error: '%s': No such file or directory.\n", filename);
    kfree(dir_sector);
    current_dir_cluster = old_cluster;
    current_dir_sector = old_sector;
}

/* Create an empty file (0 bytes) */
void fs_touch(const char* filename) {
    uint32_t parent_cluster, parent_sector;
    char target_fat[11];
    if (!resolve_path(filename, &parent_cluster, &parent_sector, target_fat)) {
        kprintf("Error: Invalid path '%s'.\n", filename);
        return;
    }

    int len = 0; while(target_fat[len] && target_fat[len] != ' ') len++;
    if (len > 12) { kprintf("Error: Filename too long.\n"); return; }

    uint32_t old_cluster = current_dir_cluster;
    uint32_t old_sector = current_dir_sector;
    current_dir_cluster = parent_cluster;
    current_dir_sector = parent_sector;

    if (find_entry(target_fat, NULL, NULL, NULL)) { 
        kprintf("Error: '%s' already exists.\n", filename); 
        current_dir_cluster = old_cluster;
        current_dir_sector = old_sector;
        return; 
    }

    if (!check_current_dir_permission(1)) {
        kprintf("Error: Permission denied (Write access required).\n");
        current_dir_cluster = old_cluster;
        current_dir_sector = old_sector;
        return;
    }

    uint32_t target_sec;
    int free_entry;
    if (!find_free_entry(&target_sec, &free_entry)) {
        kprintf("Error: No space left in directory.\n"); 
        current_dir_cluster = old_cluster;
        current_dir_sector = old_sector;
        return;
    }

    uint8_t* dir_sector = (uint8_t*)kmalloc(512);
    ata_read_sector(target_sec, dir_sector);
    fat_dir_entry_t* dir = (fat_dir_entry_t*)dir_sector;

    for(int i=0; i<11; i++) dir[free_entry].name[i] = target_fat[i];
    dir[free_entry].attributes = 0x20; 
    dir[free_entry].first_cluster_low = 0; 
    dir[free_entry].size = 0;
    dir[free_entry].sec_byte = (get_active_uid() & 0x0F) | 0x30;

    ata_write_sector(target_sec, dir_sector);
    kfree(dir_sector);
    
    kprintf("File '%s' created.\n", filename);

    current_dir_cluster = old_cluster;
    current_dir_sector = old_sector;
}

/* Write text to a file (replaces the content) */
int fs_write(const char* filename, const char* text) {
    uint32_t parent_cluster, parent_sector;
    char target_fat[11];
    if (!resolve_path(filename, &parent_cluster, &parent_sector, target_fat)) {
        return 0; /* Invalid parent path */
    }

    uint32_t old_cluster = current_dir_cluster;
    uint32_t old_sector = current_dir_sector;
    current_dir_cluster = parent_cluster;
    current_dir_sector = parent_sector;

    fat_dir_entry_t entry;
    uint32_t dir_sec;
    int dir_idx;

    /* 1. Find the file or create it if it does not exist */
    if (!find_entry(target_fat, &entry, &dir_sec, &dir_idx)) {
        fs_touch(filename);
        if (!find_entry(target_fat, &entry, &dir_sec, &dir_idx)) {
            current_dir_cluster = old_cluster;
            current_dir_sector = old_sector;
            return 0;
        }
    }

    if (entry.attributes & 0x10) { 
        fs_error = FS_ERR_NOT_FOUND;
        current_dir_cluster = old_cluster;
        current_dir_sector = old_sector;
        return 0; 
    }

    if (!check_permission_byte(entry.sec_byte, 1)) {
        fs_error = FS_ERR_PERMISSION;
        current_dir_cluster = old_cluster;
        current_dir_sector = old_sector;
        return 0;
    }

    current_dir_cluster = old_cluster;
    current_dir_sector = old_sector;

    /* 2. Allocate a cluster if it does not have one yet */
    uint32_t cluster = entry.first_cluster_low;
    if (cluster == 0) {
        for (int i = 2; i < 256; i++) {
            if (read_fat_entry(i) == 0x0000) {
                cluster = i;
                write_fat_entry(i, 0xFFFF);
                break;
            }
        }
        if (cluster == 0) { return 0; }
    }

    /* 3. Prepare the data to be written */
    int text_len = 0; 
    while(text[text_len]) text_len++;
    if (text_len > 512) text_len = 512;

    uint32_t data_sec = data_start + ((cluster - 2) * sectors_per_cluster);
    uint8_t* sec_buf = (uint8_t*)kmalloc(512);
    
    for(int i=0; i<512; i++) sec_buf[i] = 0;
    for(int i=0; i<text_len; i++) sec_buf[i] = text[i];
    
    ata_write_sector(data_sec, sec_buf);
    kfree(sec_buf);

    /* 4. Update the size and the cluster in the parent entry */
    uint8_t* dir_sector = (uint8_t*)kmalloc(512);
    ata_read_sector(dir_sec, dir_sector);
    fat_dir_entry_t* dir = (fat_dir_entry_t*)dir_sector;
    
    dir[dir_idx].first_cluster_low = cluster;
    dir[dir_idx].size = text_len;
    
    ata_write_sector(dir_sec, dir_sector);
    kfree(dir_sector);

    return 1;
}

/* Robust binary file copy (Variable scope fix) */
int fs_cp(const char* src_name, const char* dest_name) {
    uint32_t src_parent_cluster, src_parent_sector;
    char src_fat[11];
    if (!resolve_path(src_name, &src_parent_cluster, &src_parent_sector, src_fat)) {
        kprintf("Error: Invalid source path '%s'.\n", src_name);
        return 0;
    }

    uint32_t dest_parent_cluster, dest_parent_sector;
    char dest_fat[11];
    if (!resolve_path(dest_name, &dest_parent_cluster, &dest_parent_sector, dest_fat)) {
        kprintf("Error: Invalid destination path '%s'.\n", dest_name);
        return 0;
    }

    uint32_t old_cluster = current_dir_cluster;
    uint32_t old_sector = current_dir_sector;
    current_dir_cluster = dest_parent_cluster;
    current_dir_sector = dest_parent_sector;

    fat_dir_entry_t dest_dir_entry;
    int dest_exists = find_entry(dest_fat, &dest_dir_entry, NULL, NULL);

    current_dir_cluster = old_cluster;
    current_dir_sector = old_sector;

    /* UNIX BEHAVIOR: If the destination is an existing directory, copy THE FILE INSIDE IT */
    if (dest_exists && (dest_dir_entry.attributes & 0x10)) {
        dest_parent_cluster = dest_dir_entry.first_cluster_low;
        dest_parent_sector = (dest_parent_cluster == 0) ? root_dir_start : data_start + ((dest_parent_cluster - 2) * sectors_per_cluster);
        for (int i = 0; i < 11; i++) {
            dest_fat[i] = src_fat[i];
        }
    }

    /* 1. Load and validate the source file */
    current_dir_cluster = src_parent_cluster;
    current_dir_sector = src_parent_sector;

    fat_dir_entry_t src_entry;
    uint32_t src_dir_sec;
    int src_dir_idx;
    int src_found = find_entry(src_fat, &src_entry, &src_dir_sec, &src_dir_idx);

    current_dir_cluster = old_cluster;
    current_dir_sector = old_sector;

    if (!src_found) {
        kprintf("Error: Source file '%s' not found.\n", src_name);
        return 0;
    }
    if (src_entry.attributes & 0x10) {
        kprintf("Error: '%s' is a directory.\n", src_name);
        return 0;
    }
    if (!check_permission_byte(src_entry.sec_byte, 0)) {
        kprintf("Error: Permission denied (Read source required).\n");
        return 0;
    }

    /* 2. Check if the destination file already exists in the target directory */
    current_dir_cluster = dest_parent_cluster;
    current_dir_sector = dest_parent_sector;

    if (find_entry(dest_fat, NULL, NULL, NULL)) {
        kprintf("Error: Destination file already exists.\n");
        current_dir_cluster = old_cluster;
        current_dir_sector = old_sector;
        return 0;
    }
    if (!check_current_dir_permission(1)) {
        kprintf("Error: Permission denied (Write to destination directory required).\n");
        current_dir_cluster = old_cluster;
        current_dir_sector = old_sector;
        return 0;
    }

    /* 3. Direct creation of the file entry in the target directory (Isolated scope) */
    uint32_t target_sec;
    int free_entry;
    if (!find_free_entry(&target_sec, &free_entry)) {
        kprintf("Error: No space left in destination directory.\n");
        current_dir_cluster = old_cluster;
        current_dir_sector = old_sector;
        return 0;
    }

    {
        uint8_t* dir_sector = (uint8_t*)kmalloc(512);
        ata_read_sector(target_sec, dir_sector);
        fat_dir_entry_t* dir = (fat_dir_entry_t*)dir_sector;

        for (int i = 0; i < 11; i++) {
            dir[free_entry].name[i] = dest_fat[i];
        }
        dir[free_entry].attributes = 0x20; /* Archive (file) */
        dir[free_entry].first_cluster_low = 0;
        dir[free_entry].size = 0;
        dir[free_entry].sec_byte = (get_active_uid() & 0x0F) | 0x30; /* Secured by default to 0x30 */

        ata_write_sector(target_sec, dir_sector);
        kfree(dir_sector);
    }
    
    fat_dir_entry_t dest_entry;
    uint32_t dest_dir_sec;
    int dest_dir_idx;
    int dest_found = find_entry(dest_fat, &dest_entry, &dest_dir_sec, &dest_dir_idx);

    current_dir_cluster = old_cluster;
    current_dir_sector = old_sector;

    if (!dest_found) {
        kprintf("Error: Failed to locate created destination file.\n");
        return 0;
    }

    uint32_t src_cluster = src_entry.first_cluster_low;
    if (src_cluster == 0) {
        return 1; /* Empty file copied successfully */
    }

    uint8_t* cluster_buf = (uint8_t*)kmalloc(512 * sectors_per_cluster);

    uint32_t last_dest_cluster = 0;
    uint32_t first_dest_cluster = 0;

    while (src_cluster >= 2 && src_cluster < 0xFFF8) {
        uint32_t src_sec_lba = data_start + ((src_cluster - 2) * sectors_per_cluster);
        for (int s = 0; s < sectors_per_cluster; s++) {
            ata_read_sector(src_sec_lba + s, cluster_buf + (s * 512));
        }

        /* Find a free cluster */
        uint32_t free_cluster = 0;
        for (int i = 2; i < 256; i++) {
            if (read_fat_entry(i) == 0x0000) {
                free_cluster = i;
                write_fat_entry(i, 0xFFFF);
                break;
            }
        }

        if (free_cluster == 0) {
            kprintf("Error: Disk full during copy.\n");
            if (first_dest_cluster != 0) {
                free_fat_chain(first_dest_cluster);
            }
            kfree(cluster_buf);
            return 0;
        }

        uint32_t dest_sec_lba = data_start + ((free_cluster - 2) * sectors_per_cluster);
        for (int s = 0; s < sectors_per_cluster; s++) {
            ata_write_sector(dest_sec_lba + s, cluster_buf + (s * 512));
        }

        if (last_dest_cluster != 0) {
            write_fat_entry(last_dest_cluster, free_cluster);
        } else {
            first_dest_cluster = free_cluster;
        }
        last_dest_cluster = free_cluster;

        src_cluster = read_fat_entry(src_cluster);
    }

    /* Update the destination entry (Isolated scope) */
    {
        uint8_t* dir_sector = (uint8_t*)kmalloc(512);
        ata_read_sector(dest_dir_sec, dir_sector);
        fat_dir_entry_t* dir = (fat_dir_entry_t*)dir_sector;
        
        dir[dest_dir_idx].first_cluster_low = (uint16_t)first_dest_cluster;
        dir[dest_dir_idx].size = src_entry.size;
        dir[dest_dir_idx].sec_byte = (current_uid & 0x0F) | 0x30; /* Secured by default to 0x30 */
        
        ata_write_sector(dest_dir_sec, dir_sector);
        kfree(dir_sector);
    }

    kfree(cluster_buf);
    kprintf("File copied successfully (%d bytes).\n", src_entry.size);
    return 1;
}

/* Secure file movement */
void fs_mv(const char* src_name, const char* dest_name) {
    /* The source file is only deleted if the copy succeeded (returns 1) */
    if (fs_cp(src_name, dest_name)) {
        uint32_t src_parent_cluster, src_parent_sector;
        char src_fat[11];
        if (resolve_path(src_name, &src_parent_cluster, &src_parent_sector, src_fat)) {
            uint32_t old_cluster = current_dir_cluster;
            uint32_t old_sector = current_dir_sector;
            current_dir_cluster = src_parent_cluster;
            current_dir_sector = src_parent_sector;
            
            fs_rm(src_fat, NULL); /* Delete the original */
            
            current_dir_cluster = old_cluster;
            current_dir_sector = old_sector;
        }
    } else {
        kprintf("Error: Move failed.\n");
    }
}

int fs_load_file(const char* filename, uint8_t** buffer_out, uint32_t* size_out) {
    fs_error = FS_ERR_NONE; /* Reset the error */
    
    uint32_t parent_cluster, parent_sector;
    char target_fat[11];
    if (!resolve_path(filename, &parent_cluster, &parent_sector, target_fat)) {
        fs_error = FS_ERR_NOT_FOUND;
        return 0;
    }

    uint32_t old_cluster = current_dir_cluster;
    uint32_t old_sector = current_dir_sector;
    current_dir_cluster = parent_cluster;
    current_dir_sector = parent_sector;

    fat_dir_entry_t entry;
    int found = find_entry(target_fat, &entry, NULL, NULL);

    if (!found || (entry.attributes & 0x10)) {
        fs_error = FS_ERR_NOT_FOUND;
        current_dir_cluster = old_cluster;
        current_dir_sector = old_sector;
        return 0;
    }
    
    /* SILENT PERMISSION CHECK */
    if (!check_permission_byte(entry.sec_byte, 0)) {
        fs_error = FS_ERR_PERMISSION; /* <--- Raise a permission error */
        current_dir_cluster = old_cluster;
        current_dir_sector = old_sector;
        return 0; /* Fail silently */
    }

    *size_out = entry.size;
    *buffer_out = (uint8_t*)kmalloc(entry.size);
    
    uint32_t bytes_left = entry.size;
    uint32_t cluster = entry.first_cluster_low;
    uint8_t* sector = (uint8_t*)kmalloc(512);
    
    uint8_t* write_ptr = *buffer_out;

    while (bytes_left > 0 && cluster >= 2 && cluster < 0xFFF8) {
        uint32_t f_sec = data_start + ((cluster - 2) * sectors_per_cluster);
        for (int s = 0; s < sectors_per_cluster && bytes_left > 0; s++) {
            ata_read_sector(f_sec + s, sector);
            uint32_t chunk = (bytes_left > 512) ? 512 : bytes_left;
            for(uint32_t j=0; j<chunk; j++) *(write_ptr++) = sector[j];
            bytes_left -= chunk;
        }
        cluster = read_fat_entry(cluster);
    }
    kfree(sector);

    current_dir_cluster = old_cluster;
    current_dir_sector = old_sector;
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
        if (cluster == 0) {
            if (sec_offset >= (root_dir_entries * 32 / 512)) break;
            sec = root_dir_start + sec_offset;
        } else {
            if (sec_offset % sectors_per_cluster == 0) {
                cluster = read_fat_entry(cluster);
                if (cluster < 2 || cluster >= 0xFFF8) break;
                sec = data_start + ((cluster - 2) * sectors_per_cluster);
            } else sec++;
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
                target_cluster = 0;
                target_sector = root_dir_start;
            } else {
                uint32_t saved_cluster = current_dir_cluster;
                uint32_t saved_sector = current_dir_sector;
                current_dir_cluster = parent_cluster;
                current_dir_sector = parent_sector;

                fat_dir_entry_t entry;
                int found = find_entry(target_fat, &entry, NULL, NULL);

                current_dir_cluster = saved_cluster;
                current_dir_sector = saved_sector;

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

                target_cluster = entry.first_cluster_low;
                target_sector = (target_cluster == 0) ? root_dir_start : data_start + ((target_cluster - 2) * sectors_per_cluster);
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
                        kprintf("%s/", dir_path);
                    }
                    kprintf("%s  ", printable_name);
                }
                if (match_count == 1) {
                    /* Reconstructing the full path for user autocomplete */
                    int idx = 0;
                    if (dir_path[0] != '\0') {
                        while (dir_path[idx]) {
                            out_match[idx] = dir_path[idx];
                            idx++;
                        }
                        if (idx > 0 && dir_path[idx - 1] != '/') {
                            out_match[idx++] = '/';
                        }
                    }
                    int f_idx = 0;
                    while (printable_name[f_idx]) {
                        out_match[idx++] = printable_name[f_idx++];
                    }
                    out_match[idx] = '\0';
                }
            }
        }
        
        sec_offset++;
        if (cluster == 0) { 
            if (sec_offset >= (root_dir_entries * 32 / 512)) break; 
            sec = root_dir_start + sec_offset; 
        } 
        else {
            if (sec_offset % sectors_per_cluster == 0) {
                cluster = read_fat_entry(cluster);
                if (cluster < 2 || cluster >= 0xFFF8) break; 
                sec = data_start + ((cluster - 2) * sectors_per_cluster);
            } else sec++;
        }
    }
end_complete:
    kfree(sector);
    *out_match_count = match_count;
    return match_count;
}

/* Append text to an existing file (limited to 512 bytes of log) */
void fs_append(const char* filename, const char* text) {
    bypass_dir_permission = 1; /* Enable temporary write permission in the parent directory */
    
    uint8_t* buf = NULL;
    uint32_t size = 0;
    
    char temp_buf[512];
    int temp_idx = 0;
    
    if (fs_load_file(filename, &buf, &size)) {
        uint32_t to_copy = (size > 511) ? 511 : size;
        for (uint32_t i = 0; i < to_copy; i++) {
            temp_buf[temp_idx++] = buf[i];
        }
        kfree(buf);
    }
    
    for (int i = 0; text[i] != '\0' && temp_idx < 511; i++) {
        temp_buf[temp_idx++] = text[i];
    }
    temp_buf[temp_idx] = '\0';
    
    fs_write(filename, temp_buf);
    
    bypass_dir_permission = 0; /* Immediately restore default security */
}