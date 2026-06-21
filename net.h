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
#define SOCK_BUF_SIZE 8192  /* Increased to 8KB for HTTP payloads */

/* TCP States */
#define TCP_CLOSED      0
#define TCP_SYN_SENT    1
#define TCP_ESTABLISHED 2

/* 20-byte TCP Header */
typedef struct {
    uint16_t src_port;
    uint16_t dst_port;
    uint32_t seq;
    uint32_t ack;
    uint16_t flags_offset; /* Data Offset (4 bits), Reserved (6 bits), Flags (6 bits) */
    uint16_t window_size;
    uint16_t checksum;
    uint16_t urgent_p;
} __attribute__((packed)) tcp_hdr_t;

/* TCP Pseudo Header (Required for TCP Checksum) */
typedef struct {
    uint8_t src_ip[4];
    uint8_t dst_ip[4];
    uint8_t zeros;
    uint8_t proto;
    uint16_t tcp_len;
} __attribute__((packed)) tcp_pseudo_hdr_t;

/* Internal Socket Structure */
typedef struct {
    int is_used;
    int type; /* 1 = UDP, 2 = TCP */
    uint16_t local_port;
    
    /* TCP Specifics */
    uint8_t remote_ip[4];
    uint16_t remote_port;
    uint32_t seq_num;
    uint32_t ack_num;
    int tcp_state;

    /* Buffer for received data */
    uint8_t rx_buffer[SOCK_BUF_SIZE];
    uint32_t rx_len; /* Changed to 32-bit to handle larger files */
    int rx_ready;
} socket_t;

int net_alloc_socket(int type);
void net_bind_socket(int sock_idx, uint16_t port);
void net_send_udp(int sock_idx, uint8_t* dest_ip, uint16_t dest_port, uint8_t* payload, uint16_t len);
int net_recv_udp(int sock_idx, uint8_t* buf, uint32_t max_len);

/* Endianness macros for 32-bit integers */
#define htonl(x) (uint32_t)( (((uint32_t)(x) & 0xFF) << 24) | (((uint32_t)(x) & 0xFF00) << 8) | (((uint32_t)(x) & 0xFF0000) >> 8) | (((uint32_t)(x) & 0xFF000000) >> 24) )
#define ntohl(x) htonl(x)

/* DHCP Packet Structure */
typedef struct {
    uint8_t op;         /* Message op code / message type. 1 = BOOTREQUEST, 2 = BOOTREPLY */
    uint8_t htype;      /* Hardware address type */
    uint8_t hlen;       /* Hardware address length */
    uint8_t hops;
    uint32_t xid;       /* Transaction ID */
    uint16_t secs;
    uint16_t flags;
    uint8_t ciaddr[4];  /* Client IP address */
    uint8_t yiaddr[4];  /* 'Your' (client) IP address */
    uint8_t siaddr[4];  /* Server IP address */
    uint8_t giaddr[4];  /* Gateway IP address */
    uint8_t chaddr[16]; /* Client hardware address (MAC) */
    uint8_t sname[64];
    uint8_t file[128];
    uint32_t magic;     /* Magic cookie */
    uint8_t options[64];/* Optional parameters */
} __attribute__((packed)) dhcp_t;

extern uint8_t sOS_router[4];
extern uint8_t sOS_subnet[4];
extern uint8_t sOS_dns[4];

void net_dhcp_discover(void);

#endif