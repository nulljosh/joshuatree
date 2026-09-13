/* Ethernet + ARP + IPv4 + UDP, just enough of each to get one datagram onto
   the wire and resolve a MAC first. All three headers are defined here
   together rather than split into eth.c/arp.c/ip.c/udp.c since the roadmap
   scopes them as one "minimal stack" item and they're small enough that
   splitting them now would just be more files to keep in sync for no
   present benefit; split later if any one of them grows real complexity. */
#include "net.h"
#include "rtl8139.h"

typedef unsigned int   u32;
typedef unsigned short u16;
typedef unsigned char  u8;

static u16 htons(u16 v) { return (u16)((v << 8) | (v >> 8)); }
static u32 htonl(u32 v) { return (v << 24) | ((v & 0xFF00) << 8) | ((v & 0xFF0000) >> 8) | (v >> 24); }

static u8 our_mac[6];
static u32 our_ip = 0;

struct eth_header {
    u8  dest[6];
    u8  src[6];
    u16 ethertype;
} __attribute__((packed));

#define ETHERTYPE_ARP 0x0806
#define ETHERTYPE_IP  0x0800

struct arp_packet {
    u16 htype, ptype;
    u8  hlen, plen;
    u16 oper;
    u8  sender_mac[6]; u32 sender_ip;
    u8  target_mac[6]; u32 target_ip;
} __attribute__((packed));

struct ip_header {
    u8  version_ihl;
    u8  tos;
    u16 total_length;
    u16 id;
    u16 flags_fragment;
    u8  ttl;
    u8  protocol;
    u16 checksum;
    u32 src_ip, dst_ip;
} __attribute__((packed));

struct udp_header {
    u16 src_port, dst_port, length, checksum;
} __attribute__((packed));

static u16 checksum16(const void *data, u32 len) {
    u32 sum = 0;
    const u16 *p = data;
    while (len > 1) { sum += *p++; len -= 2; }
    if (len) sum += *(const u8 *)p;
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return (u16)~sum;
}

void net_init(u32 ip) {
    rtl8139_get_mac(our_mac);
    our_ip = ip;
}

int arp_resolve(u32 ip, u8 mac_out[6]) {
    u8 frame[sizeof(struct eth_header) + sizeof(struct arp_packet)];
    struct eth_header *eth = (struct eth_header *)frame;
    struct arp_packet *arp = (struct arp_packet *)(frame + sizeof(*eth));

    for (int i = 0; i < 6; i++) { eth->dest[i] = 0xFF; eth->src[i] = our_mac[i]; }
    eth->ethertype = htons(ETHERTYPE_ARP);

    arp->htype = htons(1);          /* Ethernet */
    arp->ptype = htons(0x0800);     /* IPv4 */
    arp->hlen = 6; arp->plen = 4;
    arp->oper = htons(1);           /* request */
    for (int i = 0; i < 6; i++) { arp->sender_mac[i] = our_mac[i]; arp->target_mac[i] = 0; }
    arp->sender_ip = htonl(our_ip);
    arp->target_ip = htonl(ip);

    if (!rtl8139_send(frame, sizeof(frame))) return 0;

    u8 rx[1514];
    for (int attempts = 0; attempts < 2000000; attempts++) {
        u32 n = rtl8139_receive(rx, sizeof(rx));
        if (n < sizeof(struct eth_header) + sizeof(struct arp_packet)) continue;

        struct eth_header *reth = (struct eth_header *)rx;
        if (reth->ethertype != htons(ETHERTYPE_ARP)) continue;
        struct arp_packet *rarp = (struct arp_packet *)(rx + sizeof(*reth));
        if (rarp->oper != htons(2)) continue;             /* not a reply */
        if (rarp->sender_ip != htonl(ip)) continue;        /* not from who we asked */

        for (int i = 0; i < 6; i++) mac_out[i] = rarp->sender_mac[i];
        return 1;
    }
    return 0;
}

int udp_send(u32 dest_ip, u16 dest_port, u16 src_port, const void *data, u32 len) {
    u8 dest_mac[6];
    if (!arp_resolve(dest_ip, dest_mac)) return 0;

    u8 frame[sizeof(struct eth_header) + sizeof(struct ip_header) + sizeof(struct udp_header) + 512];
    if (len > 512) return 0;

    struct eth_header *eth = (struct eth_header *)frame;
    struct ip_header  *ip  = (struct ip_header *)(frame + sizeof(*eth));
    struct udp_header *udp = (struct udp_header *)(frame + sizeof(*eth) + sizeof(*ip));
    u8 *payload = frame + sizeof(*eth) + sizeof(*ip) + sizeof(*udp);

    for (int i = 0; i < 6; i++) { eth->dest[i] = dest_mac[i]; eth->src[i] = our_mac[i]; }
    eth->ethertype = htons(ETHERTYPE_IP);

    u16 udp_len = (u16)(sizeof(*udp) + len);
    u16 ip_len  = (u16)(sizeof(*ip) + udp_len);

    ip->version_ihl = 0x45; /* IPv4, 5 words (20 bytes, no options) */
    ip->tos = 0;
    ip->total_length = htons(ip_len);
    ip->id = 0;
    ip->flags_fragment = 0;
    ip->ttl = 64;
    ip->protocol = 17; /* UDP */
    ip->checksum = 0;
    ip->src_ip = htonl(our_ip);
    ip->dst_ip = htonl(dest_ip);
    ip->checksum = checksum16(ip, sizeof(*ip));

    udp->src_port = htons(src_port);
    udp->dst_port = htons(dest_port);
    udp->length = htons(udp_len);
    udp->checksum = 0; /* optional in IPv4, 0 means "not computed" */

    const u8 *src = data;
    for (u32 i = 0; i < len; i++) payload[i] = src[i];

    u32 frame_len = sizeof(*eth) + ip_len;
    if (frame_len < 60) frame_len = 60; /* Ethernet minimum */
    return rtl8139_send(frame, frame_len);
}
