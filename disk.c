#include "system.h"

/* 
 * Function to read ONE sector (512 bytes) from the master hard drive (Drive 0).
 * lba: The sector number (Logical Block Address)
 * buffer: The buffer where we will store the 512 read bytes
*/
void ata_read_sector(uint32_t lba, uint8_t *buffer) {
    /* 1. Tell the controller we want to use the Master drive (0xE0) and send the 4 most significant bits of the LBA */
    outb(0x1F6, 0xE0 | ((lba >> 24) & 0x0F));
    
    /* 2. Specify that we want to read exactly 1 sector */
    outb(0x1F2, 1);
    
    /* 3. Send the rest of the LBA address (remaining 24 bits) to 3 different ports */
    outb(0x1F3, (uint8_t) lba);
    outb(0x1F4, (uint8_t)(lba >> 8));
    outb(0x1F5, (uint8_t)(lba >> 16));
    
    /* 4. Send the "READ" command (0x20) */
    outb(0x1F7, 0x20);
    
    /* 5. Wait for the drive to be ready to send data (Polling) */
    /* Read the status port (0x1F7). 
       As long as the BSY bit (Busy - 0x80) is 1, we wait.
       We also wait for the DRQ bit (Data Request - 0x08) to become 1.
    */
    uint8_t status;
    while (((status = inb(0x1F7)) & 0x80) || !(status & 0x08)) {
        /* Do nothing, loop until the drive is ready */
    }
    
    /* 6. The drive is ready. We will read 256 16-bit words (which equals 512 bytes) */
    uint16_t *ptr = (uint16_t *)buffer;
    for (int i = 0; i < 256; i++) {
        ptr[i] = inw(0x1F0);
    }
}

/* Write ONE sector (512 bytes) to the hard drive */
void ata_write_sector(uint32_t lba, uint8_t *buffer) {
    outb(0x1F6, 0xE0 | ((lba >> 24) & 0x0F));
    outb(0x1F2, 1);
    outb(0x1F3, (uint8_t) lba);
    outb(0x1F4, (uint8_t)(lba >> 8));
    outb(0x1F5, (uint8_t)(lba >> 16));
    
    outb(0x1F7, 0x30); /* WRITE SECTORS command */
    
    uint8_t status;
    while (((status = inb(0x1F7)) & 0x80) || !(status & 0x08)) {
        /* Wait for the drive to be ready to receive */
    }
    
    uint16_t *ptr = (uint16_t *)buffer;
    for (int i = 0; i < 256; i++) {
        outw(0x1F0, ptr[i]); /* Send the data (16-bit words) */
    }
    
    /* Flush the disk cache to force an immediate write */
    outb(0x1F7, 0xEA); 
    while (inb(0x1F7) & 0x80);
}