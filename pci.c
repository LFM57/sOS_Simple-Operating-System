#include "system.h"
#include "pci.h"

/* Read a 32-bit register from the PCI configuration space */
uint32_t pci_read_dword(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset) {
    uint32_t address;
    
    /* Format the address as required by the PCI specification */
    address = (uint32_t)((bus << 16) | (slot << 11) | (func << 8) | (offset & 0xFC) | ((uint32_t)0x80000000));
    
    /* Write address to the Address Port */
    outl(PCI_CONFIG_ADDRESS, address);
    
    /* Read data from the Data Port */
    return inl(PCI_CONFIG_DATA);
}

/* Scan the PCI bus to find connected devices */
void init_pci(void) {
    kprintf("[INFO] Scanning PCI Bus...\n");
    int devices_found = 0;

    for (uint16_t bus = 0; bus < 256; bus++) {
        for (uint8_t slot = 0; slot < 32; slot++) {
            /* Offset 0 contains Vendor ID (lower 16) and Device ID (upper 16) */
            uint32_t vendor_device = pci_read_dword(bus, slot, 0, 0);
            
            /* If Vendor ID is 0xFFFF, the device doesn't exist */
            if ((vendor_device & 0xFFFF) == 0xFFFF) {
                continue;
            }

            uint16_t vendor_id = vendor_device & 0xFFFF;
            uint16_t device_id = vendor_device >> 16;
            
            /* Offset 8 contains Revision ID and Class Code */
            uint32_t class_info = pci_read_dword(bus, slot, 0, 8);
            uint8_t class_code = class_info >> 24;

            kprintf("  -> Bus %d, Slot %d | Vendor: 0x%x, Dev: 0x%x | Class: %d\n", 
                    bus, slot, vendor_id, device_id, class_code);
            devices_found++;
        }
    }
    kprintf("[INFO] PCI Scan complete. %d devices found.\n", devices_found);
}