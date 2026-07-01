#include "system.h"
#include "elf.h"
#include "pci.h"
#include "net.h"
#include "e1000.h"
#include "crypto.h" 

#define SOS_VERSION "2.4.2"

extern int fs_load_file(const char*, uint8_t**, uint32_t*);

/* Forward declarations to fix the implicit declaration warnings */
void custom_strcpy(char* dest, const char* src);
void logout_and_login(const char* message);
void execute_command(void);

void user_program() {
    __asm__ volatile ("mov $1, %eax; int $0x80");
    __asm__ volatile ("cli");
    while(1);
}

/* --- GLOBAL SECURITY AND INPUT VARIABLES --- */
uint8_t current_uid = 0;
char current_user[32] = "root";

volatile int is_kernel_gets_active = 0;
volatile char kernel_gets_char = 0;
volatile int cmd_ready = 0;
volatile int ctrl_active = 0;
volatile int is_logging_out = 0;
volatile int shell_input_enabled = 1;
volatile int session_reset = 0; 

char cmd_buffer[256];
int cmd_idx = 0;

/* --- HISTORY AND INTERACTIVE EDITING --- */
#define HISTORY_MAX 5
char cmd_history[HISTORY_MAX][256];
int history_count = 0;
int history_index = -1;
char current_input_save[256];     /* Save the command currently being written before using arrow keys */
volatile int cursor_pos = 0;      /* Current cursor position in cmd_buffer (independent of cmd_idx) */

/* --- BASIC SYSTEM COMMANDS --- */
const char* system_commands[] = {
    "help", "clear", "uptime", "ram", "ls", "cd", 
    "mkdir", "rm", "touch", "write", "cat", "loop", 
    "run", "ps", "kill", "df", "about", "shutdown",
    "whoami", "chown", "chmod", "su", "sudo", "userlist",
    "nano", "passwd", "cp", "mv", "hex", "ifconfig", "wget",
    "update"
};
unsigned int num_system_commands = sizeof(system_commands) / sizeof(system_commands[0]);

void init_task_fds(task_t* task) {
    /* Step 1: Set all slots to zero (close everything by default) */
    for (int i = 0; i < MAX_OPEN_FILES; i++) {
        task->open_files[i].is_used = 0;
        task->open_files[i].type = TYPE_NONE;
        task->open_files[i].offset = 0;
    }
    
    /* Step 2: Configure standard streams (stdin, stdout, stderr) to the Terminal */
    // FD 0: stdin (Standard Input)
    task->open_files[0].is_used = 1;
    task->open_files[0].type = TYPE_TTY;

    // FD 1: stdout (Standard Output)
    task->open_files[1].is_used = 1;
    task->open_files[1].type = TYPE_TTY;

    // FD 2: stderr (Standard Error)
    task->open_files[2].is_used = 1;
    task->open_files[2].type = TYPE_TTY;
}

/* Add a validated command to the history (5-element FIFO method) */
void history_add(const char* cmd) {
    if (cmd[0] == '\0') return;
    
    /* Avoids saving two times in a row the same command */
    if (history_count > 0 && strcmp(cmd_history[history_count - 1], cmd) == 0) {
        return;
    }
    
    if (history_count < HISTORY_MAX) {
        custom_strcpy(cmd_history[history_count], cmd);
        history_count++;
    } else {
        /* Shift the history to the left to make room */
        for (int i = 0; i < HISTORY_MAX - 1; i++) {
            custom_strcpy(cmd_history[i], cmd_history[i + 1]);
        }
        custom_strcpy(cmd_history[HISTORY_MAX - 1], cmd);
    }
}

/* Cleanly replace the line currently being edited with a history line */
void replace_cmd_buffer(const char* new_str) {
    /* 1. Move the cursor to the far right to align it with the end of the line */
    int shift_to_end = cmd_idx - cursor_pos;
    for (int i = 0; i < shift_to_end; i++) {
        kputc(cmd_buffer[cursor_pos + i]);
    }
    /* 2. Deletes previous terminal line */
    for (int i = 0; i < cmd_idx; i++) {
        kputc('\b'); kputc(' '); kputc('\b');
    }
    /* 3. Copies the new command */
    custom_strcpy(cmd_buffer, new_str);
    cmd_idx = 0;
    while (cmd_buffer[cmd_idx]) cmd_idx++;
    cursor_pos = cmd_idx;
    /* 4. Displays the new command */
    for (int i = 0; i < cmd_idx; i++) {
        kputc(cmd_buffer[i]);
    }
}

/* --- STRING TOOLS --- */
void custom_strcpy(char* dest, const char* src) { 
    while((*dest++ = *src++)); 
}

int strcmp(const char *s1, const char *s2) {
    while (*s1 && (*s1 == *s2)) { s1++; s2++; }
    return *(const unsigned char*)s1 - *(const unsigned char*)s2;
}

struct multiboot_info {
    uint32_t flags; uint32_t mem_lower; uint32_t mem_upper;
    uint32_t boot_device; uint32_t cmdline; uint32_t mods_count; uint32_t mods_addr;
    uint32_t syms[4]; uint32_t mmap_length; uint32_t mmap_addr;
    uint32_t drives_length; uint32_t drives_addr; uint32_t config_table;
    uint32_t boot_loader_name; uint32_t apm_table;
    uint32_t vbe_control_info; uint32_t vbe_mode_info; uint16_t vbe_mode;
    uint16_t vbe_interface_seg; uint16_t vbe_interface_off; uint16_t vbe_interface_len;
    /* Framebuffer info (GRUB Multiboot 1 extension) */
    uint64_t framebuffer_addr;
    uint32_t framebuffer_pitch;
    uint32_t framebuffer_width;
    uint32_t framebuffer_height;
    uint8_t framebuffer_bpp;
    uint8_t framebuffer_type;
    uint8_t color_info[6];
} __attribute__((packed));

/* --- MULTITASKING (Context Switching) --- */
task_t tasks[MAX_TASKS];
int current_task = -1;

uint8_t get_active_uid() {
    if (current_task == -1) return current_uid; /* Security fallback on startup */
    if (current_task == 0) {
        return current_uid; /* Shell (PID 0) always follows active session's UID */
    }
    return tasks[current_task].uid; /* Other tasks use their creator's UID */
}

void init_multitasking() {
    for(int i = 0; i < MAX_TASKS; i++) tasks[i].state = 0;
    
    tasks[0].id = 0;
    tasks[0].state = 1;
    tasks[0].uid = 0; /* The original shell belongs to root */
    tasks[0].page_directory = kernel_page_directory;
    tasks[0].is_background = 0;
    current_task = 0;

    init_task_fds(&tasks[0]);
}

uint32_t schedule(uint32_t current_esp) {
    if (current_task == -1) return current_esp;

    if (tasks[current_task].state == 1) {
        tasks[current_task].esp = current_esp;
        tasks[current_task].state = 2; 
    }

    int next_task = current_task;
    do {
        next_task = (next_task + 1) % MAX_TASKS;
    } while (tasks[next_task].state != 2); 

    if (current_task != next_task) {
        current_task = next_task;
        
        /* Hardware address space switch */
        uint32_t new_pd = (uint32_t)tasks[current_task].page_directory;
        uint32_t current_cr3;
        __asm__ volatile("mov %%cr3, %0" : "=r"(current_cr3));
        if (current_cr3 != new_pd) {
            __asm__ volatile("mov %0, %%cr3" :: "r"(new_pd));
        }

        set_kernel_stack(tasks[current_task].kernel_esp);
    }

    tasks[current_task].state = 1;
    return tasks[current_task].esp;
}

void cleanup_task_memory(int id) {
    /* [FIX ADDITION] Close and release all open file descriptors / sockets */
    for (int i = 0; i < MAX_OPEN_FILES; i++) {
        if (tasks[id].open_files[i].is_used) {
            
            /* If the descriptor was a socket, release it in the global network stack */
            if (tasks[id].open_files[i].type == TYPE_SOCKET) {
                int sock_idx = tasks[id].open_files[i].offset;
                extern socket_t sockets[];
                if (sock_idx >= 0 && sock_idx < MAX_SOCKETS) {
                    sockets[sock_idx].is_used = 0;        /* Release socket slot */
                    sockets[sock_idx].tcp_state = 0;      /* Force TCP_CLOSED */
                    sockets[sock_idx].rx_len = 0;         /* Flush buffers */
                    sockets[sock_idx].rx_ready = 0;
                }
            }
            
            /* Mark task file descriptor as closed */
            tasks[id].open_files[i].is_used = 0;
            tasks[id].open_files[i].type = TYPE_NONE;
            tasks[id].open_files[i].offset = 0;
        }
    }

    if (tasks[id].page_directory != kernel_page_directory && tasks[id].page_directory != NULL) {
        uint32_t* pd = tasks[id].page_directory;
        
        /* [FIX MODIFICATION] Change upper limit from 1024 to 64. 
           Indices 31 to 63 are User Space. Index 64+ is Kernel Space */
        for (int i = 31; i < 64; i++) {
            if (pd[i] & 0x01) {
                uint32_t* pt = (uint32_t*)(pd[i] & 0xFFFFF000);
                for (int j = 0; j < 1024; j++) {
                    if (pt[j] & 0x01) pmm_free_page((void*)(pt[j] & 0xFFFFF000));
                }
                pmm_free_page((void*)pt);
            }
        }
        pmm_free_page(pd);
    } else {
        if (tasks[id].code_page) pmm_free_page(tasks[id].code_page);
        if (tasks[id].user_stack) pmm_free_page(tasks[id].user_stack);
    }
    if (tasks[id].kernel_stack) pmm_free_page(tasks[id].kernel_stack);
    
    tasks[id].page_directory = NULL;
    tasks[id].state = 0;
}

void kill_current_task() {
    cleanup_task_memory(current_task);
    while(1) {
        __asm__ volatile("sti; hlt");
    }
}

void create_task(void (*entry_point)()) {
    int id = -1;
    for(int i = 1; i < MAX_TASKS; i++) {
        if (tasks[i].state == 0) { id = i; break; }
    }
    if (id == -1) { kprintf("Error: Max threads reached.\n"); return; }

    uint32_t* stack = (uint32_t*)pmm_alloc_page();
    uint32_t* top = (uint32_t*)((uint32_t)stack + 4096);

    *(--top) = 0x202;                
    *(--top) = 0x08;                 
    *(--top) = (uint32_t)entry_point;

    *(--top) = 0; *(--top) = 0; *(--top) = 0; *(--top) = 0;
    *(--top) = 0; *(--top) = 0; *(--top) = 0; *(--top) = 0;

    *(--top) = 0x10; *(--top) = 0x10; *(--top) = 0x10; *(--top) = 0x10;

    tasks[id].esp = (uint32_t)top;
    tasks[id].id = id;
    tasks[id].uid = current_uid; /* Saves the creator */
    tasks[id].page_directory = kernel_page_directory;
    init_task_fds(&tasks[id]);
    tasks[id].state = 2; 
    tasks[id].is_background = 0;
}

void init_pic() {
    outb(0x20, 0x11); outb(0xA0, 0x11);
    outb(0x21, 0x20); outb(0xA1, 0x28);
    outb(0x21, 0x04); outb(0xA1, 0x02);
    outb(0x21, 0x01); outb(0xA1, 0x01);
    outb(0x21, 0xEC); outb(0xA1, 0xFF); 
}

volatile uint32_t timer_ticks = 0;
void init_pit() {
    uint32_t divisor = 1193180 / 100;
    outb(0x43, 0x36);
    outb(0x40, (uint8_t)(divisor & 0xFF));
    outb(0x40, (uint8_t)((divisor >> 8) & 0xFF));
}

uint32_t timer_handler(uint32_t esp) {
    timer_ticks++;
    outb(0x20, 0x20); 
    return schedule(esp);
}

/* --- KEYBOARD & USER ENTRIES --- */

const char kbd_us_shift[128] = {
    0,  27, '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+', '\b',
  '\t', 'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', '\n',
    0,  'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', '"', '~',
    0,  '|', 'Z', 'X', 'C', 'V', 'B', 'N', 'M', '<', '>', '?', 0, '*',
    0,  ' ', 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, '-', 0, 0, 0, '+', 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
};

const char kbd_us[128] = {
    0,  27, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', '\b',
  '\t', 'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\n',
    0,  'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`',
    0, '\\', 'z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '/', 0, '*',
    0,  ' ', 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, '-', 0, 0, 0, '+', 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
};

extern void kputc(char);

/* Secure synchronous read */
void kernel_gets(char* buffer, int max_len, int hidden, int allow_empty) {
    int i = 0;
    is_kernel_gets_active = 1;
    while (i < max_len - 1) {
        kernel_gets_char = 0;
        while (kernel_gets_char == 0) { __asm__ volatile("hlt"); }
        char c = kernel_gets_char;
        
        if (c == '\r' || c == '\n') { 
            if (!allow_empty && i == 0) continue; 
            kprintf("\n"); break; 
        } 
        else if (c == '\b' || c == 127) { 
            if (i > 0) { i--; kprintf("\b \b"); } 
        } else if (c >= 32 && c <= 126) {
            buffer[i++] = c;
            if (hidden) kprintf("*"); else kprintf("%c", c);
        }
    }
    buffer[i] = '\0';
    is_kernel_gets_active = 0;
}

void handle_input_char(char c) {
    /* Simple state machine to detect consecutive double tabs */
    static int last_key_was_tab = 0;
    int is_double_tab = (c == 9 && last_key_was_tab);
    if (c != 9) {
        last_key_was_tab = 0;
    } else {
        last_key_was_tab = 1;
    }

    if (c == '\r' || c == '\n') { 
        kprintf("\n"); 
        cmd_buffer[cmd_idx] = '\0'; 
        
        history_add(cmd_buffer);
        history_index = -1; 
        
        cmd_ready = 1; 
    } 
    else if (c == '\b' || c == 127) { 
        if (cursor_pos > 0) { 
            cursor_pos--;
            /* Shift the remaining characters to the left */
            for (int i = cursor_pos; i < cmd_idx - 1; i++) {
                cmd_buffer[i] = cmd_buffer[i + 1];
            }
            cmd_idx--;
            cmd_buffer[cmd_idx] = '\0'; /* Maintain the null terminator after backspace */
            
            /* Updates display */
            kputc('\b');
            for (int i = cursor_pos; i < cmd_idx; i++) {
                kputc(cmd_buffer[i]);
            }
            kputc(' '); 
            
            /* Reposition the cursor at the deletion point */
            int shift_back = cmd_idx + 1 - cursor_pos;
            for (int i = 0; i < shift_back; i++) {
                kputc('\b');
            }
        } 
    }
    else if (c == 9) { /* Tabulation Key */
        int word_start = cursor_pos;
        while (word_start > 0 && cmd_buffer[word_start - 1] != ' ') {
            word_start--;
        }
        
        char prefix[64];
        int prefix_len = cursor_pos - word_start;
        if (prefix_len > 63) prefix_len = 63;
        for (int i = 0; i < prefix_len; i++) {
            prefix[i] = cmd_buffer[word_start + i];
        }
        prefix[prefix_len] = '\0';

        if (prefix_len == 0) {
            last_key_was_tab = 0; 
            return; 
        }

        if (is_double_tab) {
            last_key_was_tab = 0;
        }

        int match_count = 0;
        char single_match[64] = {0};

        if (word_start == 0) {
            for (unsigned int i = 0; i < num_system_commands; i++) {
                int is_match = 1;
                for (int j = 0; j < prefix_len; j++) {
                    if (system_commands[i][j] != prefix[j]) {
                        is_match = 0;
                        break;
                    }
                }
                if (is_match) {
                    match_count++;
                    if (match_count == 1) {
                        custom_strcpy(single_match, system_commands[i]);
                    }
                }
            }

            if (is_double_tab && match_count > 0) {
                kprintf("\n");
                for (unsigned int i = 0; i < num_system_commands; i++) {
                    int is_match = 1;
                    for (int j = 0; j < prefix_len; j++) {
                        if (system_commands[i][j] != prefix[j]) {
                            is_match = 0;
                            break;
                        }
                    }
                    if (is_match) {
                        kprintf("%s  ", system_commands[i]);
                    }
                }
                kprintf("\n");
                kprintf("\033[32m%s@sOS\033[0m \033[34m%s\033[0m\033[33m%c\033[0m %s", current_user, get_current_path(), current_uid == 0 ? '#' : '$', cmd_buffer);
                int shift_back = cmd_idx - cursor_pos;
                for (int i = 0; i < shift_back; i++) {
                    kputc('\b');
                }
            }
        } else {
            fs_complete(prefix, single_match, &match_count, 0);
            
            if (match_count > 0 && is_double_tab) {
                kprintf("\n");
                fs_complete(prefix, single_match, &match_count, 1); 
                kprintf("\n");
                kprintf("\033[32m%s@sOS\033[0m \033[34m%s\033[0m\033[33m%c\033[0m %s", current_user, get_current_path(), current_uid == 0 ? '#' : '$', cmd_buffer);
                int shift_back = cmd_idx - cursor_pos;
                for (int i = 0; i < shift_back; i++) {
                    kputc('\b');
                }
            }
        }

        if (match_count == 1 && !is_double_tab) {
            char* suffix = &single_match[prefix_len];
            int suffix_len = 0;
            while (suffix[suffix_len]) suffix_len++;
            
            if (cmd_idx + suffix_len < 255) {
                for (int i = cmd_idx + suffix_len - 1; i >= cursor_pos + suffix_len; i--) {
                    cmd_buffer[i] = cmd_buffer[i - suffix_len];
                }
                for (int i = 0; i < suffix_len; i++) {
                    cmd_buffer[cursor_pos + i] = suffix[i];
                }
                cmd_idx += suffix_len;
                cursor_pos += suffix_len;
                cmd_buffer[cmd_idx] = '\0'; /* Maintain the null terminator after autocomplete */
                
                for (int i = cursor_pos - suffix_len; i < cmd_idx; i++) {
                    kputc(cmd_buffer[i]);
                }
                int shift_back = cmd_idx - cursor_pos;
                for (int i = 0; i < shift_back; i++) {
                    kputc('\b');
                }
            }
        }
    }
    else if (c == 17) { /* Up Arrow Key */
        if (history_count > 0) {
            if (history_index == -1) {
                custom_strcpy(current_input_save, cmd_buffer);
                history_index = history_count - 1;
            } else if (history_index > 0) {
                history_index--;
            }
            replace_cmd_buffer(cmd_history[history_index]);
        }
    }
    else if (c == 18) { /* Down Arrow Key */
        if (history_index != -1) {
            if (history_index < history_count - 1) {
                history_index++;
                replace_cmd_buffer(cmd_history[history_index]);
            } else {
                history_index = -1;
                replace_cmd_buffer(current_input_save);
            }
        }
    }
    else if (c == 20) { /* Left Arrow Key */
        if (cursor_pos > 0) {
            cursor_pos--;
            kputc('\b');
        }
    }
    else if (c == 21) { /* Right Arrow Key */
        if (cursor_pos < cmd_idx) {
            kputc(cmd_buffer[cursor_pos]);
            cursor_pos++;
        }
    }
    else if (cmd_idx < 255 && c >= 32 && c <= 126) { 
        for (int i = cmd_idx; i > cursor_pos; i--) {
            cmd_buffer[i] = cmd_buffer[i - 1];
        }
        cmd_buffer[cursor_pos] = c;
        cmd_idx++;
        cursor_pos++;
        cmd_buffer[cmd_idx] = '\0'; /* Maintain the null terminator after keyboard input */
        
        for (int i = cursor_pos - 1; i < cmd_idx; i++) {
            kputc(cmd_buffer[i]);
        }
        int shift_back = cmd_idx - cursor_pos;
        for (int i = 0; i < shift_back; i++) {
            kputc('\b');
        }
    }
}

volatile int shift_active = 0; /* Follows Shift state in RAM */

void keyboard_handler() {
    uint8_t scancode = inb(0x60);
    
    /* Detect left Ctrl key press (0x1D) */
    if (scancode == 0x1D) {
        ctrl_active = 1;
    } 
    else if (scancode == 0x9D) {
        ctrl_active = 0;
    }
    
    if (scancode == 0x2A || scancode == 0x36) {
        shift_active = 1;
    } 
    else if (scancode == 0xAA || scancode == 0xB6) { 
        shift_active = 0;
    }
    
    if (!(scancode & 0x80)) { 
        char ascii = 0;
        
        /* Direct mapping of physical keyboard arrow scancodes */
        if (scancode == 0x48) ascii = 17;      /* Up Arrow -> ASCII 17 */
        else if (scancode == 0x50) ascii = 18; /* Down Arrow -> ASCII 18 */
        else if (scancode == 0x4B) ascii = 20; /* Left Arrow -> ASCII 20 */
        else if (scancode == 0x4D) ascii = 21; /* Right Arrow -> ASCII 21 */
        else if (ctrl_active) {
            /* Ctrl Shortcuts */
            if (scancode == 0x1F) ascii = 19;  /* Ctrl+S */
            else if (scancode == 0x2D) ascii = 24; /* Ctrl+X */
        } else if (shift_active) {
            ascii = kbd_us_shift[scancode];
        } else {
            ascii = kbd_us[scancode];
        }
        
        if (ascii != 0) {
            if (is_kernel_gets_active) {
                kernel_gets_char = ascii;
            } else if (shell_input_enabled) {
                handle_input_char(ascii);
            }
        }
    }
    outb(0x20, 0x20); 
}

void serial_handler() {
    if (inb(0x3f8 + 5) & 0x01) {
        char c = inb(0x3f8);
        
        /* CRLF key sequence handling */
        static char last_char = 0;
        if (c == '\n' && last_char == '\r') {
            last_char = 0;
            outb(0x20, 0x20);
            return;
        }
        last_char = c;
        
        /* ANSI navigation sequence handling (PuTTY / Terminal) */
        /* WARNING: SHELL VIA SSH IS DEPRECATED AND WILL BE REMOVED IN FURTHER VERSIONS */
        static int escape_state = 0;
        if (c == 27) { escape_state = 1; outb(0x20, 0x20); return; } /* Escape */
        if (escape_state == 1 && c == '[') { escape_state = 2; outb(0x20, 0x20); return; }
        if (escape_state == 2) {
            escape_state = 0;
            if (c == 'A') c = 17;      /* Up Arrow */
            else if (c == 'B') c = 18; /* Down Arrow */
            else if (c == 'C') c = 21; /* Right Arrow */
            else if (c == 'D') c = 20; /* Left Arrow */
        } else {
            if (c == '\r') c = '\n';
        }
        
        if (is_kernel_gets_active) {
            kernel_gets_char = c;
        } else if (shell_input_enabled) {
            handle_input_char(c);
        }
    }
    outb(0x20, 0x20);
}

char wait_key(void) {
    is_kernel_gets_active = 1;
    kernel_gets_char = 0;
    while (kernel_gets_char == 0) { __asm__ volatile("hlt"); }
    char c = kernel_gets_char;
    is_kernel_gets_active = 0;
    return c;
}

void background_loop() {
    int counter = 0;
    uint32_t last_tick = timer_ticks;
    while(1) {
        if (timer_ticks >= last_tick + 300) {
            kprintf("\n[Background Task] J'existe ! Compteur: %d\n", counter++);
            kprintf("\033[32m%s@sOS\033[0m \033[34m%s\033[0m\033[33m%c\033[0m %s", current_user, get_current_path(), current_uid == 0 ? '#' : '$', cmd_buffer); 
            
            /* Reposition the interactive cursor if the user was typing in the middle of a word */
            int shift_back = cmd_idx - cursor_pos;
            for (int i = 0; i < shift_back; i++) {
                kputc('\b');
            }
            last_tick = timer_ticks;
        }
        __asm__ volatile("hlt");
    }
}

int create_user_task(uint8_t* elf_buffer, uint32_t elf_size) {
    (void)elf_size;

    /* 1. ELF Header Validation */
    Elf32_Ehdr* header = (Elf32_Ehdr*)elf_buffer;
    if (header->e_ident[0] != 0x7F || header->e_ident[1] != 'E' ||
        header->e_ident[2] != 'L' || header->e_ident[3] != 'F') {
        kprintf("Error: Invalid ELF magic!\n");
        return -1;
    }

    /* 2. Search for a free task slot */
    int id = -1;
    for(int i = 1; i < MAX_TASKS; i++) {
        if (tasks[i].state == 0) { id = i; break; }
    }
    if (id == -1) { kprintf("Error: Max threads reached.\n"); return -1; }

    /* 3. Allocation of system and user stacks */
    uint32_t* kstack = (uint32_t*)pmm_alloc_page();
    uint32_t* ustack = (uint32_t*)pmm_alloc_page();
    
    /* 4. Creation and initialization of the task's page directory */
    uint32_t* pd = (uint32_t*)pmm_alloc_page();
    for(int i = 0; i < 1024; i++) pd[i] = kernel_page_directory[i];

    tasks[id].page_directory = pd;
    tasks[id].user_stack = (void*)ustack;
    tasks[id].kernel_stack = (void*)kstack;

    /* 5. Load PT_LOAD segments */
    Elf32_Phdr* phdr = (Elf32_Phdr*)(elf_buffer + header->e_phoff);
    uint32_t max_vaddr = 0;

    for (int i = 0; i < header->e_phnum; i++) {
        if (phdr[i].p_type == PT_LOAD) {
            uint32_t vaddr = phdr[i].p_vaddr;
            uint32_t memsz = phdr[i].p_memsz;
            uint32_t filesz = phdr[i].p_filesz;
            uint32_t offset = phdr[i].p_offset;

            /* [FIX ADDITION] Prevent ELF segments from mapping into kernel space */
            if (vaddr >= 0x10000000 || vaddr + memsz > 0x10000000 || vaddr + memsz < vaddr) {
                kprintf("Error: ELF segment out of bounds or overlaps kernel space.\n");
                pmm_free_page(pd);
                pmm_free_page(ustack);
                pmm_free_page(kstack);
                return -1;
            }

            /* Calculate page alignment to map for this segment */
            uint32_t start_page = vaddr & 0xFFFFF000;
            uint32_t end_page = (vaddr + memsz + 4095) & 0xFFFFF000;

            /* Physical allocation and virtual mapping */
            for (uint32_t page = start_page; page < end_page; page += 4096) {
                void* phys = pmm_alloc_page();
                /* Initialize the new page to zero */
                for (int k = 0; k < 1024; k++) ((uint32_t*)phys)[k] = 0;
                map_page_pd(pd, page, (uint32_t)phys);
            }

            /*
             * TIP: To cleanly copy data to the target virtual address,
             * we temporarily switch to the new task's page directory. Since the
             * kernel is cloned into this directory, the operation is safe and there
             * is no risk of a crash.
            */
            uint32_t old_cr3;
            __asm__ volatile("mov %%cr3, %0" : "=r"(old_cr3));
            __asm__ volatile("mov %0, %%cr3" :: "r"(pd));

            /* Copy code/data from the ELF file */
            uint8_t* dest = (uint8_t*)vaddr;
            uint8_t* src = elf_buffer + offset;
            for (uint32_t j = 0; j < filesz; j++) {
                dest[j] = src[j];
            }
            
            /* BSS Initialisation (memsz - filesz) at zero */
            for (uint32_t j = filesz; j < memsz; j++) {
                dest[j] = 0;
            }

            /* Restore original memory space */
            __asm__ volatile("mov %0, %%cr3" :: "r"(old_cr3));

            if (vaddr + memsz > max_vaddr) {
                max_vaddr = vaddr + memsz;
            }
        }
    }

    /* 6. Map the user stack */
    uint32_t ustack_virt = 0x7FF0000;
    map_page_pd(pd, ustack_virt, (uint32_t)ustack);

    /* 7. Prepare the kernel stack (starting context for IRET) */
    uint32_t* top = (uint32_t*)((uint32_t)kstack + 4096);

    *(--top) = 0x23;                     
    *(--top) = ustack_virt + 4096;  
    *(--top) = 0x202;                    
    *(--top) = 0x1B;                     
    *(--top) = header->e_entry;  /* The starting address is the actual ELF entry point */

    for(int i = 0; i < 8; i++) *(--top) = 0;
    *(--top) = 0x23; *(--top) = 0x23; *(--top) = 0x23; *(--top) = 0x23;

    tasks[id].esp = (uint32_t)top;
    tasks[id].kernel_esp = (uint32_t)kstack + 4096;
    
    /*
     * prog_break (application heap limit): configured at the end
     * of the highest segment, rounded up to the next page.
     */
    tasks[id].prog_break = (max_vaddr + 4095) & 0xFFFFF000; 
    
    tasks[id].id = id;
    tasks[id].uid = current_uid;
    tasks[id].state = 2; 
    tasks[id].is_background = 0;

    init_task_fds(&tasks[id]); /* stdin/stdout/stderr Initalisation */

    return id; 
}

uint32_t sys_sbrk(int increment) {
    if (current_task == -1) return (uint32_t)-1;
    uint32_t old_break = tasks[current_task].prog_break;
    if (increment == 0) return old_break;

    uint32_t new_break = old_break + increment;

    /* [FIX ADDITION] Prevent user heap from growing into kernel space */
    if (new_break >= 0x10000000 || new_break < old_break) {
        return (uint32_t)-1;
    }

    uint32_t start_page = ((old_break - 1) / 4096) + 1;
    uint32_t end_page = (new_break - 1) / 4096;
    
    for (uint32_t i = start_page; i <= end_page; i++) {
        void* phys = pmm_alloc_page();
        if (!phys) return (uint32_t)-1;
        map_page_pd(tasks[current_task].page_directory, i * 4096, (uint32_t)phys);
    }
    
    tasks[current_task].prog_break = new_break;
    return old_break;
}

int is_valid_user_range(uint32_t ptr, uint32_t size) {
    if (current_task <= 0) return 1;
    if (tasks[current_task].page_directory == kernel_page_directory) return 1;
    
    // Protection against integer overflow during address addition
    if (ptr + size < ptr) return 0;
    
    uint32_t stack_start = 0x7FF0000;
    uint32_t stack_end   = 0x7FF1000; // Size: 4096 bytes
    uint32_t app_start   = 0x8000000;
    uint32_t app_end     = tasks[current_task].prog_break;
    
    // Case 1: The range lies entirely within the user stack space
    if (ptr >= stack_start && (ptr + size) <= stack_end) {
        return 1;
    }
    
    // Case 2: The range lies entirely within the user code/heap space
    if (ptr >= app_start && (ptr + size) <= app_end) {
        return 1;
    }
    
    return 0; // Out of bounds or prohibited overlap
}

int is_valid_user_string(const char* str) {
    if (current_task <= 0) return 1;
    if (tasks[current_task].page_directory == kernel_page_directory) return 1;
    
    uint32_t ptr = (uint32_t)str;
    uint32_t stack_start = 0x7FF0000;
    uint32_t stack_end   = 0x7FF1000;
    uint32_t app_start   = 0x8000000;
    uint32_t app_end     = tasks[current_task].prog_break;
    
    uint32_t limit = 0;
    if (ptr >= stack_start && ptr < stack_end) {
        limit = stack_end;
    } else if (ptr >= app_start && ptr < app_end) {
        limit = app_end;
    } else {
        return 0; // Starting pointer outside the allocated space
    }
    
    // Traverse the string while ensuring no overflow out of the identified segment
    while (ptr < limit) {
        if (*(char*)ptr == '\0') {
            return 1;
        }
        ptr++;
    }
    return 0; // Limit reached without a null terminator
}

int is_valid_user_ptr(uint32_t ptr) {
    return is_valid_user_range(ptr, 1);
}

int edit_distance_is_one(const char* s1, const char* s2) {
    int len1 = 0; while (s1[len1]) len1++;
    int len2 = 0; while (s2[len2]) len2++;

    int diff = len1 - len2;
    if (diff < -1 || diff > 1) return 0; 

    int i = 0, j = 0, edits = 0;
    while (i < len1 && j < len2) {
        if (s1[i] != s2[j]) {
            if (edits == 1) return 0; 
            edits = 1;
            if (len1 > len2) i++; 
            else if (len1 < len2) j++; 
            else { i++; j++; }
        } else { i++; j++; }
    }
    if (i < len1 || j < len2) edits++;
    return (edits == 1);
}

/* Ultra-fast non-cryptographic hash (FNV-1a) */
uint32_t fnv1a_hash(const char* str) {
    uint32_t hash = 0x811c9dc5;
    while (*str) {
        hash ^= (unsigned char)*str++;
        hash *= 0x01000193;
    }
    return hash;
}

/* Convert the 32-bit hash to an 8-character hexadecimal string */
void hash_to_str(uint32_t hash, char* out_str) {
    const char* hex = "0123456789abcdef";
    for (int i = 7; i >= 0; i--) {
        out_str[i] = hex[hash & 0xF];
        hash >>= 4;
    }
    out_str[8] = '\0';
}

/* [FIX ADDITION] Salt Generation */
void generate_salt(char* out_salt) {
    extern volatile uint32_t timer_ticks;
    uint32_t seed = timer_ticks ^ 0xDEADBEEF;
    const char* hex = "0123456789abcdef";
    for(int i=0; i<8; i++) {
        seed = (seed * 1103515245) + 12345; /* LCG PRNG */
        out_salt[i] = hex[(seed >> 16) & 0xF];
    }
    out_salt[8] = '\0';
}

/* [FIX ADDITION] Salted and stretched hashing */
void hash_password_salted(const char* password, const char* salt, char* out_hash_str) {
    char buffer[64];
    custom_strcpy(buffer, salt);
    int salt_len = 0; while(salt[salt_len]) salt_len++;
    int pass_len = 0; while(password[pass_len]) pass_len++;
    
    for(int i=0; i<pass_len && (salt_len + i < 63); i++) buffer[salt_len + i] = password[i];
    buffer[salt_len + pass_len] = '\0';
    
    /* Key stretching (10,000 iterations) to massively slow down brute-force */
    uint32_t hash = fnv1a_hash(buffer);
    for(int i = 0; i < 10000; i++) {
        hash ^= fnv1a_hash((char*)&hash);
    }
    hash_to_str(hash, out_hash_str);
}

/* [FIX MODIFICATION] Upgraded check_password supporting salts */
int check_password(const char* input_pass, const char* stored_str) {
    int has_salt = 0;
    for(int i=0; stored_str[i]; i++) {
        if (stored_str[i] == '$') { has_salt = i; break; }
    }
    
    if (has_salt) {
        char salt[16];
        for(int i=0; i<has_salt; i++) salt[i] = stored_str[i];
        salt[has_salt] = '\0';
        
        char computed_hash[9];
        hash_password_salted(input_pass, salt, computed_hash);
        
        return (strcmp(computed_hash, &stored_str[has_salt + 1]) == 0);
    } else {
        /* Backwards compatibility for old unsalted passwords */
        uint32_t hash = fnv1a_hash(input_pass);
        char hash_str[9];
        hash_to_str(hash, hash_str);
        return (strcmp(hash_str, stored_str) == 0);
    }
}

int get_user_info(const char* username, char* out_password, uint8_t* out_uid) {
    /* Temporary privilege elevation to ROOT (0) to read the passwd file */
    uint8_t old_uid = current_uid;
    current_uid = 0; 

    uint8_t* buf; uint32_t size;
    if (!fs_load_file("/passwd", &buf, &size)) {
        current_uid = old_uid;
        return 0;
    }
    current_uid = old_uid; /* Restore the original user's UID */
    
    uint32_t i = 0;
    while (i < size) {
        char u[32], p[32], id_str[8]; int ui=0, pi=0, idi=0;
        
        while(i < size && buf[i] != ':' && buf[i] != '\n') { if(ui<31) u[ui++] = buf[i]; i++; } u[ui] = '\0';
        if (i < size && buf[i] == ':') i++;
        while(i < size && buf[i] != ':' && buf[i] != '\n') { if(pi<31) p[pi++] = buf[i]; i++; } p[pi] = '\0';
        if (i < size && buf[i] == ':') i++;
        while(i < size && buf[i] != '\n') { if(idi<7) id_str[idi++] = buf[i]; i++; } id_str[idi] = '\0';
        if (i < size && buf[i] == '\n') i++;

        if (ui == 0) continue;

        if (strcmp(u, username) == 0) {
            custom_strcpy(out_password, p);
            int uid = 0; for(int k=0; k<idi; k++) uid = uid*10 + (id_str[k]-'0');
            *out_uid = (uint8_t)uid;
            kfree(buf); return 1;
        }
    }
    kfree(buf); return 0;
}

/* --- NANO LIGHT WITH SCROLLING AND DYNAMIC RESOLUTION --- */
#define NANO_MAX_LINES 1024
#define NANO_MAX_COLS  128  /* Increased to support 1024px width */
char edit_lines[NANO_MAX_LINES][NANO_MAX_COLS];
int edit_lens[NANO_MAX_LINES];
int edit_cx = 0;
int edit_cy = 0;
int scroll_y = 0; 

/* Expose graphics engine dimensions */
extern uint32_t g_width, g_height;

void draw_editor(const char* filename) {
    /* Dynamically calculate terminal size */
    int term_cols = is_graphics_mode ? (g_width / 8) : 80;
    int term_rows = is_graphics_mode ? (g_height / 16) : 25;
    int visible_lines = term_rows - 3; /* Header, Footer, and Status take 3 lines */

    if (term_cols > NANO_MAX_COLS) term_cols = NANO_MAX_COLS;

    /* 1. Format Header & Footer Strings to span the whole width */
    char header[129];
    for(int i = 0; i < term_cols; i++) header[i] = ' ';
    header[term_cols] = '\0';
    
    char* title = "  sOS Nano Light v3.1 -- File: ";
    int idx = 0;
    while(title[idx]) { header[idx] = title[idx]; idx++; }
    int f_idx = 0;
    while(filename[f_idx] && idx < term_cols - 5) { header[idx++] = filename[f_idx++]; }
    
    char footer[129];
    for(int i = 0; i < term_cols; i++) footer[i] = ' ';
    footer[term_cols] = '\0';
    
    char* f_text = "  ^S: Save    ^X: Quit    (Use Arrows to Scroll)";
    int f_text_idx = 0;
    while(f_text[f_text_idx]) { footer[f_text_idx] = f_text[f_text_idx]; f_text_idx++; }

    /* 2. Graphical Rendering Mode */
    if (is_graphics_mode) {
        uint32_t bg_normal = 0x00000000; /* Black */
        uint32_t fg_normal = 0x00FFFFFF; /* White */
        uint32_t bg_invert = 0x00AAAAAA; /* Gray */
        uint32_t fg_invert = 0x00000000; /* Black */

        /* Header */
        draw_string(header, 0, 0, fg_invert, bg_invert);

        /* Editing Area */
        for (int l = 0; l < visible_lines; l++) {
            for (int c = 0; c < term_cols; c++) {
                char char_to_draw = ' ';
                if (scroll_y + l < NANO_MAX_LINES && c < edit_lens[scroll_y + l]) {
                    char_to_draw = edit_lines[scroll_y + l][c];
                }
                draw_char(char_to_draw, c * 8, (l + 1) * 16, fg_normal, bg_normal);
            }
        }
        
        /* Clear Status Line */
        for (int c = 0; c < term_cols; c++) {
            draw_char(' ', c * 8, (term_rows - 2) * 16, fg_normal, bg_normal);
        }

        /* Footer */
        draw_string(footer, 0, (term_rows - 1) * 16, fg_invert, bg_invert);

        /* Draw Software Block Cursor */
        char char_under_cursor = ' ';
        if (edit_cy < NANO_MAX_LINES && edit_cx < edit_lens[edit_cy]) {
            char_under_cursor = edit_lines[edit_cy][edit_cx];
        }
        draw_char(char_under_cursor, edit_cx * 8, ((edit_cy - scroll_y) + 1) * 16, bg_normal, fg_normal);

        swap_buffers();
    } 
    /* 3. Fallback Legacy VGA Mode */
    else {
        uint16_t* vmem = (uint16_t*)0xB8000;
        for(int i = 0; i < 80; i++) vmem[i] = (uint16_t)header[i] | 0x7000;
        
        for (int l = 0; l < visible_lines; l++) {
            for (int c = 0; c < 80; c++) {
                char char_to_draw = ' ';
                if (scroll_y + l < NANO_MAX_LINES && c < edit_lens[scroll_y + l]) {
                    char_to_draw = edit_lines[scroll_y + l][c];
                }
                vmem[(l + 1) * 80 + c] = (uint16_t)char_to_draw | 0x0F00;
            }
        }
        
        for (int i = 0; i < 80; i++) vmem[23 * 80 + i] = ' ' | 0x0F00;
        for (int i = 0; i < 80; i++) vmem[24 * 80 + i] = (uint16_t)footer[i] | 0x7000;
        
        extern void update_cursor(int, int);
        update_cursor(edit_cx, (edit_cy - scroll_y) + 1);
    }
}

void run_nano(const char* filename) {
    int term_cols = is_graphics_mode ? (g_width / 8) : 80;
    if (term_cols > NANO_MAX_COLS) term_cols = NANO_MAX_COLS;
    int term_rows = is_graphics_mode ? (g_height / 16) : 25;
    int visible_lines = term_rows - 3;

    /* Clean up the editor memory */
    for(int l = 0; l < NANO_MAX_LINES; l++) {
        for(int c = 0; c < NANO_MAX_COLS; c++) edit_lines[l][c] = '\0';
        edit_lens[l] = 0;
    }
    edit_cx = 0; edit_cy = 0; scroll_y = 0;
    
    /* VERY IMPORTANT: Wipe the shell output from the screen before launching */
    clear_terminal();

    /* Load the file if it exists on disk */
    uint8_t* buf = NULL;
    uint32_t size = 0;
    if (fs_load_file(filename, &buf, &size)) {
        int l = 0, c = 0;
        for (uint32_t i = 0; i < size && l < NANO_MAX_LINES; i++) {
            if (buf[i] == '\n') {
                edit_lines[l][c] = '\0';
                edit_lens[l] = c;
                l++; c = 0;
            } else if (buf[i] == '\r') {
                /* Ignore */
            } else {
                if (c < term_cols - 1) edit_lines[l][c++] = buf[i];
            }
        }
        if (l < NANO_MAX_LINES && c > 0) {
            edit_lines[l][c] = '\0';
            edit_lens[l] = c;
        }
        kfree(buf);
    } else if (fs_error == FS_ERR_PERMISSION) {
        kprintf("Error: Permission denied.\n");
        return;
    }
    
    while (1) {
        draw_editor(filename);
        char c = wait_key();
        
        if (c == 24) break; /* Ctrl+X */
        else if (c == 19) { /* Ctrl+S */
            int last_non_empty = -1;
            for (int l = NANO_MAX_LINES - 1; l >= 0; l--) {
                if (edit_lens[l] > 0) { last_non_empty = l; break; }
            }
            int max_line_to_write = (last_non_empty == -1) ? 0 : last_non_empty;
            
            uint32_t required_size = 0;
            for (int l = 0; l <= max_line_to_write; l++) required_size += edit_lens[l] + 1;
            
            char* out_buf = (char*)kmalloc(required_size + 1);
            int out_idx = 0;
            for (int l = 0; l <= max_line_to_write; l++) {
                for (int col = 0; col < edit_lens[l]; col++) out_buf[out_idx++] = edit_lines[l][col];
                out_buf[out_idx++] = '\n';
            }
            out_buf[out_idx] = '\0';
            
            extern int fs_write_bin(const char*, uint8_t*, uint32_t);
            fs_write_bin(filename, (uint8_t*)out_buf, out_idx);
            kfree(out_buf);
            
            /* Visual confirmation adapted for graphics mode */
            char* save_msg = "  [ File successfully saved! ]  ";
            if (is_graphics_mode) {
                uint32_t bg_success = 0x0000AA00;
                uint32_t fg_success = 0x00FFFFFF;
                draw_string(save_msg, 20 * 8, (term_rows - 2) * 16, fg_success, bg_success);
                swap_buffers();
            } else {
                uint16_t* vmem = (uint16_t*)0xB8000;
                int s_idx = 0;
                while(save_msg[s_idx]) {
                    vmem[23 * 80 + 20 + s_idx] = (uint16_t)save_msg[s_idx] | 0x0A00;
                    s_idx++;
                }
            }
            
            uint32_t start_ticks = timer_ticks;
            while (timer_ticks < start_ticks + 150) __asm__ volatile("hlt");
        }
        else if (c == 17) { /* Up */
            if (edit_cy > 0) {
                edit_cy--;
                if (edit_cy < scroll_y) scroll_y--;
                if (edit_cx > edit_lens[edit_cy]) edit_cx = edit_lens[edit_cy];
            }
        }
        else if (c == 18) { /* Down */
            if (edit_cy < NANO_MAX_LINES - 1) {
                edit_cy++;
                if (edit_cy >= scroll_y + visible_lines) scroll_y++;
                if (edit_cx > edit_lens[edit_cy]) edit_cx = edit_lens[edit_cy];
            }
        }
        else if (c == 20) { /* Left */
            if (edit_cx > 0) edit_cx--;
            else if (edit_cy > 0) {
                edit_cy--;
                if (edit_cy < scroll_y) scroll_y--;
                edit_cx = edit_lens[edit_cy];
            }
        }
        else if (c == 21) { /* Right */
            if (edit_cx < edit_lens[edit_cy]) edit_cx++;
            else if (edit_cy < NANO_MAX_LINES - 1) {
                edit_cy++;
                if (edit_cy >= scroll_y + visible_lines) scroll_y++;
                edit_cx = 0;
            }
        }
        else if (c == '\n' || c == '\r') { /* Enter */
            if (edit_cy < NANO_MAX_LINES - 1) {
                for (int l = NANO_MAX_LINES - 1; l > edit_cy; l--) {
                    for(int col = 0; col < NANO_MAX_COLS; col++) edit_lines[l][col] = edit_lines[l - 1][col];
                    edit_lens[l] = edit_lens[l - 1];
                }
                int remaining = edit_lens[edit_cy] - edit_cx;
                for (int col = 0; col < remaining; col++) {
                    edit_lines[edit_cy + 1][col] = edit_lines[edit_cy][edit_cx + col];
                }
                edit_lens[edit_cy + 1] = remaining;
                edit_lens[edit_cy] = edit_cx;
                
                edit_cy++;
                if (edit_cy >= scroll_y + visible_lines) scroll_y++;
                edit_cx = 0;
            }
        }
        else if (c == '\b' || c == 127) { /* Backspace */
            if (edit_cx > 0) {
                for (int col = edit_cx - 1; col < edit_lens[edit_cy]; col++) {
                    edit_lines[edit_cy][col] = edit_lines[edit_cy][col + 1];
                }
                edit_cx--;
                edit_lens[edit_cy]--;
            } else if (edit_cy > 0) {
                int prev_len = edit_lens[edit_cy - 1];
                int curr_len = edit_lens[edit_cy];
                if (prev_len + curr_len < term_cols - 1) {
                    for (int col = 0; col < curr_len; col++) {
                        edit_lines[edit_cy - 1][prev_len + col] = edit_lines[edit_cy][col];
                    }
                    edit_lens[edit_cy - 1] += curr_len;
                    for (int l = edit_cy; l < NANO_MAX_LINES - 1; l++) {
                        for(int col = 0; col < NANO_MAX_COLS; col++) edit_lines[l][col] = edit_lines[l + 1][col];
                        edit_lens[l] = edit_lens[l + 1];
                    }
                    edit_lens[NANO_MAX_LINES - 1] = 0;
                    
                    edit_cy--;
                    if (edit_cy < scroll_y) scroll_y--;
                    edit_cx = prev_len;
                }
            }
        }
        else if (c >= 32 && c <= 126) { /* Typing text */
            if (edit_lens[edit_cy] < term_cols - 1) {
                for (int col = edit_lens[edit_cy]; col > edit_cx; col--) {
                    edit_lines[edit_cy][col] = edit_lines[edit_cy][col - 1];
                }
                edit_lines[edit_cy][edit_cx] = c;
                edit_cx++;
                edit_lens[edit_cy]++;
            }
        }
    }
    clear_terminal(); 
}

/* PASSWORD OVERWRITTING */

int update_user_password(const char* target_user, const char* new_hash_str) {
    uint8_t* buf; uint32_t size;
    /* Temporary privilege elevation to Root to modify the passwd file */
    uint8_t old_uid = current_uid;
    current_uid = 0;
    
    if (!fs_load_file("/passwd", &buf, &size)) {
        current_uid = old_uid;
        return 0;
    }
    
    /* Allocation of a temporary buffer with extra margin */
    uint8_t* new_buf = (uint8_t*)kmalloc(size + 64);
    uint32_t new_size = 0;
    uint32_t offset = 0;
    int found = 0;
    
    while (offset < size) {
        uint32_t line_start = offset;
        char u[32], p[32], id_str[8]; int ui=0, pi=0, idi=0;
        while(offset < size && buf[offset] != ':' && buf[offset] != '\n') { if(ui<31) u[ui++] = buf[offset]; offset++; } u[ui] = '\0';
        if (offset < size && buf[offset] == ':') offset++;
        while(offset < size && buf[offset] != ':' && buf[offset] != '\n') { if(pi<31) p[pi++] = buf[offset]; offset++; } p[pi] = '\0';
        if (offset < size && buf[offset] == ':') offset++;
        while(offset < size && buf[offset] != '\n') { if(idi<7) id_str[idi++] = buf[offset]; offset++; } id_str[idi] = '\0';
        if (offset < size && buf[offset] == '\n') offset++;
        uint32_t line_end = offset;
        
        if (ui == 0) continue;
        
        if (strcmp(u, target_user) == 0) {
            found = 1;
            /* Rewrite this user's line with the new hash */
            char new_line[128];
            int nl_len = 0;
            char* u_ptr = u; while(*u_ptr) new_line[nl_len++] = *u_ptr++;
            new_line[nl_len++] = ':';
            const char* h_ptr = new_hash_str; while(*h_ptr) new_line[nl_len++] = *h_ptr++;
            new_line[nl_len++] = ':';
            char* id_ptr = id_str; while(*id_ptr) new_line[nl_len++] = *id_ptr++;
            new_line[nl_len++] = '\n';
            
            for(int k = 0; k < nl_len; k++) {
                new_buf[new_size++] = new_line[k];
            }
        } else {
            /* Copy other users' lines untouched */
            for(uint32_t k = line_start; k < line_end; k++) {
                new_buf[new_size++] = buf[k];
            }
        }
    }
    new_buf[new_size] = '\0';
    
    if (found) {
        fs_write("/passwd", (const char*)new_buf);
    }
    
    kfree(buf);
    kfree(new_buf);
    current_uid = old_uid;
    return found;
}

/* Display an offset/address in 4 hexadecimal digits (e.g., 0010) */
void print_hex_offset(uint32_t offset) {
    const char* hex_chars = "0123456789abcdef";
    char buf[5];
    buf[0] = hex_chars[(offset >> 12) & 0xF];
    buf[1] = hex_chars[(offset >> 8) & 0xF];
    buf[2] = hex_chars[(offset >> 4) & 0xF];
    buf[3] = hex_chars[offset & 0xF];
    buf[4] = '\0';
    kprintf("%s", buf);
}

/* Display a byte in 2 hexadecimal digits (e.g., 0a) */
void print_hex_byte(uint8_t b) {
    const char* hex_chars = "0123456789abcdef";
    char buf[3];
    buf[0] = hex_chars[(b >> 4) & 0xF];
    buf[1] = hex_chars[b & 0xF];
    buf[2] = '\0';
    kprintf("%s", buf);
}

/* Main function of the hex editor (hexdump) */
void run_hexdump(const char* filename) {
    uint8_t* file_buf = NULL;
    uint32_t file_size = 0;
    
    if (!fs_load_file(filename, &file_buf, &file_size)) {
        if (fs_error == FS_ERR_PERMISSION) {
            kprintf("Error: Permission denied (Read access required).\n");
        } else {
            kprintf("Error: '%s': No such file\n", filename);
        }
        return;
    }

    if (file_size == 0) {
        kprintf("[Empty file]\n");
        kfree(file_buf);
        return;
    }

    for (uint32_t i = 0; i < file_size; i += 16) {
        /* 1. Address/offset display */
        print_hex_offset(i);
        kprintf(":  ");

        /* 2. Hexadecimal byte display */
        for (uint32_t j = 0; j < 16; j++) {
            if (i + j < file_size) {
                print_hex_byte(file_buf[i + j]);
                kprintf(" ");
            } else {
                /* Padding spaces in case of early end of file */
                kprintf("   "); 
            }
            
            /* Extra separation space in the middle of the block (after 8 bytes) */
            if (j == 7) {
                kprintf(" ");
            }
        }

        kprintf(" |");

        /* 3. Readable ASCII representation display */
        for (uint32_t j = 0; j < 16; j++) {
            if (i + j < file_size) {
                uint8_t c = file_buf[i + j];
                /* Standard printable ASCII characters */
                if (c >= 32 && c <= 126) {
                    kprintf("%c", c);
                } else {
                    kprintf(".");
                }
            }
        }
        kprintf("|\n");
    }

    kfree(file_buf);
}

/* --- INSERT THESE FUNCTIONS DIRECTLY ABOVE execute_command() --- */

void bin_to_hex(const uint8_t* bin, size_t len, char* hex_out) {
    const char* hex_chars = "0123456789abcdef";
    for(size_t i = 0; i < len; i++) {
        hex_out[i * 2] = hex_chars[(bin[i] >> 4) & 0x0F];
        hex_out[i * 2 + 1] = hex_chars[bin[i] & 0x0F];
    }
    hex_out[len * 2] = '\0';
}

/* Reusable HTTP Downloader extracted from wget */
int http_get(const char* host, uint16_t port, const char* path, uint8_t** out_data, uint32_t* out_len) {
    uint8_t target_ip[4];
    extern int net_gethostbyname(const char*, uint8_t*);
    if (!net_gethostbyname(host, target_ip)) return 0;
    
    extern int net_alloc_socket(int);
    int sock_idx = net_alloc_socket(2); /* TCP */
    if (sock_idx == -1) return 0;
    
    extern socket_t sockets[];
    /* Randomize local port slightly to prevent TIME_WAIT TCP conflicts on consecutive requests */
    sockets[sock_idx].local_port = 55555 + (timer_ticks % 10000); 
    for(int k=0; k<4; k++) sockets[sock_idx].remote_ip[k] = target_ip[k];
    sockets[sock_idx].remote_port = port; /* Direct port assignment */
    sockets[sock_idx].seq_num = timer_ticks * 100;
    sockets[sock_idx].ack_num = 0;
    sockets[sock_idx].tcp_state = 1; /* TCP_SYN_SENT */
    
    extern void net_send_tcp_raw(int, uint8_t, uint8_t*, uint16_t);
    net_send_tcp_raw(sock_idx, 0x02, NULL, 0); /* Send SYN */
    
    __asm__ volatile("sti");
    uint32_t start = timer_ticks;
    while(sockets[sock_idx].tcp_state != 2) { /* 2 = TCP_ESTABLISHED */
        if (timer_ticks > start + 500) { sockets[sock_idx].is_used = 0; return 0; }
        __asm__ volatile("hlt");
    }
    
    char request[256];
    int r_idx = 0;
    const char* p1 = "GET "; while(*p1) request[r_idx++] = *p1++;
    const char* p2 = path; while(*p2) request[r_idx++] = *p2++;
    const char* p3 = " HTTP/1.0\r\nHost: "; while(*p3) request[r_idx++] = *p3++;
    const char* p4 = host; while(*p4) request[r_idx++] = *p4++;
    const char* p5 = "\r\nConnection: close\r\n\r\n"; while(*p5) request[r_idx++] = *p5++;
    request[r_idx] = '\0';
    
    net_send_tcp_raw(sock_idx, 0x18, (uint8_t*)request, r_idx);
    sockets[sock_idx].seq_num += r_idx;
    
    uint32_t MAX_DL_SIZE = 1024 * 1024; /* 1 MB limit */
    uint8_t* http_data = (uint8_t*)kmalloc(MAX_DL_SIZE); 
    uint32_t http_len = 0;
    int conn_closed = 0;
    start = timer_ticks;
    
    while(timer_ticks < start + 500 && !conn_closed) {
        if (sockets[sock_idx].rx_ready) {
            __asm__ volatile("cli");
            for(uint32_t j=0; j<sockets[sock_idx].rx_len; j++) {
                if(http_len < MAX_DL_SIZE) http_data[http_len++] = sockets[sock_idx].rx_buffer[j];
            }
            sockets[sock_idx].rx_len = 0;
            sockets[sock_idx].rx_ready = 0;
            __asm__ volatile("sti");
            start = timer_ticks;
        }
        if (sockets[sock_idx].tcp_state == 0) conn_closed = 1;
        else __asm__ volatile("hlt");
    }
    sockets[sock_idx].is_used = 0;
    
    if (http_len == 0) { kfree(http_data); return 0; }
    
    uint32_t payload_start = 0;
    for(uint32_t i=0; i < http_len - 3; i++) {
        if(http_data[i] == '\r' && http_data[i+1] == '\n' && 
           http_data[i+2] == '\r' && http_data[i+3] == '\n') {
            payload_start = i + 4;
            break;
        }
    }
    
    if (payload_start == 0) { kfree(http_data); return 0; }
    
    uint32_t file_size = http_len - payload_start;
    /* Allocate exact size, copy payload, and free the massive buffer! */
    uint8_t* clean_buf = (uint8_t*)kmalloc(file_size);
    for(uint32_t i=0; i<file_size; i++) clean_buf[i] = http_data[payload_start + i];
    
    kfree(http_data);
    *out_data = clean_buf;
    *out_len = file_size;
    return 1;
}

void execute_command() {
    shell_input_enabled = 0; 

    /* --- SECURITY AND '&' WILDCARD DETECTION --- */
    int is_bg = 0;
    int cmd_len = 0;
    while (cmd_buffer[cmd_len]) cmd_len++;
    
    /* Remove spaces at the end of the buffer (Trim) */
    int last_idx = cmd_len - 1;
    while (last_idx >= 0 && cmd_buffer[last_idx] == ' ') {
        last_idx--;
    }
    cmd_buffer[last_idx + 1] = '\0';

    /* Check if the last character is a '&' */
    cmd_len = last_idx + 1;
    if (cmd_len > 0 && cmd_buffer[cmd_len - 1] == '&') {
        is_bg = 1;
        cmd_buffer[cmd_len - 1] = '\0'; /* Remove the '&' */
        
        /* Clean up again the potential spaces at the end */
        last_idx = cmd_len - 2;
        while (last_idx >= 0 && cmd_buffer[last_idx] == ' ') {
            last_idx--;
        }
        cmd_buffer[last_idx + 1] = '\0';
    }

    /* Save of the parsed buffer */
    char raw_buf[256];
    custom_strcpy(raw_buf, cmd_buffer);

    char* cmd = NULL;
    char* arg1 = NULL;
    char* arg2 = NULL;
    char* arg_rest = NULL; 
    char* p = cmd_buffer;

    while(*p == ' ') p++;
    if(*p) cmd = p;
    while(*p && *p != ' ') p++;
    if(*p) { *p = '\0'; p++; } 

    while(*p == ' ') p++;
    if(*p) arg1 = p;
    while(*p && *p != ' ') p++;
    
    if(*p) { 
        *p = '\0'; p++; 
        while(*p == ' ') p++;
        if(*p) {
            arg2 = p;
            arg_rest = p; 
        }
    }

    if (cmd == NULL) goto end_prompt;

    if (strcmp(cmd, "help") == 0) {
        /* This line is ugly and non dynamic: it will be modified later on */
        kprintf("Available commands:\n  help      clear    uptime\n  ram       ls       cd\n  cat       mkdir    rm\n  touch     write    loop\n  run       df       ps\n  kill      about    shutdown\n  whoami    chown    chmod\n  su        sudo     userlist\n  nano      passwd   cp\n  mv        hex      ifconfig\n  wget      update\n");
    }
    else if (strcmp(cmd, "ifconfig") == 0) {
        kprintf("e1000     Link encap: Ethernet  HWaddr ");
        
        /* Print MAC Address in hex format */
        const char* hex = "0123456789ABCDEF";
        for (int i = 0; i < 6; i++) {
            uint8_t m = e1000_mac[i];
            kputc(hex[m >> 4]); 
            kputc(hex[m & 0x0F]);
            if (i < 5) kputc(':');
        }
        kprintf("\n");
        
        /* Print IPv4 Configuration */
        kprintf("          inet addr: %d.%d.%d.%d  Mask: %d.%d.%d.%d\n",
                sOS_ip[0], sOS_ip[1], sOS_ip[2], sOS_ip[3],
                sOS_subnet[0], sOS_subnet[1], sOS_subnet[2], sOS_subnet[3]);
                
        kprintf("          Gateway  : %d.%d.%d.%d  DNS : %d.%d.%d.%d\n",
                sOS_router[0], sOS_router[1], sOS_router[2], sOS_router[3],
                sOS_dns[0], sOS_dns[1], sOS_dns[2], sOS_dns[3]);
                
        kprintf("          UP BROADCAST RUNNING MULTICAST  MTU: 1500\n");
    }
    else if (strcmp(cmd, "clear") == 0) {
        clear_terminal();
    } 
    else if (strcmp(cmd, "uptime") == 0) {
        kprintf("Uptime: %d sec\n", timer_ticks / 100);
    } 
    else if (strcmp(cmd, "ram") == 0) {
        kprintf("Total detected RAM: %d Mo\n", total_ram_kb / 1024);
    } 
    else if (strcmp(cmd, "ls") == 0) {
        fs_ls();
    } 
    else if (strcmp(cmd, "cd") == 0) {
        if (arg1) fs_cd(arg1);
        else kprintf("Usage: cd <directory>\n");
    }
    else if (strcmp(cmd, "mkdir") == 0) {
        if (arg1) fs_mkdir(arg1);
        else kprintf("Usage: mkdir <directory>\n");
    }
    else if (strcmp(cmd, "rm") == 0) {
        fs_rm(arg1, arg2);
    }
    else if (strcmp(cmd, "touch") == 0) {
        if (arg1) fs_touch(arg1);
        else kprintf("Usage: touch <filename>\n");
    }
    else if (strcmp(cmd, "write") == 0) {
        if (arg1 && arg_rest) {
            if (!fs_write(arg1, arg_rest)) {
                kprintf("Error: Write failed (Permission denied or invalid path).\n");
            }
        } else {
            kprintf("Usage: write <filename> <text to write>\n");
        }
    }
    else if (strcmp(cmd, "cat") == 0) {
        if (arg1 != NULL) fs_cat(arg1);
        else kprintf("Error: Specify a file (ex: cat root.txt)\n");
    }
    else if (strcmp(cmd, "loop") == 0) {
        create_task(background_loop);
        kprintf("Background task launched! It will count in the background.\n");
    }
    else if (strcmp(cmd, "df") == 0) {
        fs_df();
    }
    else if (strcmp(cmd, "cp") == 0) {
        if (arg1 && arg2) fs_cp(arg1, arg2);
        else kprintf("Usage: cp <source> <destination>\n");
    }
    else if (strcmp(cmd, "mv") == 0) {
        if (arg1 && arg2) fs_mv(arg1, arg2);
        else kprintf("Usage: mv <source> <destination>\n");
    }
    else if (strcmp(cmd, "run") == 0) {
        if (arg1 != NULL) {
            uint8_t* file_buf;
            uint32_t file_size;
            if (fs_load_file(arg1, &file_buf, &file_size)) {
                int task_id = create_user_task(file_buf, file_size);
                kfree(file_buf);
                if (task_id != -1) {
                    /* Associate the background status with the task */
                    tasks[task_id].is_background = is_bg; 
                    
                    if (is_bg) {
                        kprintf("[Process %d launched in background]\n", task_id);
                        /* Prompt is given back instantly to the user */
                    } else {
                        /* Classic blocking synchronous mode */
                        while(tasks[task_id].state != 0) {
                            __asm__ volatile("sti; hlt");
                        }
                    }
                }
            } else {
                if (fs_error == FS_ERR_PERMISSION) {
                    kprintf("Error: Permission denied (Read access required).\n");
                } else {
                    kprintf("Error: '%s': No such file\n", arg1);
                    char file_suggest[13];
                    extern int fs_suggest_file(const char*, char*);
                    if (fs_suggest_file(arg1, file_suggest)) {
                        kprintf("Did you mean '%s'?\n", file_suggest);
                    }
                }
            }
        } else {
            kprintf("Usage: run <filename.sos>\n");
        }
    }
    else if (strcmp(cmd, "ps") == 0) {
        kprintf("PID | Type       | State\n");
        kprintf("-------------------------\n");
        for(int i = 0; i < MAX_TASKS; i++) {
            if (tasks[i].state != 0) {
                const char* type = (i == 0) ? "Shell" : (tasks[i].code_page == NULL ? "SysTask" : "UserApp");
                const char* state = (tasks[i].state == 1) ? "RUNNING" : "READY";
                kprintf(" %d  | %s", i, type);
                int len = 0; while(type[len]) len++;
                for(int s = len; s < 10; s++) kprintf(" ");
                kprintf(" | %s\n", state);
            }
        }
    }
    else if (strcmp(cmd, "kill") == 0) {
        if (arg1 != NULL) {
            int pid = 0; char* p = arg1;
            while (*p >= '0' && *p <= '9') { pid = pid * 10 + (*p - '0'); p++; }
            if (pid == 0) {
                kprintf("Error: Cannot kill the system Shell.\n");
            } else if (pid >= MAX_TASKS || tasks[pid].state == 0) {
                kprintf("Error: No active process with PID %d.\n", pid);
            } else {
                /* SECURITY: Only the creator (or root UID 0) can kill the task */
                if (current_uid != 0 && tasks[pid].uid != current_uid) {
                    kprintf("Error: Operation not permitted (You do not own this process).\n");
                } else {
                    cleanup_task_memory(pid);
                    kprintf("Process %d terminated and memory freed.\n", pid);
                }
            }
        } else {
            kprintf("Usage: kill <PID>\n");
        }
    }
    else if (strcmp(cmd, "about") == 0) {
        kprintf("        ________     _________\n");
        kprintf("  ______\\_____  \\   /   _____/\n");
        kprintf(" /  ___/ /   |   \\  \\_____  \\ \n");
        kprintf(" \\___ \\ /    |    \\ /        \\ \n");
        kprintf("/____  >\\_______  //_______  /\n");
        kprintf("     \\/         \\/         \\/ \n\n");
        kprintf("sOS (Simple Operating System) - v%s\n", SOS_VERSION); 
        kprintf("Arch  : i686 (32-bit x86 Intel)\n");
        kprintf("Memory: %d MB\n", total_ram_kb / 1024);
        kprintf("FS    : FAT16 (ATA PIO Mode)\n");
        kprintf("Uptime: %d sec\n", timer_ticks / 100);
        kprintf("Author: LFM57\n\n");
    }
    else if (strcmp(cmd, "shutdown") == 0) {
        kprintf("Are you sure you want to shutdown? [y/N]: ");
        char choice = wait_key();
        kprintf("%c\n", choice); 
        if (choice == 'y' || choice == 'Y') {
            kprintf("Shutting down sOS cleanly...\n");
            for (volatile int i = 0; i < 100000000; i++);
            outw(0x4004, 0x3400);
            outw(0x604, 0x2000);
            outw(0xB004, 0x2000);
            kprintf("\nPower off failed. You can safely turn off your computer.\n");
            while(1) { __asm__ volatile("cli; hlt"); }
        } else {
            kprintf("Shutdown aborted.\n");
        }
    }
    else if (strcmp(cmd, "whoami") == 0) {
        kprintf("User: %s (UID: %d)\n", current_user, current_uid);
    }
    else if (strcmp(cmd, "chown") == 0) {
        if (arg1 && arg2) {
            int new_uid = 0; char* p = arg2;
            while (*p >= '0' && *p <= '9') { new_uid = new_uid * 10 + (*p - '0'); p++; }
            fs_chown(arg1, (uint8_t)new_uid);
        } else kprintf("Usage: chown <file> <uid>\n");
    }
    else if (strcmp(cmd, "chmod") == 0) {
        if (arg1 && arg2) {
            uint8_t perms = 0; char* p = arg1;
            while(*p) {
                perms *= 16;
                if (*p >= '0' && *p <= '9') perms += *p - '0';
                else if (*p >= 'a' && *p <= 'f') perms += *p - 'a' + 10;
                else if (*p >= 'A' && *p <= 'F') perms += *p - 'A' + 10;
                p++;
            }
            fs_chmod(arg2, perms);
        } else kprintf("Usage: chmod <hex_perms> <file> (ex: chmod 70 secret.txt)\n");
    }
    else if (strcmp(cmd, "su") == 0) {
        if (arg1 == NULL) arg1 = "root";
        char real_pass[32]; uint8_t uid;
        if (get_user_info(arg1, real_pass, &uid)) {
            kprintf("Password: ");
            char pass[32]; kernel_gets(pass, 32, 1, 1);
            if (check_password(pass, real_pass)) {
                current_uid = uid; custom_strcpy(current_user, arg1);
                kprintf("Switched to user %s.\n", current_user);
                
                /* [FIX] Return to the root first to target the correct directory */
                fs_cd("/");
                
                if (current_uid != 0) {
                    current_uid = 0; /* Switch to Root for chown/chmod */
                    fs_chown(arg1, uid);
                    fs_chmod(arg1, 0x30);
                    current_uid = uid; /* Restores user original identity */
                    
                    fs_cd(arg1); /* Move into his personnal folder */
                }
            } else kprintf("su: Authentication failure\n");
        } else kprintf("su: user %s does not exist\n", arg1);
    }
    else if (strcmp(cmd, "sudo") == 0) {
        if (arg1 == NULL) { kprintf("Usage: sudo <command>\n"); goto end_prompt; }
        
        char real_pass[32]; uint8_t dummy_uid;
        if (get_user_info("root", real_pass, &dummy_uid)) {
            kprintf("[sudo] password for root: ");
            char pass[32]; kernel_gets(pass, 32, 1, 1); 
            if (check_password(pass, real_pass)) {
                uint8_t old_uid = current_uid;
                char old_user[32];
                custom_strcpy(old_user, current_user);
                
                current_uid = 0; 
                
                int i = 0; 
                while (raw_buf[i] && raw_buf[i] != ' ') i++; 
                while (raw_buf[i] && raw_buf[i] == ' ') i++; 
                custom_strcpy(cmd_buffer, &raw_buf[i]);
                
                execute_command(); 
                    
                /* [SECURITY FIX] Restore credentials only if the session has not been reset */
                if (!session_reset) {
                    current_uid = old_uid; 
                    custom_strcpy(current_user, old_user);
                }
                shell_input_enabled = 1;
                return;
            } else kprintf("sudo: 1 incorrect password attempt\n");
        }
    }
    else if (strcmp(cmd, "userlist") == 0) {
        if (arg1 == NULL) {
            kprintf("Usage: userlist <list|add|remove> [args]\n");
            goto end_prompt;
        }
        
        /* 1. ACTION: LIST */
        if (strcmp(arg1, "list") == 0) {
            uint8_t* buf; uint32_t size;
            uint8_t old_uid = current_uid; current_uid = 0; /* Temporarly Root */
            if (!fs_load_file("/passwd", &buf, &size)) {
                kprintf("Error: Could not read /passwd\n"); current_uid = old_uid; goto end_prompt;
            }
            current_uid = old_uid;
            
            kprintf("UID | Username    | Password Details\n");
            kprintf("------------------------------------\n");
            uint32_t i = 0;
            while (i < size) {
                char u[32], p[32], id_str[8]; int ui=0, pi=0, idi=0;
                while(i < size && buf[i] != ':' && buf[i] != '\n') { if(ui<31) u[ui++] = buf[i]; i++; } u[ui] = '\0';
                if (i < size && buf[i] == ':') i++;
                while(i < size && buf[i] != ':' && buf[i] != '\n') { if(pi<31) p[pi++] = buf[i]; i++; } p[pi] = '\0';
                if (i < size && buf[i] == ':') i++;
                while(i < size && buf[i] != '\n') { if(idi<7) id_str[idi++] = buf[i]; i++; } id_str[idi] = '\0';
                if (i < size && buf[i] == '\n') i++;

                if (ui == 0) continue;
                
                kprintf(" %s  | %s", id_str, u);
                int len = 0; while(u[len]) len++;
                for(int s = len; s < 11; s++) kprintf(" ");
                
                /* [SECURITY] Only Root can see the true hashes */
                if (current_uid == 0) {
                    int dollar_idx = -1;
                    for (int k = 0; p[k]; k++) {
                        if (p[k] == '$') { dollar_idx = k; break; }
                    }
                    
                    /* If a '$' separator is found, split and print Salt + Hash */
                    if (dollar_idx != -1) {
                        p[dollar_idx] = '\0'; /* Split string in-place */
                        char* salt = p;
                        char* hash = &p[dollar_idx + 1];
                        kprintf(" | Salt: %s | Hash: %s\n", salt, hash);
                    } else {
                        /* For backwards compatibility with old unsalted passwords */
                        kprintf(" | Salt: None     | Hash: %s [UNSALTED/INSECURE]\n", p);
                    }
                }
                else {
                    kprintf(" | [HIDDEN]\n");
                }
            }
            kfree(buf);
        }
        
        /* 2. ACTION: ADD */
        else if (strcmp(arg1, "add") == 0) {
            if (current_uid != 0) { kprintf("Error: Root privileges required.\n"); goto end_prompt; }
            
            /* Internal splitting of the argument (username / password) */
            char* username = arg2; char* password = NULL;
            if (username) {
                char* p_pass = username;
                while (*p_pass && *p_pass != ' ') p_pass++;
                if (*p_pass == ' ') {
                    *p_pass = '\0'; p_pass++;
                    while (*p_pass && *p_pass == ' ') p_pass++;
                    if (*p_pass) password = p_pass;
                }
            }

            if (!username || !password) { kprintf("Usage: userlist add <username> <password>\n"); goto end_prompt; }

            /* Strict character validation to prevent injection */
            char* check_u = username;
            while (*check_u) {
                if (*check_u == ':' || *check_u == '\n' || *check_u == '\r') {
                    kprintf("Error: Username contains invalid characters (':', '\\n', '\\r').\n");
                    goto end_prompt;
                }
                check_u++;
            }

            char* check_p = password;
            while (*check_p) {
                if (*check_p == ':' || *check_p == '\n' || *check_p == '\r') {
                    kprintf("Error: Password contains invalid characters (':', '\\n', '\\r').\n");
                    goto end_prompt;
                }
                check_p++;
            }

            /* Validation of the username length for FAT16 */
            int u_len = 0;
            while (username[u_len] != '\0') u_len++;
            if (u_len > 8) {
                kprintf("Error: Username is too long (max 8 characters allowed for FAT16 home folder).\n");
                goto end_prompt;
            }

            /* Checks if user already exists */
            char dummy_pass[32]; uint8_t dummy_id;
            if (get_user_info(username, dummy_pass, &dummy_id)) {
                kprintf("Error: User '%s' already exists.\n", username); goto end_prompt;
            }

            /* Searches for the next available UID */
            uint8_t* buf; uint32_t size;
            if (!fs_load_file("/passwd", &buf, &size)) { kprintf("Error: Could not read /passwd\n"); goto end_prompt; }
            uint8_t max_uid = 0; uint32_t offset = 0;
            while (offset < size) {
                char u[32], p[32], id_str[8]; int ui=0, pi=0, idi=0;
                while(offset < size && buf[offset] != ':' && buf[offset] != '\n') { if(ui<31) u[ui++] = buf[offset]; offset++; } u[ui] = '\0';
                if (offset < size && buf[offset] == ':') offset++;
                while(offset < size && buf[offset] != ':' && buf[offset] != '\n') { if(pi<31) p[pi++] = buf[offset]; offset++; } p[pi] = '\0';
                if (offset < size && buf[offset] == ':') offset++;
                while(offset < size && buf[offset] != '\n') { if(idi<7) id_str[idi++] = buf[offset]; offset++; } id_str[idi] = '\0';
                if (offset < size && buf[offset] == '\n') offset++;
                if (ui == 0) continue;
                (void)u;
                (void)p;
                int uid = 0; for(int k=0; k<idi; k++) uid = uid*10 + (id_str[k]-'0');
                if (uid > max_uid) max_uid = uid;
            }
            uint8_t new_uid = max_uid + 1;
            if (new_uid >= 16) { kprintf("Error: Max 15 users supported (UID 1-15).\n"); kfree(buf); goto end_prompt; }

            /* [UPDATED] Salt generation and salted, stretched password hashing */
            char salt[9]; generate_salt(salt);
            char hash_str[9]; hash_password_salted(password, salt, hash_str);
            
            /* Build the final format "salt$hash" (e.g. "a1b2c3d4$e5f6a7b8") */
            char final_store[32];
            int fs_idx = 0;
            for(int k=0; salt[k]; k++) final_store[fs_idx++] = salt[k];
            final_store[fs_idx++] = '$';
            for(int k=0; hash_str[k]; k++) final_store[fs_idx++] = hash_str[k];
            final_store[fs_idx] = '\0';

            /* Creating new line in passwd */
            char new_line[128]; int nl_len = 0;
            char* u_ptr = username; while(*u_ptr) new_line[nl_len++] = *u_ptr++;
            new_line[nl_len++] = ':';
            /* [UPDATED] h_ptr now points to final_store ("salt$hash") instead of the raw hash */
            char* h_ptr = final_store; while(*h_ptr) new_line[nl_len++] = *h_ptr++;
            new_line[nl_len++] = ':';
            if (new_uid >= 10) {
                new_line[nl_len++] = (new_uid / 10) + '0';
                new_line[nl_len++] = (new_uid % 10) + '0';
            } else { new_line[nl_len++] = new_uid + '0'; }
            new_line[nl_len++] = '\n'; new_line[nl_len] = '\0';

            /* Secure allocation of size + 1 for the null terminator */
            uint32_t new_size = size + nl_len;
            uint8_t* new_buf = (uint8_t*)kmalloc(new_size + 1);
            if (new_buf == NULL) {
                kprintf("Error: Out of memory.\n");
                kfree(buf);
                goto end_prompt;
            }
            
            for(uint32_t k=0; k<size; k++) new_buf[k] = buf[k];
            for(int k=0; k<nl_len; k++) new_buf[size + k] = new_line[k];
            new_buf[new_size] = '\0';

            fs_write("/passwd", (const char*)new_buf);
            kfree(buf); kfree(new_buf);

            /* Automatic creation of Home folder */
            fs_mkdir(username);
            fs_chown(username, new_uid);
            fs_chmod(username, 0x30); /* Standard 0x30 permissions */

            kprintf("User '%s' (UID %d) added. Home directory created.\n", username, new_uid);
        }
        
        /* 3. ACTION: REMOVE */
        else if (strcmp(arg1, "remove") == 0) {
            if (current_uid != 0) { kprintf("Error: Root privileges required.\n"); goto end_prompt; }
            char* target = arg2;
            if (!target) { kprintf("Usage: userlist remove <username>\n"); goto end_prompt; }
            if (strcmp(target, "root") == 0) { kprintf("Error: Cannot remove root user.\n"); goto end_prompt; }

            uint8_t* buf; uint32_t size;
            if (!fs_load_file("/passwd", &buf, &size)) { kprintf("Error: Could not read /passwd\n"); goto end_prompt; }

            uint8_t* new_buf = (uint8_t*)kmalloc(size);
            uint32_t new_size = 0; uint32_t offset = 0; int found = 0;

            while (offset < size) {
                uint32_t line_start = offset;
                char u[32]; int ui=0;
                while(offset < size && buf[offset] != ':' && buf[offset] != '\n') { if(ui<31) u[ui++] = buf[offset]; offset++; } u[ui] = '\0';
                while(offset < size && buf[offset] != '\n') offset++;
                if (offset < size && buf[offset] == '\n') offset++;
                uint32_t line_end = offset;

                if (ui == 0) continue;
                if (strcmp(u, target) == 0) { found = 1; continue; } /* We skip this line */

                for(uint32_t k=line_start; k<line_end; k++) new_buf[new_size++] = buf[k];
            }

            if (!found) {
                kprintf("Error: User '%s' not found.\n", target);
                kfree(buf); kfree(new_buf); goto end_prompt;
            }

            new_buf[new_size] = '\0';
            fs_write("/passwd", (const char*)new_buf);
            kfree(buf); kfree(new_buf);

            kprintf("User '%s' removed from /passwd.\n", target);

            if (strcmp(target, current_user) == 0) {
                logout_and_login("\nYour account has been deleted, please log into an existing one.");
                return;
            }
        }
    }
    else if (strcmp(cmd, "nano") == 0) {
        if (arg1 != NULL) {
            char nano_file[64];
            custom_strcpy(nano_file, arg1);
            run_nano(nano_file);
        } else {
            kprintf("Usage: nano <file>\n");
        }
    }
    else if (strcmp(cmd, "hex") == 0) {
        if (arg1 != NULL) {
            char hex_file[64];
            custom_strcpy(hex_file, arg1);
            run_hexdump(hex_file);
        } else {
            kprintf("Usage: hex <filename>\n");
        }
    }
    else if (strcmp(cmd, "passwd") == 0) {
        char* target = arg1;
        if (target == NULL) {
            target = current_user; /* Default: current user */
        }
        
        /* [SECURITY] A non-root user cannot target an other account */
        if (current_uid != 0 && strcmp(target, current_user) != 0) {
            kprintf("Error: Operation not permitted.\n");
            goto end_prompt;
        }
        
        /* 1. If we are not root, we first need the CURRENT password */
        if (current_uid != 0) {
            char real_pass[32]; uint8_t dummy_uid;
            if (get_user_info(current_user, real_pass, &dummy_uid)) {
                kprintf("Current password: ");
                char pass[32]; kernel_gets(pass, 32, 1, 1);
                if (!check_password(pass, real_pass)) {
                    kprintf("passwd: Authentication failure\n");
                    goto end_prompt;
                }
            } else {
                kprintf("Error: Could not retrieve current user info.\n");
                goto end_prompt;
            }
        }
        
        /* 2. NEW password input */
        kprintf("New password: ");
        char new_pass[32]; kernel_gets(new_pass, 32, 1, 1);
        
        /* 3. Security Check */
        kprintf("Retype new password: ");
        char re_pass[32]; kernel_gets(re_pass, 32, 1, 1);
        
        if (strcmp(new_pass, re_pass) != 0) {
            kprintf("Error: Passwords do not match.\n");
            goto end_prompt;
        }
        
        /* 4. [UPDATED] Salted and stretched hash calculation and write */
        char salt[9]; generate_salt(salt);
        char hash_str[9]; hash_password_salted(new_pass, salt, hash_str);
        
        /* Build the final format "salt$hash" (e.g. "a1b2c3d4$e5f6a7b8") */
        char final_store[32];
        int fs_idx = 0;
        for(int k=0; salt[k]; k++) final_store[fs_idx++] = salt[k];
        final_store[fs_idx++] = '$';
        for(int k=0; hash_str[k]; k++) final_store[fs_idx++] = hash_str[k];
        final_store[fs_idx] = '\0';
        
        /* Save the updated "salt$hash" string to /passwd */
        if (update_user_password(target, final_store)) {
            kprintf("passwd: password updated successfully.\n");
        } else {
            kprintf("Error: User '%s' not found.\n", target);
        }
    }
    /* --- REPLACE THE wget COMMAND BLOCK IN kernel.c WITH THIS --- */
    else if (strcmp(cmd, "wget") == 0) {
        if (!arg1 || !arg2) {
            kprintf("Usage: wget <http://domain.com:port/path> <out.txt>\n");
            goto end_prompt;
        }

        char* url = arg1;
        if (url[0] == 'h' && url[1] == 't' && url[2] == 't' && url[3] == 'p' && 
            url[4] == 's' && url[5] == ':' && url[6] == '/' && url[7] == '/') {
            kprintf("Error: HTTPS is not supported. Please use HTTP URLs.\n");
            goto end_prompt;
        }
        
        if (url[0] == 'h' && url[1] == 't' && url[2] == 't' && url[3] == 'p' && 
            url[4] == ':' && url[5] == '/' && url[6] == '/') {
            url += 7;
        }

        char host[64] = {0}; char path[128] = {0};
        int i = 0;
        while (url[i] && url[i] != '/' && i < 63) { host[i] = url[i]; i++; }
        host[i] = '\0';
        
        if (url[i] == '/') {
            int j = 0;
            while (url[i] && j < 127) path[j++] = url[i++];
            path[j] = '\0';
        } else { 
            path[0] = '/'; path[1] = '\0'; 
        }

        /* Extract optional port (e.g. host:8080) */
        uint16_t port = 80;
        for (int k = 0; host[k] != '\0'; k++) {
            if (host[k] == ':') {
                host[k] = '\0'; /* Split string at colon */
                port = 0;
                int p_idx = k + 1;
                while (host[p_idx] >= '0' && host[p_idx] <= '9') {
                    port = port * 10 + (host[p_idx] - '0');
                    p_idx++;
                }
                break;
            }
        }

        kprintf("Downloading from %s:%d%s...\n", host, port, path);
        
        uint8_t* dl_data = NULL;
        uint32_t dl_size = 0;
        
        if (http_get(host, port, path, &dl_data, &dl_size)) {
            kprintf("Saving %d bytes to '%s'...\n", dl_size, arg2);
            extern int fs_write_bin(const char*, uint8_t*, uint32_t);
            if (fs_write_bin(arg2, dl_data, dl_size)) {
                kprintf("Success! Use 'cat %s' to read it.\n", arg2);
            } else {
                kprintf("Error: File system write failed.\n");
            }
            kfree(dl_data);
        } else {
            kprintf("Error: Download failed or timeout.\n");
        }
    }
    else if (strcmp(cmd, "update") == 0) {
        if (current_uid != 0) {
            kprintf("Error: Root privileges required. Use 'sudo update <server_ip:port>'\n");
            goto end_prompt;
        }
        if (!arg1) {
            kprintf("Usage: sudo update <server_ip:port>\n");
            kprintf("Example: sudo update 10.0.2.2:8080\n");
            goto end_prompt;
        }
        
        kprintf("========================================\n");
        kprintf("       sOS Over-The-Air Update          \n");
        kprintf("========================================\n");

        /* Parse host and optional port from arg1 (e.g. 10.0.2.2:8080) */
        char host[64];
        uint16_t port = 80;
        int idx = 0;
        while (arg1[idx] && arg1[idx] != ':' && idx < 63) {
            host[idx] = arg1[idx];
            idx++;
        }
        host[idx] = '\0';
        if (arg1[idx] == ':') {
            idx++;
            port = 0;
            while (arg1[idx] >= '0' && arg1[idx] <= '9') {
                port = port * 10 + (arg1[idx] - '0');
                idx++;
            }
        }
        
        uint8_t* sig_data = NULL;
        uint32_t sig_len = 0;
        kprintf("[1/4] Fetching kernel signature from %s:%d...\n", host, port);
        if (!http_get(host, port, "/kernel.sig", &sig_data, &sig_len)) {
            kprintf("Error: Could not download /kernel.sig from %s:%d\n", host, port);
            goto end_prompt;
        }
        
        uint8_t* bin_data = NULL;
        uint32_t bin_len = 0;
        kprintf("[2/4] Downloading kernel.bin...\n");
        if (!http_get(host, port, "/kernel.bin", &bin_data, &bin_len)) {
            kprintf("Error: Could not download /kernel.bin from %s:%d\n", host, port);
            kfree(sig_data);
            goto end_prompt;
        }
        
        kprintf("[3/4] Verifying cryptographic signature...\n");
        
        uint8_t mac[32];
        const uint8_t key[] = "sOS_super_secret_ota_key_2024";
        /* 29 is the exact length of the key string above */
        hmac_sha256(key, 29, bin_data, bin_len, mac);
        
        char mac_hex[65];
        bin_to_hex(mac, 32, mac_hex);
        
        kprintf("\n[+] Downloaded File Size: %d bytes\n", bin_len);
        
        kprintf("[+] Expected Sig : ");
        for(int j=0; j<64; j++) kputc(sig_data[j]);
        kprintf("\n");
        
        kprintf("[+] Computed Sig : %s\n", mac_hex);
        
        int match = 1;
        for (int j = 0; j < 64; j++) {
            if (mac_hex[j] != sig_data[j]) { match = 0; break; }
        }
        
        if (!match) {
            kprintf("\n\033[31mCRITICAL ERROR: Signature mismatch! Update rejected.\033[0m\n");
            kprintf("The file is corrupt, unsigned, or has been tampered with.\n");
            kfree(sig_data); kfree(bin_data);
            goto end_prompt;
        }

        kprintf("[+] Checking current version against update...\n\n");
        uint8_t* cur_bin = NULL;
        uint32_t cur_len = 0;
        if (fs_load_file("kernel.bin", &cur_bin, &cur_len)) {
            if (cur_len == bin_len) {
                int is_diff = 0;
                for (uint32_t j = 0; j < bin_len; j++) {
                    if (cur_bin[j] != bin_data[j]) { is_diff = 1; break; }
                }
                if (!is_diff) {
                    kprintf("\033[32mYour system is already up to date\033[0m\n");
                    kfree(cur_bin); kfree(sig_data); kfree(bin_data);
                    goto end_prompt;
                }
            }
            kfree(cur_bin);
        }
        
        kprintf("[4/4] Flashing new kernel to disk...\n");
        extern int fs_write_bin(const char*, uint8_t*, uint32_t);
        
        if (fs_write_bin("kernel.bin", bin_data, bin_len)) {
            kprintf("\033[32mSuccess! Firmware verified and updated.\033[0m\n");
            kprintf("\nRebooting system in 3 seconds...\n");
            
            extern void fs_touch(const char*);
            fs_touch("updt.flg");

            uint32_t start_wait = timer_ticks;
            while(timer_ticks < start_wait + 300) { __asm__ volatile("hlt"); }
            
            outb(0x64, 0xFE);
            while(1) { __asm__ volatile("cli; hlt"); }
        } else {
            kprintf("Error: Failed to write kernel to disk.\n");
        }
        
        kfree(sig_data); kfree(bin_data);
    }
    else if (strcmp(cmd, "lock") == 0) {
        clear_terminal();
        logout_and_login("Session locked.");
        return;
    }
    else {
        kprintf("Unknown command: '%s'\n", cmd);
        const char* suggest_cmd = NULL;
        /* Usage of global variable system_commands */
        for (unsigned int i = 0; i < num_system_commands; i++) {
            if (edit_distance_is_one(cmd, system_commands[i])) {
                suggest_cmd = system_commands[i];
                break;
            }
        }
        if (suggest_cmd != NULL) {
            kprintf("Did you mean '%s'?\n", suggest_cmd);
        }
    }

end_prompt:
    cmd_idx = 0; 
    cmd_buffer[0] = '\0';
    cursor_pos = 0;       /* Cleans up cursor position */
    history_index = -1;   /* Cleans up history */
    kprintf("\033[32m%s@sOS\033[0m \033[34m%s\033[0m\033[33m%c\033[0m ", current_user, get_current_path(), current_uid == 0 ? '#' : '$');
    shell_input_enabled = 1; /* Reactivates shell input */
}

void logout_and_login(const char* message) {
    session_reset = 1;   /* Invalidate current session */
    is_logging_out = 1; /* Activate logout flag */
    
    if (message != NULL) {
        kprintf("%s\n", message);
    }
    
    /* Full Shell cleanup */
    current_uid = 0;
    custom_strcpy(current_user, "root");
    cmd_idx = 0;
    cmd_buffer[0] = '\0';
    cmd_ready = 0;
    
    /* Back to root directory for security */
    fs_cd("/");
    
    /* Infinite login */
    while (1) {
        char user[32] = {0}; char pass[32] = {0}; uint8_t uid;
        kprintf("login: ");
        kernel_gets(user, 32, 0, 0); 
        
        kprintf("password: ");
        kernel_gets(pass, 32, 1, 1); 
        
        char real_pass[32] = {0};
        if (get_user_info(user, real_pass, &uid) && check_password(pass, real_pass)) {
            current_uid = uid; custom_strcpy(current_user, user);
            kprintf("\nWelcome, %s!\n", current_user);
            kprintf("Type 'help' for a list of available commands.\n");
            
            /* Auto-CD to the user's personnal directory */
            if (current_uid != 0) {
                uint8_t old_uid = current_uid;
                current_uid = 0; /* Temporarly Root */
                fs_chown(user, uid);
                fs_chmod(user, 0x30); 
                current_uid = old_uid; /* Restores original identity */
                
                fs_cd(user);
            }
            break;
        } else {
            kprintf("\nLogin incorrect.\n\n");
        }
    }
    
    is_logging_out = 0; /* Deactivate the flag */

    kprintf("\033[32m%s@sOS\033[0m \033[34m%s\033[0m\033[33m%c\033[0m ", current_user, get_current_path(), current_uid == 0 ? '#' : '$');
    shell_input_enabled = 1; 
}

void kernel_main(uint32_t magic, uint32_t mb_info_addr) {

    kprintf("========================================\n[INFO] Starting sOS...\n");

    init_gdt(); kprintf("[INFO] GDT OK\n");
    init_idt(); kprintf("[INFO] IDT OK (Exceptions OK)\n");
    init_pic(); kprintf("[INFO] PIC Reprogrammation OK\n");

    /* VARIABLES TO HOLD VIDEO INFO */
    uint64_t fb_addr = 0;
    uint32_t fb_width = 0, fb_height = 0, fb_pitch = 0;
    uint8_t fb_bpp = 0;

    if (magic == 0x2BADB002) {
        struct multiboot_info* mbi = (struct multiboot_info*)mb_info_addr;
        if (mbi->flags & 0x01) total_ram_kb = mbi->mem_lower + mbi->mem_upper;
        
        /* BIT 12 indicates that Framebuffer info is provided */
        if (mbi->flags & (1 << 12)) { 
            fb_addr = mbi->framebuffer_addr;
            fb_width = mbi->framebuffer_width;
            fb_height = mbi->framebuffer_height;
            fb_pitch = mbi->framebuffer_pitch;
            fb_bpp = mbi->framebuffer_bpp;
        }
    }

    if (total_ram_kb > 0) {
        init_pmm(total_ram_kb); kprintf("[INFO] PMM OK\n");
        init_paging(); kprintf("[INFO] Paging OK\n");
    }

    /* INITIALIZE GRAPHICS DIRECTLY AFTER PAGING */
    extern void init_graphics(uint32_t, uint32_t, uint32_t, uint32_t, uint8_t);
    init_graphics((uint32_t)fb_addr, fb_width, fb_height, fb_pitch, fb_bpp);
    
    /* We can clear the terminal (which will now clear the graphics screen) */
    clear_terminal();

    init_heap(); kprintf("[INFO] Heap (kmalloc) OK\n");
    init_fs();
    init_pit();  kprintf("[INFO] PIT (100 Hz) OK\n");
    init_e1000();
    
    init_multitasking(); kprintf("[INFO] Multitasking OK\n");

    /* Request IP from VirtualBox/QEMU DHCP Server */
    extern void net_dhcp_discover(void);
    net_dhcp_discover();

    /* --- Block OS boot until DHCP completes or times out --- */
    extern uint8_t sOS_ip[4];
    uint32_t timeout = timer_ticks + 300; /* 3 seconds timeout (100 Hz = 300 ticks) */
    while (sOS_ip[0] == 0 && timer_ticks < timeout) {
        __asm__ volatile("hlt"); /* Yield CPU while waiting for network interrupt */
    }

    if (sOS_ip[0] == 0) {
        kprintf("[WARNING] DHCP Timeout! Network might be unavailable.\n");
    }

    kprintf("[INFO] Booting finished without errors!\n");
    kprintf("========================================\n");
    kprintf("         Welcome to SimpleOS v%s\n", SOS_VERSION);
    kprintf("========================================\n\n");
    
    uint8_t* flg_buf = NULL;
    uint32_t flg_size = 0;
    if (fs_load_file("updt.flg", &flg_buf, &flg_size)) {
        kfree(flg_buf);
        extern void fs_rm(const char*, const char*);
        fs_rm("updt.flg", NULL); /* Delete the flag so it only shows once */
        
        kprintf("\033[32m"); /* Switch to Green */
        kprintf("========================================\n");
        kprintf("  SYSTEM UPDATE APPLIED SUCCESSFULLY!   \n");
        kprintf("========================================\n");
        kprintf("\033[0m\n"); /* Reset color */
        
        uint32_t wait_ticks = timer_ticks;
        while(timer_ticks < wait_ticks + 200) { __asm__ volatile("hlt"); }
    }

    logout_and_login(NULL);

    while(1) {
        if (cmd_ready) {
            session_reset = 0;
            execute_command();
            cmd_ready = 0;
        }
        __asm__ volatile ("hlt");
    }
}