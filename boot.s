.section .text
.global _start

/* --- Multiboot Header --- */
.balign 4
.long 0x1BADB002    /* MAGIC */
.long 0x00000003    /* FLAGS (ALIGN | MEMINFO) */
.long 0xE4524FFB    /* CHECKSUM */
/* ----------------------------------------------------------- */

.type _start, @function
_start:
    mov $stack_top, %esp
    
    /* MULTIBOOT : Bootloader put the infos into EAX and EBX */
    /* We push onto the stack to pass them as parameters to C */
    push %ebx  /* Parameter 2: Multiboot structure address */
    push %eax  /* Parameter 1: Magic Number (0x2BADB002) */
    
    call kernel_main

    cli
1:  hlt
    jmp 1b

.section .bss
.balign 16
stack_bottom:
.skip 16384 # 16 KiB
stack_top:

.section .text

.global switch_to_user_mode
.type switch_to_user_mode, @function
switch_to_user_mode:
    cli
    mov 4(%esp), %ebx   /* Argument 1: Address of the user function */
    mov 8(%esp), %ecx   /* Argument 2: Address of the user stack */

    /* 1. Load Ring 3 data segments (GDT Entry 4 = 0x20 | RPL 3 = 0x23) */
    mov $0x23, %ax
    mov %ax, %ds
    mov %ax, %es
    mov %ax, %fs
    mov %ax, %gs

    /* 2. Prepare the stack for the IRET instruction */
    /* IRET pops in the following order: SS, ESP, EFLAGS, CS, EIP */
    
    push $0x23          /* SS (Ring 3 stack segment) */
    push %ecx           /* ESP (Ring 3 stack pointer) */
    
    pushf               /* Retrieve current EFLAGS... */
    pop %eax
    or $0x200, %eax     /* ...Force interrupt enablement (IF bit) */
    push %eax           /* And push the new EFLAGS */
    
    push $0x1B          /* CS (Ring 3 code segment: Entry 3 = 0x18 | RPL 3 = 0x1B) */
    push %ebx           /* EIP (Address of the user function!) */

    iret                /* The jump is made, CPU control is transferred. */

.global gdt_flush
.type gdt_flush, @function
gdt_flush:
    mov 4(%esp), %eax
    lgdt (%eax)
    mov $0x10, %ax
    mov %ax, %ds
    mov %ax, %es
    mov %ax, %fs
    mov %ax, %gs
    mov %ax, %ss
    ljmp $0x08, $.flush
.flush:
    ret

.global idt_flush
.type idt_flush, @function
idt_flush:
    mov 4(%esp), %eax
    lidt (%eax)
    ret

/* IRQ0 - Timer with Context Switching! */
.global irq0
.type irq0, @function
irq0:
    pusha               /* Save EAX -> EDI */
    push %ds            /* Save segment registers */
    push %es
    push %fs
    push %gs

    mov $0x10, %ax
    mov %ax, %ds
    mov %ax, %es
    mov %ax, %fs
    mov %ax, %gs

    push %esp           /* Argument for C */
    call timer_handler  
    
    mov %eax, %esp      /* Restore stack pointer (no need for add $4, %esp) */

    pop %gs             /* Restore in the exact reverse order */
    pop %fs
    pop %es
    pop %ds
    popa
    iret

/* Old IRQ1 (PS/2 Keyboard) */
.global irq1
.type irq1, @function
irq1:
    pusha
    call keyboard_handler
    popa
    iret

/* IRQ for the Network Interface Card (E1000) */
.global irq_nic
.type irq_nic, @function
irq_nic:
    pusha
    call e1000_handler
    popa
    iret

/* IRQ4 - COM1 Serial Port (For Shell via SSH) */
/* WARNING: SHELL VIA SSH IS DEPRECATED AND WILL BE REMOVED IN FURTHER VERSIONS */
.global irq4
.type irq4, @function
irq4:
    pusha
    call serial_handler
    popa
    iret

/* Interrupt 0x80 (128) - Syscalls */
.global isr128
.type isr128, @function
isr128:
    cli
    /* Save all registers EXCEPT EAX (which will contain the return value of the C function) */
    push %ebx
    push %ecx
    push %edx
    push %esi
    push %edi
    push %ebp
    push %ds
    push %es
    
    /* Push arguments for C */
    push %edx  /* arg3 */
    push %ecx  /* arg2 */
    push %ebx  /* arg1 */
    push %eax  /* syscall_num */
    
    call syscall_handler
    add $16, %esp  /* Cleaning the 4 arguments */
    
    /* Restore registers; EAX is untouched and retains its return value */
    pop %es
    pop %ds
    pop %ebp
    pop %edi
    pop %esi
    pop %edx
    pop %ecx
    pop %ebx
    iret

/* ========================================================= */
/* MACROS TO AUTOMATICALLY GENERATE THE 32 EXCEPTIONS        */
/* ========================================================= */

/* Macro for exceptions WITHOUT an error code */
.macro ISR_NOERRCODE num
.global isr\num
isr\num:
    cli
    push $0          /* Dummy error code to align the stack */
    push $\num       /* Exception number */
    jmp isr_common
.endm

/* Macro for exceptions WITH an error code (already pushed by the CPU) */
.macro ISR_ERRCODE num
.global isr\num
isr\num:
    cli
    push $\num       /* Exception number */
    jmp isr_common
.endm

/* Generation of the 32 ISR functions */
ISR_NOERRCODE 0
ISR_NOERRCODE 1
ISR_NOERRCODE 2
ISR_NOERRCODE 3
ISR_NOERRCODE 4
ISR_NOERRCODE 5
ISR_NOERRCODE 6
ISR_NOERRCODE 7
ISR_ERRCODE   8
ISR_NOERRCODE 9
ISR_ERRCODE   10
ISR_ERRCODE   11
ISR_ERRCODE   12
ISR_ERRCODE   13
ISR_ERRCODE   14
ISR_NOERRCODE 15
ISR_NOERRCODE 16
ISR_ERRCODE   17
ISR_NOERRCODE 18
ISR_NOERRCODE 19
ISR_NOERRCODE 20
ISR_NOERRCODE 21
ISR_NOERRCODE 22
ISR_NOERRCODE 23
ISR_NOERRCODE 24
ISR_NOERRCODE 25
ISR_NOERRCODE 26
ISR_NOERRCODE 27
ISR_NOERRCODE 28
ISR_NOERRCODE 29
ISR_NOERRCODE 30
ISR_NOERRCODE 31

/* Table containing the addresses of the 32 functions (so C can loop through them) */
.global isr_stub_table
isr_stub_table:
    .long isr0, isr1, isr2, isr3, isr4, isr5, isr6, isr7
    .long isr8, isr9, isr10, isr11, isr12, isr13, isr14, isr15
    .long isr16, isr17, isr18, isr19, isr20, isr21, isr22, isr23
    .long isr24, isr25, isr26, isr27, isr28, isr29, isr30, isr31

/* Common entry point for all exceptions */
.type isr_common, @function
isr_common:
    pusha            /* Save all general-purpose registers */
    push %ds         /* Save data segments */
    push %es
    push %fs
    push %gs

    /* Load kernel data segment (0x10) to be safe */
    mov $0x10, %ax
    mov %ax, %ds
    mov %ax, %es
    mov %ax, %fs
    mov %ax, %gs

    push %esp        /* Pass the stack ADDRESS as an argument to our C function */
    call fault_handler
    add $4, %esp     /* Clean up the argument */

    pop %gs          /* Restore initial state */
    pop %fs
    pop %es
    pop %ds
    popa
    add $8, %esp     /* Discard exception number and error code */
    iret             /* Return */
