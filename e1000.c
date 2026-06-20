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
    kprintf("\n[INFO] Intel E1000 Initialization complete!\n");
}