#ifndef NET_H
#define NET_H

#include <stdint.h>

#define htons(x) (uint16_t)( (((uint16_t)(x)) << 8) | (((uint16_t)(x)) >> 8) )
#define ntohs(x) htons(x)

/* 14-byte Ethernet Header */
typedef struct {
    uint8_t dst_mac[6];
    uint8_t src_mac[6];
    uint16_t type;
} __attribute__((packed)) eth_hdr_t;

/* 28-byte ARP Header */
typedef struct {
    uint16_t hw_type;
    uint16_t proto_type;
    uint8_t hw_len;
    uint8_t proto_len;
    uint16_t opcode;
    uint8_t sender_mac[6];
    uint8_t sender_ip[4];
    uint8_t target_mac[6];
    uint8_t target_ip[4];
} __attribute__((packed)) arp_hdr_t;

/* 20-byte IPv4 Header */
typedef struct {
    uint8_t version_ihl;
    uint8_t tos;
    uint16_t total_len;
    uint16_t id;
    uint16_t frag_offset;
    uint8_t ttl;
    uint8_t protocol;
    uint16_t checksum;
    uint8_t src_ip[4];
    uint8_t dst_ip[4];
} __attribute__((packed)) ipv4_hdr_t;

/* 8-byte ICMP Header */
typedef struct {
    uint8_t type;
    uint8_t code;
    uint16_t checksum;
    uint16_t id;
    uint16_t sequence;
} __attribute__((packed)) icmp_hdr_t;

extern uint8_t sOS_ip[4];
extern uint8_t e1000_mac[6];

void net_handle_packet(uint8_t* packet, uint16_t length);

/* 8-byte UDP Header */
typedef struct {
    uint16_t src_port;
    uint16_t dst_port;
    uint16_t length;
    uint16_t checksum;
} __attribute__((packed)) udp_hdr_t;

#define MAX_SOCKETS 16
#define SOCK_BUF_SIZE 1024

/* Internal Socket Structure */
typedef struct {
    int is_used;
    uint16_t local_port;
    
    /* Buffer for the latest received payload */
    uint8_t rx_buffer[SOCK_BUF_SIZE];
    uint16_t rx_len;
    int rx_ready;
} socket_t;

int net_alloc_socket(void);
void net_bind_socket(int sock_idx, uint16_t port);
void net_send_udp(int sock_idx, uint8_t* dest_ip, uint16_t dest_port, uint8_t* payload, uint16_t len);
int net_recv_udp(int sock_idx, uint8_t* buf, uint32_t max_len);

#endif