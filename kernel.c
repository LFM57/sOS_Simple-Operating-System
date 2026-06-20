#include "system.h"
#include "elf.h"

extern int fs_load_file(const char*, uint8_t**, uint32_t*);

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
    "nano", "passwd", "cp", "mv", "hex"
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

void execute_command(void);

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
    if (tasks[id].page_directory != kernel_page_directory && tasks[id].page_directory != NULL) {
        uint32_t* pd = tasks[id].page_directory;
        for (int i = 31; i < 1024; i++) {
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

/* Check if the password matches the stored hash */
int check_password(const char* input_pass, const char* stored_hash) {
    uint32_t hash = fnv1a_hash(input_pass);
    char hash_str[9];
    hash_to_str(hash, hash_str);
    return (strcmp(hash_str, stored_hash) == 0);
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

/* NANO LIGHT */

char edit_lines[22][80];
int edit_lens[22];
int edit_cx = 0;
int edit_cy = 0;

/* Draw the complete editor on the VGA screen */
void draw_editor(const char* filename) {
    uint16_t* vmem = (uint16_t*)0xB8000;
    
    /* 1. Header Bar (Line 0) - Inverted colors (Black on Gray: 0x70) */
    char header[80];
    for(int i = 0; i < 80; i++) header[i] = ' ';
    char* title = "  sOS Nano Light v2.5 -- File: ";
    int idx = 0;
    while(title[idx]) { header[idx] = title[idx]; idx++; }
    int f_idx = 0;
    while(filename[f_idx] && idx < 75) { header[idx++] = filename[f_idx++]; }
    
    for(int i = 0; i < 80; i++) {
        vmem[i] = (uint16_t)header[i] | 0x7000;
    }
    
    /* 2. Editing Area (Lines 1 to 22) - White on Black (0x0F) */
    for (int l = 0; l < 22; l++) {
        for (int c = 0; c < 80; c++) {
            char char_to_draw = ' ';
            if (c < edit_lens[l]) {
                char_to_draw = edit_lines[l][c];
            }
            vmem[(l + 1) * 80 + c] = (uint16_t)char_to_draw | 0x0F00;
        }
    }
    
    /* 3. Status Line (Line 23) - Empty */
    for (int i = 0; i < 80; i++) {
        vmem[23 * 80 + i] = ' ' | 0x0F00;
    }
    
    /* 4. Shortcut Bar (Line 24) - Inverted colors (0x70) */
    char footer[80];
    for(int i = 0; i < 80; i++) footer[i] = ' ';
    char* f_text = "  ^S: Save and Write    ^X: Quit Nano";
    int f_text_idx = 0;
    while(f_text[f_text_idx]) { footer[f_text_idx] = f_text[f_text_idx]; f_text_idx++; }
    
    for(int i = 0; i < 80; i++) {
        vmem[24 * 80 + i] = (uint16_t)footer[i] | 0x7000;
    }
    
    /* Hardware cursor positioning (x, y + 1 due to the header line) */
    extern void update_cursor(int, int);
    update_cursor(edit_cx, edit_cy + 1);
}

void run_nano(const char* filename) {
    /* Clean up the editor memory */
    for(int l = 0; l < 22; l++) {
        for(int c = 0; c < 80; c++) edit_lines[l][c] = '\0';
        edit_lens[l] = 0;
    }
    edit_cx = 0; edit_cy = 0;
    
    /* Load the file if it exists on disk */
    uint8_t* buf = NULL;
    uint32_t size = 0;
    if (fs_load_file(filename, &buf, &size)) {
        int l = 0, c = 0;
        for (uint32_t i = 0; i < size && l < 22; i++) {
            if (buf[i] == '\n') {
                edit_lines[l][c] = '\0';
                edit_lens[l] = c;
                l++; c = 0;
            } else if (buf[i] == '\r') {
                /* Ignore */
            } else {
                if (c < 79) {
                    edit_lines[l][c++] = buf[i];
                }
            }
        }
        if (l < 22 && c > 0) {
            edit_lines[l][c] = '\0';
            edit_lens[l] = c;
        }
        kfree(buf);
    } else {
        /* Block if reading is forbidden */
        if (fs_error == FS_ERR_PERMISSION) {
            kprintf("Error: Permission denied (Read access required).\n");
            return;
        }
    }
    
    while (1) {
        draw_editor(filename);
        char c = wait_key();
        
        if (c == 24) { /* Ctrl+X -> Quit */
            break;
        }
        else if (c == 19) { /* Ctrl+S -> Save */
            /* Search for the last non-empty line to avoid saving unnecessary blank lines */
            int last_non_empty = -1;
            for (int l = 21; l >= 0; l--) {
                if (edit_lens[l] > 0) {
                    last_non_empty = l;
                    break;
                }
            }
            
            char out_buf[2000];
            int out_idx = 0;
            /* If the entire file is empty, write nothing. Otherwise, stop at the last active line */
            int max_line_to_write = (last_non_empty == -1) ? 0 : last_non_empty;
            
            for (int l = 0; l <= max_line_to_write; l++) {
                for (int col = 0; col < edit_lens[l]; col++) {
                    out_buf[out_idx++] = edit_lines[l][col];
                }
                out_buf[out_idx++] = '\n';
            }
            out_buf[out_idx] = '\0';
            
            fs_write(filename, out_buf);
            
            /* Green visual confirmation on the status line */
            uint16_t* vmem = (uint16_t*)0xB8000;
            char* save_msg = "  [ File successfully saved! ]  ";
            int s_idx = 0;
            while(save_msg[s_idx]) {
                vmem[23 * 80 + 20 + s_idx] = (uint16_t)save_msg[s_idx] | 0x0A00; /* Green on Black */
                s_idx++;
            }
            
            /* PIT Usage for a pause of precisely 1.5s */
            uint32_t start_ticks = timer_ticks;
            while (timer_ticks < start_ticks + 150) {
                __asm__ volatile("hlt"); /* Put the processor to sleep while waiting for timer interrupts */
            }
        }
        else if (c == 17) {
            if (edit_cy > 0) {
                edit_cy--;
                if (edit_cx > edit_lens[edit_cy]) edit_cx = edit_lens[edit_cy];
            }
        }
        else if (c == 18) {
            if (edit_cy < 21) {
                edit_cy++;
                if (edit_cx > edit_lens[edit_cy]) edit_cx = edit_lens[edit_cy];
            }
        }
        else if (c == 20) {
            if (edit_cx > 0) {
                edit_cx--;
            } else if (edit_cy > 0) {
                edit_cy--;
                edit_cx = edit_lens[edit_cy];
            }
        }
        else if (c == 21) {
            if (edit_cx < edit_lens[edit_cy]) {
                edit_cx++;
            } else if (edit_cy < 21) {
                edit_cy++;
                edit_cx = 0;
            }
        }
        else if (c == '\n' || c == '\r') { /* Enter -> Next line */
            if (edit_cy < 21) {
                edit_cy++;
                edit_cx = 0;
            }
        }
        else if (c == '\b' || c == 127) { /* Backspace */
            if (edit_cx > 0) {
                /* Moves characters one step left */
                for (int col = edit_cx - 1; col < edit_lens[edit_cy]; col++) {
                    edit_lines[edit_cy][col] = edit_lines[edit_cy][col + 1];
                }
                edit_cx--;
                edit_lens[edit_cy]--;
            } else if (edit_cy > 0) {
                /* Merge the current line with the previous line if there is enough space */
                int prev_len = edit_lens[edit_cy - 1];
                int curr_len = edit_lens[edit_cy];
                if (prev_len + curr_len < 80) {
                    for (int col = 0; col < curr_len; col++) {
                        edit_lines[edit_cy - 1][prev_len + col] = edit_lines[edit_cy][col];
                    }
                    edit_lens[edit_cy - 1] += curr_len;
                    /* Shift all lines below upwards */
                    for (int l = edit_cy; l < 21; l++) {
                        for(int col = 0; col < 80; col++) edit_lines[l][col] = edit_lines[l + 1][col];
                        edit_lens[l] = edit_lens[l + 1];
                    }
                    for(int col = 0; col < 80; col++) edit_lines[21][col] = '\0';
                    edit_lens[21] = 0;
                    
                    edit_cy--;
                    edit_cx = prev_len;
                }
            }
        }
        else if (c >= 32 && c <= 126) { /* Printable character */
            if (edit_lens[edit_cy] < 79) {
                /* Insertion: shift characters on the right to the right */
                for (int col = edit_lens[edit_cy]; col > edit_cx; col--) {
                    edit_lines[edit_cy][col] = edit_lines[edit_cy][col - 1];
                }
                edit_lines[edit_cy][edit_cx] = c;
                edit_cx++;
                edit_lens[edit_cy]++;
            }
        }
    }
    
    clear_terminal(); /* Restores clean shell screen on exit */
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
        kprintf("Available commands:\n  help      clear    uptime\n  ram       ls       cd\n  cat       mkdir    rm\n  touch     write    loop\n  run       df       ps\n  kill      about    shutdown\n  whoami    chown    chmod\n  su        sudo     userlist\n  nano      passwd   cp\n  mv        hex\n");
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
        kprintf("sOS (Simple Operating System) - v2.3\n");
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
            
            kprintf("UID | Username    | Password Hash\n");
            kprintf("---------------------------------\n");
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
                if (current_uid == 0) kprintf(" | %s\n", p);
                else kprintf(" | [HIDDEN]\n");
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

            /* Serches for the next available UID */
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

            /* Password Hashing */
            uint32_t hash = fnv1a_hash(password);
            char hash_str[9]; hash_to_str(hash, hash_str);

            /* Creating new line in passwd */
            char new_line[128]; int nl_len = 0;
            char* u_ptr = username; while(*u_ptr) new_line[nl_len++] = *u_ptr++;
            new_line[nl_len++] = ':';
            char* h_ptr = hash_str; while(*h_ptr) new_line[nl_len++] = *h_ptr++;
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
        
        /* 4. Hash calculate and write */
        uint32_t hash = fnv1a_hash(new_pass);
        char hash_str[9];
        hash_to_str(hash, hash_str);
        
        if (update_user_password(target, hash_str)) {
            kprintf("passwd: password updated successfully.\n");
        } else {
            kprintf("Error: User '%s' not found.\n", target);
        }
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
    clear_terminal();
    kprintf("========================================\n[INFO] Starting sOS...\n");

    init_gdt(); kprintf("[INFO] GDT OK\n");
    init_idt(); kprintf("[INFO] IDT OK (Exceptions OK)\n");
    init_pic(); kprintf("[INFO] PIC Reprogrammation OK\n");

    if (magic == 0x2BADB002) {
        struct multiboot_info* mbi = (struct multiboot_info*)mb_info_addr;
        if (mbi->flags & 0x01) total_ram_kb = mbi->mem_lower + mbi->mem_upper;
    }

    if (total_ram_kb > 0) {
        init_pmm(total_ram_kb); kprintf("[INFO] PMM OK\n");
        init_paging(); kprintf("[INFO] Paging OK\n");
    }

    init_heap(); kprintf("[INFO] Heap (kmalloc) OK\n");
    init_fs();
    init_pit();  kprintf("[INFO] PIT (100 Hz) OK\n");
    
    init_multitasking(); kprintf("[INFO] Multitasking OK\n");

    __asm__ volatile ("sti");

    kprintf("[INFO] Booting finished without errors!\n");
    kprintf("========================================\n");
    kprintf("         Welcome to SimpleOS\n");
    kprintf("========================================\n\n");
    
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