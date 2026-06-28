#ifndef SYSTEM_H
#define SYSTEM_H

#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>

/* --- I/O Functions (Hardware) --- */
static inline void outb(uint16_t port, uint8_t val) {
    __asm__ volatile ( "outb %0, %1" : : "a"(val), "Nd"(port) );
}
static inline uint8_t inb(uint16_t port) {
    uint8_t ret;
    __asm__ volatile ( "inb %1, %0" : "=a"(ret) : "Nd"(port) );
    return ret;
}
static inline uint16_t inw(uint16_t port) {
    uint16_t ret;
    __asm__ volatile ("inw %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}
static inline void outw(uint16_t port, uint16_t val) {
    __asm__ volatile ("outw %0, %1" : : "a"(val), "Nd"(port));
}
static inline uint32_t inl(uint16_t port) {
    uint32_t ret;
    __asm__ volatile ( "inl %1, %0" : "=a"(ret) : "Nd"(port) );
    return ret;
}
static inline void outl(uint16_t port, uint32_t val) {
    __asm__ volatile ( "outl %0, %1" : : "a"(val), "Nd"(port) );
}
void ata_read_sector(uint32_t lba, uint8_t *buffer);

/* --- Tools (Utilities) --- */
int strcmp(const char *s1, const char *s2);
int fs_suggest_file(const char* target_name, char* out_suggestion);

/* --- Display (print.c) --- */
void clear_terminal();
void kprintf(const char* format, ...);
void kputc(char c);
void init_graphics(uint32_t phys_addr, uint32_t width, uint32_t height, uint32_t pitch, uint8_t bpp);
void put_pixel(uint32_t x, uint32_t y, uint32_t color);
void swap_buffers(void);
extern int is_graphics_mode;
void draw_char(char c, uint32_t x, uint32_t y, uint32_t fg_color, uint32_t bg_color);
void draw_string(const char* str, uint32_t x, uint32_t y, uint32_t fg_color, uint32_t bg_color);

/* --- Descriptors (gdt_idt.c) --- */
void init_gdt();
void init_idt();
void set_kernel_stack(uint32_t stack);

/* --- Memory --- */
extern uint32_t total_ram_kb;
extern uint32_t kernel_page_directory[1024];
void init_pmm(uint32_t ram_kb);
void* pmm_alloc_page();
void pmm_free_page(void* ptr);
void init_paging();
void init_heap();
void* kmalloc(size_t size);
void kfree(void* ptr);
void map_page_pd(uint32_t* pd, uint32_t virt, uint32_t phys);
void map_page(uint32_t virt, uint32_t phys);
int is_valid_user_ptr(uint32_t ptr);
int is_valid_user_range(uint32_t ptr, uint32_t size);
int is_valid_user_string(const char* str);  
void cleanup_task_memory(int id);

/* --- Hardware (kernel.c) --- */
extern volatile uint32_t timer_ticks;
void init_pic();
void init_pit();
uint32_t syscall_handler(uint32_t syscall_num, uint32_t arg1, uint32_t arg2, uint32_t arg3);

/* --- Disk & File System (disk.c / fs.c) --- */
void ata_read_sector(uint32_t lba, uint8_t *buffer);
void init_fs();
void fs_ls();
void fs_cd(const char* dirname);
void fs_cat(const char* filename);
void ata_write_sector(uint32_t lba, uint8_t *buffer);
void fs_mkdir(const char* dirname);
void fs_mkdir(const char* dirname);
void fs_rm(const char* arg1, const char* arg2);
void fs_touch(const char* filename);
int fs_write(const char* filename, const char* text);
int fs_load_file(const char* filename, uint8_t** buffer_out, uint32_t* size_out);
const char* get_current_path();
void fs_df();
void run_nano(const char* filename);
int fs_complete(const char* prefix, char* out_match, int* out_match_count, int print_matches);
int fs_cp(const char* src_name, const char* dest_name);
void fs_mv(const char* src_name, const char* dest_name);
void fs_append(const char* filename, const char* text);

/* --- User System (Auth & Permissions) --- */
extern uint8_t current_uid;
extern char current_user[32];
int check_permission_byte(uint8_t sec_byte, int is_write);
void fs_chown(const char* filename, uint8_t new_uid);
void fs_chmod(const char* filename, uint8_t new_perms);
int get_user_info(const char* username, char* out_password, uint8_t* out_uid);
void kernel_gets(char* buffer, int max_len, int hidden, int allow_empty);
uint8_t get_active_uid(void);

/* --- File System Error Codes --- */
extern int fs_error;
#define FS_ERR_NONE 0
#define FS_ERR_NOT_FOUND 1
#define FS_ERR_PERMISSION 2

/* --- Multitasking --- */
#define MAX_TASKS 8
#define MAX_OPEN_FILES 16

/* --- File descriptor types (VFS) --- */
#define TYPE_NONE 0
#define TYPE_TTY  1
#define TYPE_FILE 2
#define TYPE_SOCKET 3

typedef struct {
    uint8_t dest_ip[4];
    uint16_t dest_port;
    uint8_t* payload;
    uint32_t len;
} sendto_args_t;

typedef struct {
    int is_used;
    int type;
    uint32_t offset;
} file_descriptor_t;

typedef struct {
    uint32_t esp;
    uint32_t kernel_esp;
    uint32_t id;
    volatile uint8_t state; 
    uint8_t uid;
    uint8_t is_background;
    
    uint32_t* page_directory;
    void* code_page;
    void* user_stack;
    void* kernel_stack;
    uint32_t prog_break;
    file_descriptor_t open_files[MAX_OPEN_FILES];
} task_t;

#endif