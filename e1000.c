#include "system.h"
#include "pci.h"
#include "e1000.h"

uint32_t e1000_mmio_base = 0;
uint8_t  e1000_mac[6];

/* Helper functions to read/write e1000 memory-mapped registers */
void e1000_write_reg(uint16_t offset, uint32_t val) {
    *(volatile uint32_t*)(e1000_mmio_base + offset) = val;
}

uint32_t e1000_read_reg(uint16_t offset) {
    return *(volatile uint32_t*)(e1000_mmio_base + offset);
}

struct e1000_rx_desc *rx_descs;
struct e1000_tx_desc *tx_descs;
uint16_t rx_curr = 0;
uint16_t tx_curr = 0;
uint8_t e1000_irq_line = 0;

/* Helper to unmask a specific IRQ on the master/slave PICs */
void pic_enable_irq(uint8_t irq) {
    uint16_t port = (irq < 8) ? 0x21 : 0xA1;
    uint8_t value = inb(port) & ~(1 << (irq % 8));
    outb(port, value);
}

void e1000_rx_init(void) {
    /* Allocate 1 page (4KB) for the 32 RX Descriptors (requires 512 bytes) */
    rx_descs = (struct e1000_rx_desc *)pmm_alloc_page();
    for (int i = 0; i < NUM_RX_DESC; i++) {
        /* Allocate 1 physical page (4KB) for each packet buffer */
        rx_descs[i].addr = (uint64_t)(uint32_t)pmm_alloc_page();
        rx_descs[i].status = 0;
    }

    e1000_write_reg(0x2800, (uint32_t)rx_descs); /* RDBAL (Low address) */
    e1000_write_reg(0x2804, 0);                  /* RDBAH (High address) */
    e1000_write_reg(0x2808, NUM_RX_DESC * 16);   /* RDLEN (Total Size) */
    e1000_write_reg(0x2810, 0);                  /* RDH (Head) */
    e1000_write_reg(0x2818, NUM_RX_DESC - 1);    /* RDT (Tail) */

    /* RCTL: EN (Enable) | UPE (Unicast Promisc) | MPE (Multicast) | BAM (Broadcast) */
    e1000_write_reg(0x0100, 0x801E);
}

void e1000_tx_init(void) {
    tx_descs = (struct e1000_tx_desc *)pmm_alloc_page();
    for (int i = 0; i < NUM_TX_DESC; i++) {
        tx_descs[i].addr = 0;
        tx_descs[i].cmd = 0;
    }

    e1000_write_reg(0x3800, (uint32_t)tx_descs); /* TDBAL */
    e1000_write_reg(0x3804, 0);                  /* TDBAH */
    e1000_write_reg(0x3808, NUM_TX_DESC * 16);   /* TDLEN */
    e1000_write_reg(0x3810, 0);                  /* TDH */
    e1000_write_reg(0x3818, 0);                  /* TDT */

    /* TCTL: EN (Enable) | PSP (Pad Short Packets) */
    e1000_write_reg(0x0400, 0x0000000A); 
}

/* This function is called by the boot.s irq_nic assembly wrapper */
void e1000_handler(void) {
    /* Read ICR to clear the interrupt on the card */
    e1000_read_reg(0x00C0); 

    /* Loop through descriptors to see if any are marked DONE (0x01) */
    while ((rx_descs[rx_curr].status & 0x01)) { 
        uint16_t len = rx_descs[rx_curr].length;
        
        kprintf("\n[E1000] Packet Received! Size: %d bytes\n", len);
        
        /* Tell the card we have consumed the packet */
        rx_descs[rx_curr].status = 0;
        e1000_write_reg(0x2818, rx_curr); /* Update Tail */
        rx_curr = (rx_curr + 1) % NUM_RX_DESC;
    }
    
    /* Acknowledge the interrupt on the Master/Slave PICs */
    if (e1000_irq_line >= 8) outb(0xA0, 0x20);
    outb(0x20, 0x20);
}

void init_e1000(void) {
    kprintf("[INFO] Searching for Intel E1000 Network Card...\n");
    
    uint8_t target_bus = 0, target_slot = 0;
    int found = 0;

    /* 1. Find the E1000 on the PCI Bus */
    for (uint16_t bus = 0; bus < 256; bus++) {
        for (uint8_t slot = 0; slot < 32; slot++) {
            uint32_t vendor_device = pci_read_dword(bus, slot, 0, 0);
            if ((vendor_device & 0xFFFF) == 0xFFFF) continue;

            uint16_t vendor_id = vendor_device & 0xFFFF;
            uint16_t device_id = vendor_device >> 16;

            /* 0x8086 is Intel, 0x100E is the PRO/1000 MT Desktop */
            if (vendor_id == 0x8086 && device_id == 0x100E) {
                target_bus = bus;
                target_slot = slot;
                found = 1;
                break;
            }
        }
        if (found) break;
    }

    if (!found) {
        kprintf("[ERROR] E1000 card not found!\n");
        return;
    }

    /* 2. Retrieve MMIO Base Address (BAR0) and IRQ Line */
    uint32_t bar0 = pci_read_dword(target_bus, target_slot, 0, 0x10);
    uint32_t phys_base = bar0 & 0xFFFFFFF0; /* Clear bottom bits */
    uint8_t irq = pci_read_dword(target_bus, target_slot, 0, 0x3C) & 0xFF;

    kprintf("[INFO] E1000 found at Bus %d Slot %d. MMIO: 0x%x, IRQ: %d\n", target_bus, target_slot, phys_base, irq);

    /* 3. Identity Map the MMIO space into kernel memory (8 pages / 32 KB is plenty) */
    for (int i = 0; i < 8; i++) {
        map_page(phys_base + (i * 4096), phys_base + (i * 4096));
    }
    e1000_mmio_base = phys_base;

    /* 4. Enable PCI Bus Mastering and Memory Space Enable */
    uint32_t pci_cmd = pci_read_dword(target_bus, target_slot, 0, 0x04);
    pci_cmd |= 0x06; /* Bit 1 = Memory Access, Bit 2 = Bus Master */
    pci_write_dword(target_bus, target_slot, 0, 0x04, pci_cmd);

    /* 5. Read the MAC Address from the Receive Address Registers (RAL/RAH) */
    uint32_t mac_low = e1000_read_reg(0x5400);  /* Receive Address Low */
    uint32_t mac_high = e1000_read_reg(0x5404); /* Receive Address High */

    e1000_mac[0] = mac_low & 0xFF;
    e1000_mac[1] = (mac_low >> 8) & 0xFF;
    e1000_mac[2] = (mac_low >> 16) & 0xFF;
    e1000_mac[3] = (mac_low >> 24) & 0xFF;
    e1000_mac[4] = mac_high & 0xFF;
    e1000_mac[5] = (mac_high >> 8) & 0xFF;

    kprintf("[INFO] MAC Address: ");
    for (int i = 0; i < 6; i++) {
        uint8_t m = e1000_mac[i];
        char hex[] = "0123456789abcdef";
        kputc(hex[m >> 4]); kputc(hex[m & 0x0F]);
        if (i < 5) kputc(':');
    }
    /* Clear Multicast Table Array (fixes bugs in some VMs) */
    for(int i = 0; i < 128; i++) e1000_write_reg(0x5200 + (i * 4), 0);

    /* Force Link Up (SLU bit = 0x40 in CTRL register) */
    uint32_t ctrl = e1000_read_reg(0x0000);
    e1000_write_reg(0x0000, ctrl | 0x40);

    /* Initialize RX/TX rings */
    e1000_rx_init();
    e1000_tx_init();

    /* Hook the Interrupt to the IDT */
    extern void irq_nic();
    e1000_irq_line = irq;
    idt_set_gate(32 + irq, (uint32_t)irq_nic, 0x08, 0x8E);

    /* Unmask the IRQ on the PIC */
    pic_enable_irq(irq);
    if (irq >= 8) {
        pic_enable_irq(2); /* Enable Slave cascade on Master PIC */
    }

    /* Enable specific interrupts on the E1000 (IMS Register) */
    e1000_write_reg(0x00D0, 0x1F6DC); 
    e1000_read_reg(0x00C0); /* Clear any pending interrupts */
    kprintf("\n[INFO] Intel E1000 Initialization complete!\n");
}