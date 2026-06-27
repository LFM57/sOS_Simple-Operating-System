#include "system.h"
#include "net.h"
#include "e1000.h"


/* Reset IPs to 0.0.0.0 (DHCP will configure them) */
uint8_t sOS_ip[4] = {0, 0, 0, 0};
uint8_t sOS_router[4] = {0, 0, 0, 0};
uint8_t sOS_subnet[4] = {0, 0, 0, 0};
uint8_t sOS_dns[4] = {0, 0, 0, 0};

uint32_t dhcp_xid = 0x12345678; /* Unique transaction ID */

/* Calculates Internet Checksums (used for IP and ICMP headers) */
uint16_t calculate_checksum(void *b, int len) {
    uint16_t *buf = b;
    uint32_t sum = 0;
    for (sum = 0; len > 1; len -= 2) sum += *buf++;
    if (len == 1) sum += *(uint8_t*)buf;
    sum = (sum >> 16) + (sum & 0xFFFF);
    sum += (sum >> 16);
    return (uint16_t)(~sum);
}

socket_t sockets[MAX_SOCKETS];
static uint8_t tx_buffer[2048]; /* Static TX buffer for safe DMA transfers */

/* --- ARP CACHE SYSTEM --- */
typedef struct {
    uint8_t ip[4];
    uint8_t mac[6];
    int in_use;
} arp_entry_t;

arp_entry_t arp_cache[32];

void arp_update(uint8_t* ip, uint8_t* mac) {
    /* Ignore 0.0.0.0 */
    if (ip[0] == 0 && ip[1] == 0 && ip[2] == 0 && ip[3] == 0) return;
    
    /* Update existing entry */
    for (int i = 0; i < 32; i++) {
        if (arp_cache[i].in_use && 
            arp_cache[i].ip[0] == ip[0] && arp_cache[i].ip[1] == ip[1] &&
            arp_cache[i].ip[2] == ip[2] && arp_cache[i].ip[3] == ip[3]) {
            for(int j = 0; j < 6; j++) arp_cache[i].mac[j] = mac[j];
            return;
        }
    }
    /* Create new entry */
    for (int i = 0; i < 32; i++) {
        if (!arp_cache[i].in_use) {
            arp_cache[i].in_use = 1;
            for(int j = 0; j < 4; j++) arp_cache[i].ip[j] = ip[j];
            for(int j = 0; j < 6; j++) arp_cache[i].mac[j] = mac[j];
            return;
        }
    }
}

void arp_request(uint8_t* target_ip) {
    uint8_t packet[14 + 28];
    eth_hdr_t* eth = (eth_hdr_t*)packet;
    for(int i = 0; i < 6; i++) { eth->dst_mac[i] = 0xFF; eth->src_mac[i] = e1000_mac[i]; }
    eth->type = htons(0x0806);
    
    arp_hdr_t* arp = (arp_hdr_t*)(packet + 14);
    arp->hw_type = htons(1);
    arp->proto_type = htons(0x0800);
    arp->hw_len = 6;
    arp->proto_len = 4;
    arp->opcode = htons(1); /* Request */
    for(int i = 0; i < 6; i++) { arp->sender_mac[i] = e1000_mac[i]; arp->target_mac[i] = 0; }
    for(int i = 0; i < 4; i++) { arp->sender_ip[i] = sOS_ip[i]; arp->target_ip[i] = target_ip[i]; }
    
    e1000_transmit(packet, sizeof(packet));
}

int arp_resolve(uint8_t* dest_ip, uint8_t* out_mac) {
    /* Hardcode broadcast MAC for DHCP */
    if (dest_ip[0] == 255 && dest_ip[1] == 255 && dest_ip[2] == 255 && dest_ip[3] == 255) {
        for(int i=0; i<6; i++) out_mac[i] = 0xFF;
        return 1;
    }

    /* Target is either the dest_ip (LAN) or the router (Internet) */
    uint8_t target_ip[4];
    int is_local = 1;
    for(int i=0; i<4; i++) {
        if ((dest_ip[i] & sOS_subnet[i]) != (sOS_ip[i] & sOS_subnet[i])) is_local = 0;
    }
    for(int i=0; i<4; i++) target_ip[i] = is_local ? dest_ip[i] : sOS_router[i];

    /* 1. Check cache instantly */
    for(int i=0; i<32; i++) {
        if (arp_cache[i].in_use && 
            arp_cache[i].ip[0] == target_ip[0] && arp_cache[i].ip[1] == target_ip[1] &&
            arp_cache[i].ip[2] == target_ip[2] && arp_cache[i].ip[3] == target_ip[3]) {
            for(int j=0; j<6; j++) out_mac[j] = arp_cache[i].mac[j];
            return 1;
        }
    }

    /* 2. Not in cache: Send ARP Request and wait */
    arp_request(target_ip);
    __asm__ volatile ("sti"); /* Important: Enable interrupts so E1000 can receive the reply! */
    
    uint32_t start = timer_ticks;
    while(timer_ticks < start + 100) { /* Wait up to 1 second */
        for(int i=0; i<32; i++) {
            if (arp_cache[i].in_use && 
                arp_cache[i].ip[0] == target_ip[0] && arp_cache[i].ip[1] == target_ip[1] &&
                arp_cache[i].ip[2] == target_ip[2] && arp_cache[i].ip[3] == target_ip[3]) {
                for(int j=0; j<6; j++) out_mac[j] = arp_cache[i].mac[j];
                return 1;
            }
        }
        __asm__ volatile ("hlt");
    }
    return 0; /* Timeout */
}
/* ------------------------- */

/* --- TCP KERNEL STACK --- */

void net_send_tcp_raw(int sock_idx, uint8_t flags, uint8_t* payload, uint16_t len) {
    uint8_t dest_mac[6];
    if (!arp_resolve(sockets[sock_idx].remote_ip, dest_mac)) {
        kprintf("[TCP] Target Unreachable (ARP Timeout)\n");
        return;
    }

    uint16_t tcp_total_len = 20 + len;
    uint16_t packet_len = 14 + 20 + tcp_total_len;
    static uint8_t tcp_tx_buffer[2048]; /* Isolated buffer for TCP */

    /* 1. Ethernet Header */
    eth_hdr_t* eth = (eth_hdr_t*)tcp_tx_buffer;
    for(int i=0; i<6; i++) { eth->dst_mac[i] = dest_mac[i]; eth->src_mac[i] = e1000_mac[i]; }
    eth->type = htons(0x0800);

    /* 2. IPv4 Header */
    ipv4_hdr_t* ip = (ipv4_hdr_t*)(tcp_tx_buffer + 14);
    ip->version_ihl = 0x45;
    ip->tos = 0;
    ip->total_len = htons(20 + tcp_total_len);
    ip->id = htons(2);
    ip->frag_offset = 0;
    ip->ttl = 64;
    ip->protocol = 6; /* TCP Protocol ID */
    ip->checksum = 0;
    for(int i=0; i<4; i++) { ip->src_ip[i] = sOS_ip[i]; ip->dst_ip[i] = sockets[sock_idx].remote_ip[i]; }
    ip->checksum = calculate_checksum(ip, 20);

    /* 3. TCP Header */
    tcp_hdr_t* tcp = (tcp_hdr_t*)(tcp_tx_buffer + 14 + 20);
    tcp->src_port = htons(sockets[sock_idx].local_port);
    tcp->dst_port = htons(sockets[sock_idx].remote_port);
    tcp->seq = htonl(sockets[sock_idx].seq_num);
    tcp->ack = htonl(sockets[sock_idx].ack_num);
    tcp->flags_offset = htons((5 << 12) | flags); /* 5 words (20 bytes) offset + flags */
    tcp->window_size = htons(8192);
    tcp->checksum = 0;
    tcp->urgent_p = 0;

    /* 4. Copy Payload */
    uint8_t* data = tcp_tx_buffer + 14 + 20 + 20;
    for(int i=0; i<len; i++) data[i] = payload[i];

    /* 5. Calculate TCP Checksum (requires pseudo header) */
    uint8_t chksum_buf[2048];
    tcp_pseudo_hdr_t* p_hdr = (tcp_pseudo_hdr_t*)chksum_buf;
    for(int i=0; i<4; i++) { p_hdr->src_ip[i] = sOS_ip[i]; p_hdr->dst_ip[i] = sockets[sock_idx].remote_ip[i]; }
    p_hdr->zeros = 0; p_hdr->proto = 6; p_hdr->tcp_len = htons(tcp_total_len);
    
    /* Copy TCP header & payload directly after pseudo header for checksum calc */
    for(int i=0; i<tcp_total_len; i++) chksum_buf[sizeof(tcp_pseudo_hdr_t) + i] = ((uint8_t*)tcp)[i];
    tcp->checksum = calculate_checksum(chksum_buf, sizeof(tcp_pseudo_hdr_t) + tcp_total_len);

    e1000_transmit(tcp_tx_buffer, packet_len);
}
/* ------------------------- */

int net_alloc_socket(int type) {
    for (int i = 0; i < MAX_SOCKETS; i++) {
        if (!sockets[i].is_used) {
            sockets[i].is_used = 1;
            sockets[i].local_port = 0;
            sockets[i].rx_ready = 0;
            sockets[i].type = type; /* 1 = UDP, 2 = TCP */
            sockets[i].tcp_state = TCP_CLOSED;
            sockets[i].rx_len = 0;
            return i;
        }
    }
    return -1;
}

void net_bind_socket(int sock_idx, uint16_t port) {
    sockets[sock_idx].local_port = port;
}

/* Raw internal UDP sender (Kernel use) */
void net_send_udp_raw(uint16_t src_port, uint8_t* dest_ip, uint16_t dest_port, uint8_t* payload, uint16_t len) {
    uint8_t dest_mac[6];
    
    /* Fetch MAC using ARP Cache / Router Gateway */
    if (!arp_resolve(dest_ip, dest_mac)) {
        kprintf("\n[NET] Dropped packet: Target Unreachable (ARP Timeout)\n");
        return;
    }

    uint16_t packet_len = 14 + 20 + 8 + len;
    
    eth_hdr_t* eth = (eth_hdr_t*)tx_buffer;
    for(int i=0; i<6; i++) { eth->dst_mac[i] = dest_mac[i]; eth->src_mac[i] = e1000_mac[i]; } 
    eth->type = htons(0x0800);

    ipv4_hdr_t* ip = (ipv4_hdr_t*)(tx_buffer + 14);
    ip->version_ihl = 0x45;
    ip->tos = 0;
    ip->total_len = htons(20 + 8 + len);
    ip->id = htons(1);
    ip->frag_offset = 0;
    ip->ttl = 64;
    ip->protocol = 17;
    ip->checksum = 0;
    for(int i=0; i<4; i++) { ip->src_ip[i] = sOS_ip[i]; ip->dst_ip[i] = dest_ip[i]; }
    ip->checksum = calculate_checksum(ip, 20);

    udp_hdr_t* udp = (udp_hdr_t*)(tx_buffer + 14 + 20);
    udp->src_port = htons(src_port);
    udp->dst_port = htons(dest_port);
    udp->length = htons(8 + len);
    udp->checksum = 0;

    uint8_t* data = tx_buffer + 14 + 20 + 8;
    for(int i=0; i<len; i++) data[i] = payload[i];

    e1000_transmit(tx_buffer, packet_len);
}

/* User-space wrapped UDP sender */
void net_send_udp(int sock_idx, uint8_t* dest_ip, uint16_t dest_port, uint8_t* payload, uint16_t len) {
    net_send_udp_raw(sockets[sock_idx].local_port, dest_ip, dest_port, payload, len);
}

int net_recv_udp(int sock_idx, uint8_t* buf, uint32_t max_len) {
    if (!sockets[sock_idx].rx_ready) return 0; /* Non-blocking read */
    
    int copy_len = sockets[sock_idx].rx_len < max_len ? sockets[sock_idx].rx_len : max_len;
    for(int i=0; i<copy_len; i++) {
        buf[i] = sockets[sock_idx].rx_buffer[i];
    }
    sockets[sock_idx].rx_ready = 0; /* Consume the packet */
    return copy_len;
}

void net_dhcp_discover() {
    uint8_t payload[sizeof(dhcp_t)];
    for(int i=0; i<sizeof(dhcp_t); i++) payload[i] = 0;
    
    dhcp_t* dhcp = (dhcp_t*)payload;
    dhcp->op = 1;        /* Boot Request */
    dhcp->htype = 1;     /* Ethernet */
    dhcp->hlen = 6;      /* MAC length */
    dhcp->xid = dhcp_xid;
    dhcp->flags = htons(0x8000); /* Tell server to broadcast the reply back to us */
    for(int i=0; i<6; i++) dhcp->chaddr[i] = e1000_mac[i];
    dhcp->magic = htonl(0x63825363); /* Standard DHCP Magic Cookie */
    
    /* Options */
    dhcp->options[0] = 53; /* Message Type */
    dhcp->options[1] = 1;  /* Length */
    dhcp->options[2] = 1;  /* 1 = DHCP Discover */
    
    dhcp->options[3] = 55; /* Parameter Request List */
    dhcp->options[4] = 3;  /* Length */
    dhcp->options[5] = 1;  /* Subnet Mask */
    dhcp->options[6] = 3;  /* Router (Gateway) */
    dhcp->options[7] = 6;  /* DNS Server */
    
    dhcp->options[8] = 255; /* End of options */
    
    uint8_t broadcast_ip[4] = {255, 255, 255, 255};
    kprintf("[DHCP] Broadcasting Discover...\n");
    net_send_udp_raw(68, broadcast_ip, 67, payload, sizeof(dhcp_t));
}

void net_handle_packet(uint8_t* packet, uint16_t length) {
    if (length < 14) return;
    
    /* Read bytes 12 and 13 directly to get the true EtherType */
    uint16_t raw_type = (packet[12] << 8) | packet[13];

    eth_hdr_t* eth = (eth_hdr_t*)packet;

    /* Handle ARP (Address Resolution Protocol) */
    if (raw_type == 0x0806) {
        arp_hdr_t* arp = (arp_hdr_t*)(packet + sizeof(eth_hdr_t));

        /* UPDATE CACHE WITH SENDER'S INFO */
        arp_update(arp->sender_ip, arp->sender_mac);

        /* If it's an ARP Request (1) asking for our IP */
        if (ntohs(arp->opcode) == 1 && 
            arp->target_ip[0] == sOS_ip[0] && arp->target_ip[1] == sOS_ip[1] &&
            arp->target_ip[2] == sOS_ip[2] && arp->target_ip[3] == sOS_ip[3]) {

            arp->opcode = htons(2); /* Change to Reply */

            /* Swap Targets and Senders */
            for(int i=0; i<6; i++) arp->target_mac[i] = arp->sender_mac[i];
            for(int i=0; i<4; i++) arp->target_ip[i] = arp->sender_ip[i];

            for(int i=0; i<6; i++) arp->sender_mac[i] = e1000_mac[i];
            for(int i=0; i<4; i++) arp->sender_ip[i] = sOS_ip[i];

            /* Swap Ethernet MACs */
            for(int i=0; i<6; i++) {
                eth->dst_mac[i] = eth->src_mac[i];
                eth->src_mac[i] = e1000_mac[i];
            }

            e1000_transmit(packet, length);
        }
    }
    
    /* Handle IPv4 */
    else if (raw_type == 0x0800) {
        ipv4_hdr_t* ip = (ipv4_hdr_t*)(packet + sizeof(eth_hdr_t));
        
        /* Allow Broadcast Packets (Required for DHCP) */
        int is_broadcast = (ip->dst_ip[0] == 255 && ip->dst_ip[1] == 255 && ip->dst_ip[2] == 255 && ip->dst_ip[3] == 255);
        int is_for_us = (ip->dst_ip[0] == sOS_ip[0] && ip->dst_ip[1] == sOS_ip[1] &&
                         ip->dst_ip[2] == sOS_ip[2] && ip->dst_ip[3] == sOS_ip[3]);
        
        if (is_for_us || is_broadcast) {
            
            /* If it is an ICMP packet (Protocol 1) */
            if (ip->protocol == 1) {
                int ip_header_len = (ip->version_ihl & 0x0F) * 4;
                icmp_hdr_t* icmp = (icmp_hdr_t*)((uint8_t*)ip + ip_header_len);

                /* If it's an Echo Request (Type 8) */
                if (icmp->type == 8) {
                    /* 1. Modify ICMP to Echo Reply (Type 0) */
                    icmp->type = 0;
                    icmp->checksum = 0;
                    
                    int icmp_data_len = ntohs(ip->total_len) - ip_header_len;
                    icmp->checksum = calculate_checksum(icmp, icmp_data_len);

                    /* 2. Swap IP Source & Destination */
                    for(int i=0; i<4; i++) {
                        uint8_t temp = ip->src_ip[i];
                        ip->src_ip[i] = sOS_ip[i];
                        ip->dst_ip[i] = temp;
                    }
                    ip->checksum = 0;
                    ip->checksum = calculate_checksum(ip, ip_header_len);

                    /* 3. Swap Ethernet MACs */
                    for(int i=0; i<6; i++) {
                        eth->dst_mac[i] = eth->src_mac[i];
                        eth->src_mac[i] = e1000_mac[i];
                    }

                    e1000_transmit(packet, length);
                }
            }
            
            /* If it is a UDP packet (Protocol 17) */
            else if (ip->protocol == 17) {
                int ip_header_len = (ip->version_ihl & 0x0F) * 4;
                udp_hdr_t* udp = (udp_hdr_t*)((uint8_t*)ip + ip_header_len);

                /* [FIX ADDITION] Prevent UDP length underflow */
                if (ntohs(udp->length) < sizeof(udp_hdr_t)) return;
                
                uint16_t dst_port = ntohs(udp->dst_port);
                uint16_t payload_len = ntohs(udp->length) - sizeof(udp_hdr_t);
                uint8_t* payload = (uint8_t*)udp + sizeof(udp_hdr_t);

                /* --- KERNEL DHCP CLIENT --- */
                if (dst_port == 68) {
                    dhcp_t* dhcp = (dhcp_t*)payload;
                    if (dhcp->op == 2 && dhcp->xid == dhcp_xid) { /* Boot Reply targeting us */
                        uint8_t msg_type = 0;
                        uint8_t server_ip[4] = {0};
                        
                        /* Parse DHCP Options */
                        int opt_idx = 0;
                        while (opt_idx < 64 && dhcp->options[opt_idx] != 255) {
                            uint8_t opt = dhcp->options[opt_idx];
                            if (opt == 0) { opt_idx++; continue; } /* Padding */
                            uint8_t len = dhcp->options[opt_idx + 1];
                            
                            if (opt == 53) msg_type = dhcp->options[opt_idx + 2];
                            if (opt == 54) for(int k=0; k<4; k++) server_ip[k] = dhcp->options[opt_idx + 2 + k];
                            
                            if (opt == 1) for(int k=0; k<4; k++) sOS_subnet[k] = dhcp->options[opt_idx + 2 + k];
                            if (opt == 3) for(int k=0; k<4; k++) sOS_router[k] = dhcp->options[opt_idx + 2 + k];
                            if (opt == 6) for(int k=0; k<4; k++) sOS_dns[k] = dhcp->options[opt_idx + 2 + k];
                            
                            opt_idx += 2 + len;
                        }
                        
                        /* State 1: We received an IP Offer */
                        if (msg_type == 2) { 
                            /* Removed the "Offered IP" print here to stay silent until confirmed */
                            uint8_t req_payload[sizeof(dhcp_t)];
                            for(int i=0; i<sizeof(dhcp_t); i++) req_payload[i] = 0;
                            dhcp_t* req = (dhcp_t*)req_payload;
                            req->op = 1; req->htype = 1; req->hlen = 6; req->xid = dhcp_xid;
                            req->flags = htons(0x8000);
                            for(int i=0; i<6; i++) req->chaddr[i] = e1000_mac[i];
                            req->magic = htonl(0x63825363);
                            
                            req->options[0] = 53; req->options[1] = 1; req->options[2] = 3; /* Request */
                            req->options[3] = 50; req->options[4] = 4; /* Requested IP */
                            for(int i=0; i<4; i++) req->options[5+i] = dhcp->yiaddr[i];
                            req->options[9] = 54; req->options[10] = 4; /* Server ID */
                            for(int i=0; i<4; i++) req->options[11+i] = server_ip[i];
                            req->options[15] = 255;
                            
                            uint8_t broadcast_ip[4] = {255, 255, 255, 255};
                            net_send_udp_raw(68, broadcast_ip, 67, req_payload, sizeof(dhcp_t));
                        }
                        /* State 2: The server Acknowledged our request */
                        else if (msg_type == 5) { 
                            for(int i=0; i<4; i++) sOS_ip[i] = dhcp->yiaddr[i];
                            kprintf("[DHCP] Bound to %d.%d.%d.%d (Gateway: %d.%d.%d.%d)\n", 
                                    sOS_ip[0], sOS_ip[1], sOS_ip[2], sOS_ip[3],
                                    sOS_router[0], sOS_router[1], sOS_router[2], sOS_router[3]);
                        }
                    }
                } 
                /* --- USER-SPACE SOCKET ROUTING --- */
                else {
                    /* Route packet to the matching bound socket */
                    for(int i=0; i<MAX_SOCKETS; i++) {
                        if(sockets[i].is_used && sockets[i].local_port == dst_port) {
                            int copy_len = payload_len < SOCK_BUF_SIZE ? payload_len : SOCK_BUF_SIZE;
                            for(int j=0; j<copy_len; j++) sockets[i].rx_buffer[j] = payload[j];
                            sockets[i].rx_len = copy_len;
                            sockets[i].rx_ready = 1; 
                            break;
                        }
                    }
                }
            }
            /* --- MINIMAL TCP STATE MACHINE (Protocol 6) --- */
            else if (ip->protocol == 6) {
                int ip_header_len = (ip->version_ihl & 0x0F) * 4;
                tcp_hdr_t* tcp = (tcp_hdr_t*)((uint8_t*)ip + ip_header_len);
                
                uint16_t dst_port = ntohs(tcp->dst_port);
                uint16_t tcp_hdr_len = ((ntohs(tcp->flags_offset) >> 12) & 0x0F) * 4;
                uint8_t flags = ntohs(tcp->flags_offset) & 0x3F;

                /* [FIX ADDITION] Prevent TCP length underflow */
                uint16_t total_ip_len = ntohs(ip->total_len);
                if (total_ip_len < ip_header_len + tcp_hdr_len) return;
                
                uint16_t payload_len = ntohs(ip->total_len) - ip_header_len - tcp_hdr_len;
                uint8_t* payload = (uint8_t*)tcp + tcp_hdr_len;

                /* Find matching socket */
                for(int i=0; i<MAX_SOCKETS; i++) {
                    if(sockets[i].is_used && sockets[i].type == 2 && sockets[i].local_port == dst_port) {
                        
                        /* State 1: Awaiting SYN-ACK */
                        if (sockets[i].tcp_state == TCP_SYN_SENT && (flags & 0x12) == 0x12) {
                            sockets[i].ack_num = ntohl(tcp->seq) + 1;
                            sockets[i].seq_num++; /* SYN consumes 1 seq */
                            sockets[i].tcp_state = TCP_ESTABLISHED;
                            net_send_tcp_raw(i, 0x10, NULL, 0); /* Send ACK */
                        }
                        
                        /* State 2: Established & Receiving Data (PSH or just data) */
                        else if (sockets[i].tcp_state == TCP_ESTABLISHED && payload_len > 0) {
                            /* Append data to socket buffer */
                            for(int j=0; j<payload_len; j++) {
                                if (sockets[i].rx_len < SOCK_BUF_SIZE) {
                                    sockets[i].rx_buffer[sockets[i].rx_len++] = payload[j];
                                }
                            }
                            sockets[i].rx_ready = 1;
                            sockets[i].ack_num += payload_len;
                            net_send_tcp_raw(i, 0x10, NULL, 0); /* Send ACK for data */
                        }
                        
                        /* State 3: Disconnect Request (FIN) */
                        else if (sockets[i].tcp_state == TCP_ESTABLISHED && (flags & 0x01)) {
                            sockets[i].ack_num++;
                            sockets[i].tcp_state = TCP_CLOSED;
                            net_send_tcp_raw(i, 0x11, NULL, 0); /* Send FIN+ACK */
                        }
                        break;
                    }
                }
            }
        }
    }
}

int net_gethostbyname(const char* hostname, uint8_t* out_ip) {
    uint8_t dns_ip[4];
    for(int i=0; i<4; i++) dns_ip[i] = sOS_dns[i];
    if (dns_ip[0] == 0 && dns_ip[1] == 0) {
        /* Fallback to Google DNS */
        dns_ip[0] = 8; dns_ip[1] = 8; dns_ip[2] = 8; dns_ip[3] = 8;
    }

    int sock = net_alloc_socket(1);
    sockets[sock].local_port = 45678;

    uint8_t buf[512];
    for (int i = 0; i < 512; i++) buf[i] = 0;
    buf[0] = 0x12; buf[1] = 0x34; buf[2] = 0x01; buf[3] = 0x00;
    buf[4] = 0x00; buf[5] = 0x01;
    
    int lock = 0;
    const char* part = hostname;
    while (*part) {
        int len = 0;
        while (part[len] && part[len] != '.') len++;
        buf[12 + lock] = len;
        for (int j = 0; j < len; j++) buf[12 + lock + 1 + j] = part[j];
        lock += len + 1;
        if (part[len] == '.') part += len + 1;
        else part += len;
    }
    buf[12 + lock] = 0;
    int qname_len = lock + 1;
    buf[12 + qname_len] = 0x00; buf[12 + qname_len + 1] = 0x01;
    buf[12 + qname_len + 2] = 0x00; buf[12 + qname_len + 3] = 0x01;
    
    net_send_udp_raw(45678, dns_ip, 53, buf, 12 + qname_len + 4);
    
    __asm__ volatile ("sti");
    uint32_t start = timer_ticks;
    while (timer_ticks < start + 500) {
        if (sockets[sock].rx_ready) {
            uint8_t* rx_buf = sockets[sock].rx_buffer;
            int rx_len = sockets[sock].rx_len;
            
            if (rx_len >= 12 && rx_buf[0] == 0x12 && rx_buf[1] == 0x34) {
                int qdcount = (rx_buf[4] << 8) | rx_buf[5];
                int ans_count = (rx_buf[6] << 8) | rx_buf[7];
                if (ans_count > 0) {
                    int offset = 12;
                    for (int q = 0; q < qdcount; q++) {
                        while (rx_buf[offset] != 0) offset++;
                        offset += 5; 
                    }
                    for (int a = 0; a < ans_count; a++) {
                        if (offset >= rx_len) break;
                        if ((rx_buf[offset] & 0xC0) == 0xC0) offset += 2;
                        else { while (rx_buf[offset] != 0) offset++; offset++; }
                        int type = (rx_buf[offset] << 8) | rx_buf[offset+1];
                        offset += 8; 
                        int data_len = (rx_buf[offset] << 8) | rx_buf[offset+1];
                        offset += 2;
                        if (type == 1 && data_len == 4) { 
                            for(int k=0; k<4; k++) out_ip[k] = rx_buf[offset+k];
                            sockets[sock].is_used = 0;
                            return 1;
                        }
                        offset += data_len;
                    }
                }
            }
            sockets[sock].rx_ready = 0;
        }
        __asm__ volatile("hlt");
    }
    sockets[sock].is_used = 0; /* Clean up socket */
    return 0;
}