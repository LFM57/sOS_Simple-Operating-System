#include "system.h"
#include "net.h"

extern void gdt_flush(uint32_t);
extern void idt_flush(uint32_t);
extern uint32_t isr_stub_table[];
extern void irq0(void);
extern void irq1(void);
extern void irq4(void);
extern void irq7(void);
extern void irq15(void);
extern void isr128(void);

/* --- GDT & TSS --- */
struct gdt_entry { uint16_t limit_low; uint16_t base_low; uint8_t base_middle; uint8_t access; uint8_t granularity; uint8_t base_high; } __attribute__((packed));
struct gdt_ptr { uint16_t limit; uint32_t base; } __attribute__((packed));

/* TSS Structure (Task State Segment) */
struct tss_entry {
    uint32_t prev_tss;
    uint32_t esp0; /* Ring 0 (Kernel) stack used during interrupts */
    uint32_t ss0;  /* Ring 0 stack segment */
    uint32_t esp1; uint32_t ss1; uint32_t esp2; uint32_t ss2;
    uint32_t cr3, eip, eflags, eax, ecx, edx, ebx, esp, ebp, esi, edi;
    uint32_t es, cs, ss, ds, fs, gs, ldt;
    uint16_t trap, iomap_base;
} __attribute__((packed));

struct gdt_entry gdt[6];
struct gdt_ptr gp;
struct tss_entry tss;

extern task_t tasks[MAX_TASKS];
extern int current_task;

/* Function to update the TSS kernel stack on the fly */
void set_kernel_stack(uint32_t stack) {
    tss.esp0 = stack;
}

void gdt_set_gate(int num, uint32_t base, uint32_t limit, uint8_t access, uint8_t gran) {
    gdt[num].base_low = (base & 0xFFFF); gdt[num].base_middle = (base >> 16) & 0xFF; gdt[num].base_high = (base >> 24) & 0xFF;
    gdt[num].limit_low = (limit & 0xFFFF); gdt[num].granularity = (limit >> 16) & 0x0F;
    gdt[num].granularity |= gran & 0xF0; gdt[num].access = access;
}

void init_gdt() {
    gp.limit = (sizeof(struct gdt_entry) * 6) - 1;
    gp.base = (uint32_t)&gdt;

    /* Entry 0: Null (Mandatory) */
    gdt_set_gate(0, 0, 0, 0, 0);
    
    /* Entry 1: Ring 0 Code (Kernel) */
    gdt_set_gate(1, 0, 0xFFFFFFFF, 0x9A, 0xCF);
    
    /* Entry 2: Ring 0 Data (Kernel) */
    gdt_set_gate(2, 0, 0xFFFFFFFF, 0x92, 0xCF);
    
    /* Entry 3: Ring 3 Code (User) */
    /* 'FA' = '9A' with privilege level set to 3 */
    gdt_set_gate(3, 0, 0xFFFFFFFF, 0xFA, 0xCF);
    
    /* Entry 4: Ring 3 Data (User) */
    /* 'F2' = '92' with privilege level set to 3 */
    gdt_set_gate(4, 0, 0xFFFFFFFF, 0xF2, 0xCF);

    /* Entry 5: The TSS (Task State Segment) */
    /* 1. Initialize the TSS structure with zeros */
    uint8_t *tss_ptr = (uint8_t*)&tss;
    for(uint32_t i = 0; i < sizeof(struct tss_entry); i++) tss_ptr[i] = 0;
    
    /* 2. Basic TSS parameters */
    tss.ss0 = 0x10; /* Kernel data segment (Entry 2 * 8 = 0x10) */
    tss.esp0 = 0;   /* Will be filled just before jumping to Ring 3 */
    tss.iomap_base = sizeof(struct tss_entry);
    
    /* 3. Declare the TSS in the GDT */
    uint32_t tss_base = (uint32_t)&tss;
    uint32_t tss_limit = sizeof(struct tss_entry) - 1;
    /* Access 0x89 = Present, Ring 0, 32-bit TSS Type */
    /* Granularity 0x40 = 32-bit Flag (no paging) */
    gdt_set_gate(5, tss_base, tss_limit, 0x89, 0x40);

    /* Load the new GDT (Assembly) */
    gdt_flush((uint32_t)&gp);
    
    /* Inform the processor where the TSS is located */
    /* The ltr (Load Task Register) instruction takes the offset of the TSS entry. */
    /* Entry 5 * 8 bytes per entry = 40 (0x28 in hex) */
    __asm__ volatile("ltr %%ax" : : "a" (0x28));
}

/* --- IDT --- */
struct idt_entry { uint16_t base_low; uint16_t sel; uint8_t always0; uint8_t flags; uint16_t base_high; } __attribute__((packed));
struct idt_ptr { uint16_t limit; uint32_t base; } __attribute__((packed));
struct idt_entry idt[256]; struct idt_ptr idtp;

void idt_set_gate(uint8_t num, uint32_t base, uint16_t sel, uint8_t flags) {
    idt[num].base_low = base & 0xFFFF; idt[num].base_high = (base >> 16) & 0xFFFF;
    idt[num].sel = sel; idt[num].always0 = 0; idt[num].flags = flags;
}

void init_idt() {
    idtp.limit = (sizeof(struct idt_entry) * 256) - 1; idtp.base = (uint32_t)&idt;
    for (int i = 0; i < 256; i++) idt_set_gate(i, 0, 0, 0);

    for (int i = 0; i < 32; i++) idt_set_gate(i, isr_stub_table[i], 0x08, 0x8E);

    idt_set_gate(32, (uint32_t)irq0, 0x08, 0x8E); 
    idt_set_gate(33, (uint32_t)irq1, 0x08, 0x8E); 
    idt_set_gate(36, (uint32_t)irq4, 0x08, 0x8E); 
    idt_set_gate(39, (uint32_t)irq7, 0x08, 0x8E); 
    idt_set_gate(47, (uint32_t)irq15, 0x08, 0x8E); 
    /* 0xEE because the high E allows Ring 3 */
    idt_set_gate(128, (uint32_t)isr128, 0x08, 0xEE); 
    idt_flush((uint32_t)&idtp);
}

/* --- EXCEPTIONS AND SYSCALLS --- */
typedef struct {
    uint32_t gs, fs, es, ds;
    uint32_t edi, esi, ebp, esp, ebx, edx, ecx, eax;
    uint32_t int_no, err_code;
    uint32_t eip, cs, eflags, useresp, ss;
} registers_t;

const char *exception_messages[] = { "Division By Zero", "Debug", "NMI", "Breakpoint", "Overflow", "Out of Bounds", "Invalid Opcode", "No Coprocessor", "Double Fault", "Coprocessor Segment Overrun", "Bad TSS", "Segment Not Present", "Stack Fault", "General Protection Fault", "Page Fault", "Unknown Interrupt", "Coprocessor Fault", "Alignment Check", "Machine Check", "SIMD Floating-Point", "Virtualization", "Control Protection", "Reserved", "Reserved", "Reserved", "Reserved", "Reserved", "Reserved", "Reserved", "Reserved", "Reserved", "Reserved" };

void fault_handler(registers_t *regs) {
    if (regs->int_no < 32) {
        // The CS register contains the privilege level in its 2 least significant bits.
        if ((regs->cs & 0x03) == 3) {
            kprintf("\n[OS] User app crashed! (Exception %d: %s). Killing task...\n", regs->int_no, exception_messages[regs->int_no]);
            extern void kill_current_task();
            kill_current_task(); // Kills app and goes back to shell
            return;
        }

        /* --- GRAPHICAL BSOD TRIGGER --- */
        if (is_graphics_mode) {
            show_cursor = 0;            /* Erase and hide the blinking cursor */
            term_bg_color = 0x000000AA; /* BSOD Blue */
            term_fg_color = 0x00FFFFFF; /* White text */
            clear_terminal();
            
            /* Dynamically calculate coordinates */
            int cols = g_width / 8;
            int rows = g_height / 16;
            int cx = cols / 2;
            int cy = rows / 2;

            /* Line 1: Top Border (50 chars) */
            cursor_x = cx - 25; cursor_y = cy - 10;
            kprintf("==================================================");

            /* Line 2: Title (50 chars) */
            cursor_x = cx - 25; cursor_y = cy - 9;
            kprintf("         sOS FATAL ERROR / KERNEL PANIC           ");

            /* Line 3: Bottom Border (50 chars) */
            cursor_x = cx - 25; cursor_y = cy - 8;
            kprintf("==================================================");

            /* Line 4: Message 1 (43 chars) */
            cursor_x = cx - 21; cursor_y = cy - 5;
            kprintf("An unrecoverable system error has occurred.");

            /* Line 5: Message 2 (47 chars) */
            cursor_x = cx - 23; cursor_y = cy - 4;
            kprintf("The kernel has halted safely to prevent damage.");

            /* Line 6: Exception Details Block (Left-aligned, centered as a unit) */
            cursor_x = cx - 18; cursor_y = cy - 1;
            kprintf("EXCEPTION  : %s (Code: %d)", exception_messages[regs->int_no], regs->int_no);

            /* Line 7: EIP */
            cursor_x = cx - 18; cursor_y = cy;
            kprintf("EIP        : 0x%x", regs->eip);

            /* Line 8: Error Code */
            cursor_x = cx - 18; cursor_y = cy + 1;
            kprintf("ERROR CODE : 0x%x", regs->err_code);

            /* Line 9: Warning Border (50 chars) */
            cursor_x = cx - 25; cursor_y = cy + 4;
            kprintf("==================================================");

            /* Line 10: Restart Message (38 chars) */
            cursor_x = cx - 19; cursor_y = cy + 5;
            kprintf("Please manually restart your computer.");
        } else {
            kprintf("\n[KERNEL PANIC] EXCEPTION %d : %s\n", regs->int_no, exception_messages[regs->int_no]);
            kprintf("SYSTEM STOPPED.\n");
        }
        while(1) { __asm__ volatile ("cli; hlt"); }
    }
}

uint32_t syscall_handler(uint32_t syscall_num, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    (void)arg2; (void)arg3;

    if (syscall_num == 1) {
        /* Syscall 1: int write(int fd, const char* buf, uint32_t count) */
        int fd = (int)arg1;
        const char* buf = (const char*)arg2;
        uint32_t count = (uint32_t)arg3;

        /* Security: Complete validation of the user memory area */
        if (!is_valid_user_range((uint32_t)buf, count)) {
            kprintf("\n[SEC] Process terminated: Segmentation Fault in sys_write (0x%x)\n", buf);
            extern void kill_current_task();
            kill_current_task();
            return 0;
        }

        /* Verification: Is the file descriptor valid and open? */
        if (fd < 0 || fd >= MAX_OPEN_FILES || !tasks[current_task].open_files[fd].is_used) {
            return (uint32_t)-1; /* Error Code */
        }

        /* Write via VFS (TTY, File, Socket, or Pipe) */
        file_descriptor_t* fd_entry = &tasks[current_task].open_files[fd];
        if (!fd_entry->node) {
            return (uint32_t)-1;
        }
        uint32_t acc = fd_entry->flags & 0x03;
        if (fd_entry->type != TYPE_TTY && acc != O_WRONLY && acc != O_RDWR) {
            return (uint32_t)-1;
        }
        if (fd_entry->node->type == VFS_FILE && (fd_entry->flags & O_APPEND)) {
            fd_entry->offset = fd_entry->node->size;
        }
        int bytes = vfs_write(fd_entry->node, fd_entry->offset, count, (const uint8_t*)buf);
        if (bytes > 0 && fd_entry->node->type == VFS_FILE) {
            fd_entry->offset += (uint32_t)bytes;
        }
        return (uint32_t)bytes;
    }
    else if (syscall_num == 2) {
        extern void kill_current_task();
        kill_current_task();
        return 0;
    } 
    else if (syscall_num == 3) {
        extern uint32_t sys_sbrk(int);
        return sys_sbrk((int)arg1);
    }
    else if (syscall_num == 4) {
        /* Syscall 4: int read(int fd, char* buf, uint32_t count) */
        int fd = (int)arg1;
        char* buf = (char*)arg2;
        uint32_t count = (uint32_t)arg3;

        /* Security: Complete validation of the user write space */
        if (!is_valid_user_range((uint32_t)buf, count)) {
            kprintf("\n[SEC] Process terminated: Segmentation Fault in sys_read (0x%x)\n", buf);
            extern void kill_current_task();
            kill_current_task();
            return 0;
        }

        /* Verification of the file descriptor */
        if (fd < 0 || fd >= MAX_OPEN_FILES || !tasks[current_task].open_files[fd].is_used) {
            return (uint32_t)-1;
        }

        /* Read via VFS (TTY, File, Socket, or Pipe) */
        file_descriptor_t* fd_entry = &tasks[current_task].open_files[fd];
        if (!fd_entry->node) {
            return (uint32_t)-1;
        }
        uint32_t acc = fd_entry->flags & 0x03;
        if (fd_entry->type != TYPE_TTY && acc != O_RDONLY && acc != O_RDWR) {
            return (uint32_t)-1;
        }
        int bytes = vfs_read(fd_entry->node, fd_entry->offset, count, (uint8_t*)buf);
        if (bytes > 0 && fd_entry->node->type == VFS_FILE) {
            fd_entry->offset += (uint32_t)bytes;
        }
        return (uint32_t)bytes;
    }
    else if (syscall_num == 5) {
        extern volatile uint32_t timer_ticks;
        return timer_ticks;
    }
    else if (syscall_num == 6) {
        /* Syscall 6: int read_file(const char* filename, uint8_t* buffer, uint32_t* size_in_out) */
        const char* filename = (const char*)arg1;
        uint8_t* out_buffer = (uint8_t*)arg2;
        uint32_t* out_size = (uint32_t*)arg3;

        /* Validation of size address */
        if (!is_valid_user_range((uint32_t)out_size, sizeof(uint32_t))) {
            kprintf("\n[SEC] Process terminated: Segmentation Fault in read_file (out_size)\n");
            extern void kill_current_task();
            kill_current_task();
            return 0;
        }

        uint32_t max_size = *out_size;

        /* Validation of the filename string */
        if (!is_valid_user_string(filename)) {
            kprintf("\n[SEC] Process terminated: Segmentation Fault in read_file (filename)\n");
            extern void kill_current_task();
            kill_current_task();
            return 0;
        }

        /* Validation of the entire buffer with the specified max size */
        if (!is_valid_user_range((uint32_t)out_buffer, max_size)) {
            kprintf("\n[SEC] Process terminated: Segmentation Fault in read_file (out_buffer)\n");
            extern void kill_current_task();
            kill_current_task();
            return 0;
        }

        uint8_t* file_data = NULL;
        uint32_t file_size = 0;
        extern int fs_load_file(const char* filename, uint8_t** buffer_out, uint32_t* size_out);
        
        if (fs_load_file(filename, &file_data, &file_size)) {
            /* Truncated copy to the maximum available user buffer */
            uint32_t copy_size = (file_size > max_size) ? max_size : file_size;
            for (uint32_t i = 0; i < copy_size; i++) {
                out_buffer[i] = file_data[i];
            }
            *out_size = copy_size; // Returns the actual size written
            kfree(file_data); 
            return 1; 
        }
        return 0; 
    }
    else if (syscall_num == 7) {
        /* Syscall 7: int write_file(const char* filename, const char* text) */
        const char* filename = (const char*)arg1;
        const char* text = (const char*)arg2;

        /* Rigorous validation of both strings */
        if (!is_valid_user_string(filename) || !is_valid_user_string(text)) {
            kprintf("\n[SEC] Process terminated: Segmentation Fault in write_file\n");
            extern void kill_current_task();
            kill_current_task();
            return 0;
        }

        extern int fs_write(const char* filename, const char* text);
        return fs_write(filename, text); 
    }
    else if (syscall_num == 8) {
        /* Syscall 8: int socket() */
        for (int i = 3; i < MAX_OPEN_FILES; i++) {
            if (!tasks[current_task].open_files[i].is_used) {
                int sock_idx = net_alloc_socket(1);
                if (sock_idx == -1) return (uint32_t)-1;

                vfs_node_t* node = vfs_create_socket_node(sock_idx);
                if (!node) {
                    extern socket_t sockets[];
                    sockets[sock_idx].is_used = 0;
                    return (uint32_t)-1;
                }
                
                tasks[current_task].open_files[i].is_used = 1;
                tasks[current_task].open_files[i].type = TYPE_SOCKET;
                tasks[current_task].open_files[i].flags = O_RDWR;
                tasks[current_task].open_files[i].offset = (uint32_t)sock_idx;
                tasks[current_task].open_files[i].node = node;
                return i;
            }
        }
        return (uint32_t)-1;
    }
    else if (syscall_num == 9) {
        /* Syscall 9: int bind(int fd, uint16_t port) */
        int fd = (int)arg1;
        uint16_t port = (uint16_t)arg2;
        
        if (fd < 0 || fd >= MAX_OPEN_FILES || !tasks[current_task].open_files[fd].is_used) return (uint32_t)-1;
        vfs_node_t* node = tasks[current_task].open_files[fd].node;
        if (!node || node->type != VFS_SOCKET) return (uint32_t)-1;
        
        int sock_idx = (int)(uintptr_t)node->private_data;
        extern void net_bind_socket(int, uint16_t);
        net_bind_socket(sock_idx, port);
        return 0;
    }
    else if (syscall_num == 10) {
        /* Syscall 10: int sendto(int fd, sendto_args_t* args) */
        int fd = (int)arg1;
        sendto_args_t* args = (sendto_args_t*)arg2;
        
        /* Security Checks */
        if (!is_valid_user_range((uint32_t)args, sizeof(sendto_args_t))) return (uint32_t)-1;
        if (!is_valid_user_range((uint32_t)args->payload, args->len)) return (uint32_t)-1;
        if (fd < 0 || fd >= MAX_OPEN_FILES || !tasks[current_task].open_files[fd].is_used) return (uint32_t)-1;
        vfs_node_t* node = tasks[current_task].open_files[fd].node;
        if (!node || node->type != VFS_SOCKET) return (uint32_t)-1;

        int sock_idx = (int)(uintptr_t)node->private_data;
        extern socket_t sockets[];
        for (int i = 0; i < 4; i++) sockets[sock_idx].remote_ip[i] = args->dest_ip[i];
        sockets[sock_idx].remote_port = args->dest_port;
        extern void net_send_udp(int, uint8_t*, uint16_t, uint8_t*, uint16_t);
        net_send_udp(sock_idx, args->dest_ip, args->dest_port, args->payload, args->len);
        return args->len;
    }
    else if (syscall_num == 11) {
        /* Syscall 11: int recvfrom(int fd, uint8_t* buf, uint32_t max_len) */
        int fd = (int)arg1;
        uint8_t* buf = (uint8_t*)arg2;
        uint32_t max_len = (uint32_t)arg3;
        
        /* Security Checks */
        if (!is_valid_user_range((uint32_t)buf, max_len)) return (uint32_t)-1;
        if (fd < 0 || fd >= MAX_OPEN_FILES || !tasks[current_task].open_files[fd].is_used) return (uint32_t)-1;
        vfs_node_t* node = tasks[current_task].open_files[fd].node;
        if (!node || node->type != VFS_SOCKET) return (uint32_t)-1;

        return (uint32_t)vfs_read(node, 0, max_len, buf);
    }
    else if (syscall_num == 12) {
        /* Syscall 12: int get_dns_ip(uint8_t* buf) */
        uint8_t* buf = (uint8_t*)arg1;
        if (!is_valid_user_range((uint32_t)buf, 4)) return 0;
        
        extern uint8_t sOS_dns[4];
        for (int i = 0; i < 4; i++) buf[i] = sOS_dns[i];
        return 1;
    }
    else if (syscall_num == 13) {
        /* Syscall 13: int socket_tcp() */
        for (int i = 3; i < MAX_OPEN_FILES; i++) {
            if (!tasks[current_task].open_files[i].is_used) {
                int sock_idx = net_alloc_socket(2); /* Type 2 = TCP */
                if (sock_idx == -1) return (uint32_t)-1;

                vfs_node_t* node = vfs_create_socket_node(sock_idx);
                if (!node) {
                    extern socket_t sockets[];
                    sockets[sock_idx].is_used = 0;
                    return (uint32_t)-1;
                }
                
                tasks[current_task].open_files[i].is_used = 1;
                tasks[current_task].open_files[i].type = TYPE_SOCKET;
                tasks[current_task].open_files[i].flags = O_RDWR;
                tasks[current_task].open_files[i].offset = (uint32_t)sock_idx;
                tasks[current_task].open_files[i].node = node;
                return i;
            }
        }
        return (uint32_t)-1;
    }
    else if (syscall_num == 14) {
        /* Syscall 14: int connect(int fd, uint8_t* dest_ip, uint16_t port) */
        int fd = (int)arg1;
        uint8_t* dest_ip = (uint8_t*)arg2;
        uint16_t port = (uint16_t)arg3;
        
        if (!is_valid_user_range((uint32_t)dest_ip, 4)) return (uint32_t)-1;
        if (fd < 0 || fd >= MAX_OPEN_FILES || !tasks[current_task].open_files[fd].is_used) return (uint32_t)-1;
        vfs_node_t* node = tasks[current_task].open_files[fd].node;
        if (!node || node->type != VFS_SOCKET) return (uint32_t)-1;

        int sock_idx = (int)(uintptr_t)node->private_data;
        extern socket_t sockets[];
        extern volatile uint32_t timer_ticks;
        
        /* Auto-bind a random local port if not done */
        if (sockets[sock_idx].local_port == 0) {
            sockets[sock_idx].local_port = 50000 + sock_idx;
        }
        
        for(int i=0; i<4; i++) sockets[sock_idx].remote_ip[i] = dest_ip[i];
        sockets[sock_idx].remote_port = port;
        if (sockets[sock_idx].type == 1) {
            return 0;
        }

        sockets[sock_idx].seq_num = timer_ticks * 100; /* Weak random ISN */
        sockets[sock_idx].ack_num = 0;
        sockets[sock_idx].tcp_state = TCP_SYN_SENT;
        
        extern void net_send_tcp_raw(int, uint8_t, uint8_t*, uint16_t);
        net_send_tcp_raw(sock_idx, 0x02, NULL, 0); /* Send SYN */

        /* Re-enable hardware interrupts so the Timer and Network Card can wake up the CPU */
        __asm__ volatile ("sti");
        
        /* Block until Established or Timeout (5 sec) */
        uint32_t start = timer_ticks;
        while(sockets[sock_idx].tcp_state != TCP_ESTABLISHED) {
            if (timer_ticks > start + 500) return (uint32_t)-1; 
            __asm__ volatile ("hlt");
        }
        return 0;
    }
    else if (syscall_num == 15) {
        /* Syscall 15: int send(int fd, void* buf, uint32_t len) -> thin wrapper over write */
        return syscall_handler(1, arg1, arg2, arg3);
    }
    else if (syscall_num == 16) {
        /* Syscall 16: int recv(int fd, void* buf, uint32_t max_len) -> thin wrapper over read */
        return syscall_handler(4, arg1, arg2, arg3);
    }
    else if (syscall_num == 17) {
        /* Syscall 17: int close(int fd) */
        int fd = (int)arg1;
        if (fd < 0 || fd >= MAX_OPEN_FILES || !tasks[current_task].open_files[fd].is_used) {
            return (uint32_t)-1;
        }
        if (tasks[current_task].open_files[fd].node != NULL) {
            vfs_close(tasks[current_task].open_files[fd].node);
        }
        tasks[current_task].open_files[fd].is_used = 0;
        tasks[current_task].open_files[fd].type = TYPE_NONE;
        tasks[current_task].open_files[fd].flags = 0;
        tasks[current_task].open_files[fd].offset = 0;
        tasks[current_task].open_files[fd].node = NULL;
        return 0;
    }
    else if (syscall_num == 18) {
        /* Syscall 18: int open(const char* pathname, int flags) */
        const char* pathname = (const char*)arg1;
        uint32_t flags = (uint32_t)arg2;

        if (!is_valid_user_string(pathname)) {
            return (uint32_t)-1;
        }

        int fd = -1;
        for (int i = 3; i < MAX_OPEN_FILES; i++) {
            if (!tasks[current_task].open_files[i].is_used) {
                fd = i;
                break;
            }
        }
        if (fd == -1) {
            return (uint32_t)-1;
        }

        vfs_node_t* node = vfs_open(pathname, flags);
        if (!node) {
            return (uint32_t)-1;
        }

        tasks[current_task].open_files[fd].is_used = 1;
        tasks[current_task].open_files[fd].type = TYPE_FILE;
        tasks[current_task].open_files[fd].flags = flags;
        tasks[current_task].open_files[fd].offset = (flags & O_APPEND) ? node->size : 0;
        tasks[current_task].open_files[fd].node = node;
        return (uint32_t)fd;
    }
    else if (syscall_num == 19) {
        /* Syscall 19: int lseek(int fd, int offset, int whence) */
        int fd = (int)arg1;
        int32_t offset = (int32_t)arg2;
        int whence = (int)arg3;

        if (fd < 0 || fd >= MAX_OPEN_FILES || !tasks[current_task].open_files[fd].is_used) {
            return (uint32_t)-1;
        }
        if (tasks[current_task].open_files[fd].type != TYPE_FILE || !tasks[current_task].open_files[fd].node) {
            return (uint32_t)-1;
        }

        int32_t new_offset = 0;
        if (whence == SEEK_SET) {
            new_offset = offset;
        } else if (whence == SEEK_CUR) {
            new_offset = (int32_t)tasks[current_task].open_files[fd].offset + offset;
        } else if (whence == SEEK_END) {
            new_offset = (int32_t)tasks[current_task].open_files[fd].node->size + offset;
        } else {
            return (uint32_t)-1;
        }

        if (new_offset < 0) {
            return (uint32_t)-1;
        }

        tasks[current_task].open_files[fd].offset = (uint32_t)new_offset;
        return (uint32_t)new_offset;
    }
    else if (syscall_num == 20) {
        /* Syscall 20: int pipe(int pipefd[2]) */
        int* pipefd = (int*)arg1;
        if (!is_valid_user_range((uint32_t)pipefd, 2 * sizeof(int))) {
            return (uint32_t)-1;
        }

        int read_fd = -1;
        int write_fd = -1;
        for (int i = 3; i < MAX_OPEN_FILES; i++) {
            if (!tasks[current_task].open_files[i].is_used) {
                if (read_fd == -1) {
                    read_fd = i;
                } else {
                    write_fd = i;
                    break;
                }
            }
        }
        if (read_fd == -1 || write_fd == -1) {
            return (uint32_t)-1;
        }

        vfs_node_t* r_node = NULL;
        vfs_node_t* w_node = NULL;
        if (vfs_create_pipe(&r_node, &w_node) < 0) {
            return (uint32_t)-1;
        }

        tasks[current_task].open_files[read_fd].is_used = 1;
        tasks[current_task].open_files[read_fd].type = TYPE_PIPE;
        tasks[current_task].open_files[read_fd].flags = O_RDONLY;
        tasks[current_task].open_files[read_fd].offset = 0;
        tasks[current_task].open_files[read_fd].node = r_node;

        tasks[current_task].open_files[write_fd].is_used = 1;
        tasks[current_task].open_files[write_fd].type = TYPE_PIPE;
        tasks[current_task].open_files[write_fd].flags = O_WRONLY;
        tasks[current_task].open_files[write_fd].offset = 0;
        tasks[current_task].open_files[write_fd].node = w_node;

        pipefd[0] = read_fd;
        pipefd[1] = write_fd;
        return 0;
    }
    else if (syscall_num == 21) {
        /* Syscall 21: int dup2(int oldfd, int newfd) */
        int oldfd = (int)arg1;
        int newfd = (int)arg2;

        if (oldfd < 0 || oldfd >= MAX_OPEN_FILES || !tasks[current_task].open_files[oldfd].is_used) {
            return (uint32_t)-1;
        }
        if (newfd < 0 || newfd >= MAX_OPEN_FILES) {
            return (uint32_t)-1;
        }
        if (oldfd == newfd) {
            return (uint32_t)newfd;
        }

        if (tasks[current_task].open_files[newfd].is_used) {
            if (tasks[current_task].open_files[newfd].node != NULL) {
                vfs_close(tasks[current_task].open_files[newfd].node);
            }
        }

        tasks[current_task].open_files[newfd] = tasks[current_task].open_files[oldfd];
        if (tasks[current_task].open_files[newfd].node != NULL &&
            tasks[current_task].open_files[newfd].node != vfs_get_tty_node()) {
            tasks[current_task].open_files[newfd].node->ref_count++;
        }
        return (uint32_t)newfd;
    }
    
    return (uint32_t)-1;
}