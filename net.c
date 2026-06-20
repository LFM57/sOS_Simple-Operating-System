#include "system.h"
#include "net.h"
#include "e1000.h"


/* Set sOS Static IP: 192.168.1.250 */
uint8_t sOS_ip[4] = {192, 168, 56, 250};

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

int net_alloc_socket() {
    for (int i = 0; i < MAX_SOCKETS; i++) {
        if (!sockets[i].is_used) {
            sockets[i].is_used = 1;
            sockets[i].local_port = 0;
            sockets[i].rx_ready = 0;
            return i;
        }
    }
    return -1;
}

void net_bind_socket(int sock_idx, uint16_t port) {
    sockets[sock_idx].local_port = port;
}

void net_send_udp(int sock_idx, uint8_t* dest_ip, uint16_t dest_port, uint8_t* payload, uint16_t len) {
    uint16_t packet_len = 14 + 20 + 8 + len;
    
    eth_hdr_t* eth = (eth_hdr_t*)tx_buffer;
    /* Broadcast MAC to bypass ARP resolution for this simple implementation */
    for(int i=0; i<6; i++) { eth->dst_mac[i] = 0xFF; eth->src_mac[i] = e1000_mac[i]; }
    eth->type = htons(0x0800);

    ipv4_hdr_t* ip = (ipv4_hdr_t*)(tx_buffer + 14);
    ip->version_ihl = 0x45;
    ip->tos = 0;
    ip->total_len = htons(20 + 8 + len);
    ip->id = htons(1);
    ip->frag_offset = 0;
    ip->ttl = 64;
    ip->protocol = 17; /* UDP Protocol ID */
    ip->checksum = 0;
    for(int i=0; i<4; i++) { ip->src_ip[i] = sOS_ip[i]; ip->dst_ip[i] = dest_ip[i]; }
    ip->checksum = calculate_checksum(ip, 20);

    udp_hdr_t* udp = (udp_hdr_t*)(tx_buffer + 14 + 20);
    udp->src_port = htons(sockets[sock_idx].local_port);
    udp->dst_port = htons(dest_port);
    udp->length = htons(8 + len);
    udp->checksum = 0; /* UDP Checksum is optional in IPv4 */

    uint8_t* data = tx_buffer + 14 + 20 + 8;
    for(int i=0; i<len; i++) data[i] = payload[i];

    e1000_transmit(tx_buffer, packet_len);
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

void net_handle_packet(uint8_t* packet, uint16_t length) {
    if (length < 14) return;
    
    /* Read bytes 12 and 13 directly to get the true EtherType */
    uint16_t raw_type = (packet[12] << 8) | packet[13];
    kprintf("[NET] Raw EtherType: 0x%x\n", raw_type);

    eth_hdr_t* eth = (eth_hdr_t*)packet;

    /* Handle ARP (Address Resolution Protocol) */
    if (raw_type == 0x0806) {
        arp_hdr_t* arp = (arp_hdr_t*)(packet + sizeof(eth_hdr_t));
        
        kprintf("[DEBUG] ARP Seen! Opcode: %d | Who is %d.%d.%d.%d? Tell %d.%d.%d.%d\n", 
                ntohs(arp->opcode),
                arp->target_ip[0], arp->target_ip[1], arp->target_ip[2], arp->target_ip[3],
                arp->sender_ip[0], arp->sender_ip[1], arp->sender_ip[2], arp->sender_ip[3]);

        /* If it's an ARP Request (1) asking for our IP */
        if (ntohs(arp->opcode) == 1 && 
            arp->target_ip[0] == sOS_ip[0] && arp->target_ip[1] == sOS_ip[1] &&
            arp->target_ip[2] == sOS_ip[2] && arp->target_ip[3] == sOS_ip[3]) {
            
            kprintf("[NET] Match! Sending ARP Reply...\n");

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
        
        /* Check if the packet is for our IP */
        if (ip->dst_ip[0] == sOS_ip[0] && ip->dst_ip[1] == sOS_ip[1] &&
            ip->dst_ip[2] == sOS_ip[2] && ip->dst_ip[3] == sOS_ip[3]) {
            
            /* If it is an ICMP packet (Protocol 1) */
            if (ip->protocol == 1) {
                int ip_header_len = (ip->version_ihl & 0x0F) * 4;
                icmp_hdr_t* icmp = (icmp_hdr_t*)((uint8_t*)ip + ip_header_len);

                /* If it's an Echo Request (Type 8) */
                if (icmp->type == 8) {
                    kprintf("\n[NET] PING Received from %d.%d.%d.%d! Bouncing back...\n", 
                            ip->src_ip[0], ip->src_ip[1], ip->src_ip[2], ip->src_ip[3]);

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
            /* ... existing ICMP block ... */
            
            /* If it is a UDP packet (Protocol 17) */
            else if (ip->protocol == 17) {
                int ip_header_len = (ip->version_ihl & 0x0F) * 4;
                udp_hdr_t* udp = (udp_hdr_t*)((uint8_t*)ip + ip_header_len);
                
                uint16_t dst_port = ntohs(udp->dst_port);
                uint16_t payload_len = ntohs(udp->length) - sizeof(udp_hdr_t);
                uint8_t* payload = (uint8_t*)udp + sizeof(udp_hdr_t);

                /* Route packet to the matching bound socket */
                for(int i=0; i<MAX_SOCKETS; i++) {
                    if(sockets[i].is_used && sockets[i].local_port == dst_port) {
                        int copy_len = payload_len < SOCK_BUF_SIZE ? payload_len : SOCK_BUF_SIZE;
                        for(int j=0; j<copy_len; j++) sockets[i].rx_buffer[j] = payload[j];
                        sockets[i].rx_len = copy_len;
                        sockets[i].rx_ready = 1; /* Mark as ready for user-space to read */
                        break;
                    }
                }
            }
        }
    }
}