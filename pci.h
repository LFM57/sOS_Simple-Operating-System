#ifndef PCI_H
#define PCI_H

#include <stdint.h>

/* Ports used by the PCI bus */
#define PCI_CONFIG_ADDRESS 0xCF8
#define PCI_CONFIG_DATA    0xCFC

/* Function prototypes */
uint32_t pci_read_dword(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset);
void init_pci(void);

#endif