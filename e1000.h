#ifndef E1000_H
#define E1000_H

#include <stdint.h>

#define NUM_RX_DESC 32
#define NUM_TX_DESC 8

/* E1000 Receive Descriptor */
struct e1000_rx_desc {
    uint64_t addr;      /* Address of the descriptor's data buffer */
    uint16_t length;    /* Length of data DMAed into data buffer */
    uint16_t checksum;  /* Packet checksum */
    uint8_t status;     /* Descriptor status */
    uint8_t errors;     /* Descriptor Errors */
    uint16_t special;
} __attribute__((packed));

/* E1000 Transmit Descriptor */
struct e1000_tx_desc {
    uint64_t addr;
    uint16_t length;
    uint8_t cso;
    uint8_t cmd;
    uint8_t status;
    uint8_t css;
    uint16_t special;
} __attribute__((packed));

void init_e1000(void);
void e1000_transmit(uint8_t* payload, uint16_t length);

#endif