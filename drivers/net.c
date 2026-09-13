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

/* ARP replies come from the same LAN segment (QEMU's own SLIRP gateway),
   basically instant, so a short poll budget is fine there. DNS and TCP
   replies cross SLIRP's NAT out to the real internet, real round-trip time,
   found by getting burned once: a real DNS reply that took ~300ms real time
   still lost the race against a poll budget that had been tuned against an
   earlier ~14ms reply. Generous and iteration-based since there's no timer
   wired into this driver, not calibrated to a real duration. */
#define LAN_TIMEOUT_ITERS 2000000
#define WAN_TIMEOUT_ITERS 50000000

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
    u8 frame[60] = {0}; /* Ethernet's minimum frame size; the header+ARP
                           payload is only 42 bytes and a short frame never
                           makes it onto the wire (found via real pcap
                           capture: rtl8139_send silently failed on this). */
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
    for (int attempts = 0; attempts < LAN_TIMEOUT_ITERS; attempts++) {
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

/* ARP only ever answers for a host on the same physical link. A real
   internet destination isn't on QEMU SLIRP's virtual /24, ARPing it
   directly gets silence forever (found exactly this way: a real TCP
   handshake to a real internet host hung until pcap showed an ARP
   broadcast for a WAN address that nothing on the LAN could ever answer).
   The fix is the one routing rule that actually matters here: same
   subnet, ARP it directly; otherwise ARP the default gateway and hand it
   the frame, the IP header still carries the real destination, the
   gateway does the actual routing. Not a routing table, one hardcoded
   default route, which is genuinely all this scope needs. */
#define LOCAL_SUBNET_MASK  0xFFFFFF00u /* /24, matches SLIRP's fixed subnet shape */
#define DEFAULT_GATEWAY_IP 0x0A000202u /* 10.0.2.2, SLIRP's fixed gateway */

static int resolve_next_hop(u32 dest_ip, u8 mac_out[6]) {
    u32 next_hop = ((dest_ip ^ our_ip) & LOCAL_SUBNET_MASK) ? DEFAULT_GATEWAY_IP : dest_ip;
    return arp_resolve(next_hop, mac_out);
}

int udp_send(u32 dest_ip, u16 dest_port, u16 src_port, const void *data, u32 len) {
    u8 dest_mac[6];
    if (!resolve_next_hop(dest_ip, dest_mac)) return 0;

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

#define DNS_PORT     53
#define DNS_SRC_PORT 53000

/* Writes a DNS header + single question (QTYPE A, QCLASS IN) for hostname
   into buf, returns the query's total length. */
static u32 dns_build_query(u8 *buf, const char *hostname, u16 id) {
    u16 *hdr = (u16 *)buf;
    hdr[0] = htons(id);
    hdr[1] = htons(0x0100); /* standard query, recursion desired */
    hdr[2] = htons(1);      /* QDCOUNT */
    hdr[3] = 0; hdr[4] = 0; hdr[5] = 0; /* ANCOUNT/NSCOUNT/ARCOUNT */

    u8 *p = buf + 12;
    const char *label = hostname;
    while (*label) {
        const char *dot = label;
        while (*dot && *dot != '.') dot++;
        u8 len = (u8)(dot - label);
        *p++ = len;
        for (u8 i = 0; i < len; i++) *p++ = (u8)label[i];
        label = (*dot == '.') ? dot + 1 : dot;
    }
    *p++ = 0; /* root label */
    *p++ = 0; *p++ = 1; /* QTYPE A */
    *p++ = 0; *p++ = 1; /* QCLASS IN */
    return (u32)(p - buf);
}

/* Advances past one DNS name occurrence (a real label sequence or a 2-byte
   compression pointer), returns the position right after it. */
static const u8 *dns_skip_name(const u8 *p) {
    while (*p) {
        if ((*p & 0xC0) == 0xC0) return p + 2;
        p += *p + 1;
    }
    return p + 1;
}

int dns_resolve(const char *hostname, u32 dns_server_ip, u32 *ip_out) {
    u8 query[256];
    u32 qlen = dns_build_query(query, hostname, 0x1234);
    if (!udp_send(dns_server_ip, DNS_PORT, DNS_SRC_PORT, query, qlen)) return 0;

    u8 rx[1514];
    for (int attempts = 0; attempts < WAN_TIMEOUT_ITERS; attempts++) {
        u32 n = rtl8139_receive(rx, sizeof(rx));
        if (n < sizeof(struct eth_header) + sizeof(struct ip_header) + sizeof(struct udp_header)) continue;

        struct eth_header *eth = (struct eth_header *)rx;
        if (eth->ethertype != htons(ETHERTYPE_IP)) continue;
        struct ip_header *ip = (struct ip_header *)(rx + sizeof(*eth));
        if (ip->protocol != 17) continue; /* UDP */
        u32 ip_hlen = (u32)(ip->version_ihl & 0x0F) * 4;
        struct udp_header *udp = (struct udp_header *)(rx + sizeof(*eth) + ip_hlen);
        if (udp->src_port != htons(DNS_PORT) || udp->dst_port != htons(DNS_SRC_PORT)) continue;

        const u8 *dns = rx + sizeof(*eth) + ip_hlen + sizeof(*udp);
        u16 ancount = htons(*(u16 *)(dns + 6));
        if (ancount == 0) return 0; /* NXDOMAIN or no A record, not a timeout */

        const u8 *p = dns + 12;
        p = dns_skip_name(p); /* question name */
        p += 4; /* QTYPE + QCLASS */

        for (u16 i = 0; i < ancount; i++) {
            p = dns_skip_name(p);
            u16 type = htons(*(u16 *)p); p += 2;
            p += 2; /* class */
            p += 4; /* ttl */
            u16 rdlength = htons(*(u16 *)p); p += 2;
            if (type == 1 && rdlength == 4) { /* A record */
                *ip_out = ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3];
                return 1;
            }
            p += rdlength;
        }
        return 0; /* answers present but none were an A record */
    }
    return 0;
}

/* ---- TCP: one connection at a time, synchronous, no retransmission or
   congestion control. Enough to do a real handshake, push one request,
   and read back a response, which is everything a raw HTTP GET needs. ---- */

struct tcp_header {
    u16 src_port, dst_port;
    u32 seq, ack;
    u8  data_offset; /* top 4 bits: header length in 32-bit words */
    u8  flags;
    u16 window;
    u16 checksum;
    u16 urgent;
} __attribute__((packed));

#define TCP_FIN 0x01
#define TCP_SYN 0x02
#define TCP_RST 0x04
#define TCP_PSH 0x08
#define TCP_ACK 0x10

#define TCP_MAX_PAYLOAD 536

static int tcp_send_segment(u32 dest_ip, const u8 dest_mac[6], u16 local_port, u16 remote_port,
                             u32 seq, u32 ack, u8 flags, const void *payload, u32 paylen) {
    if (paylen > TCP_MAX_PAYLOAD) return 0;
    u8 frame[sizeof(struct eth_header) + sizeof(struct ip_header) + sizeof(struct tcp_header) + TCP_MAX_PAYLOAD];
    struct eth_header *eth = (struct eth_header *)frame;
    struct ip_header  *ip  = (struct ip_header *)(frame + sizeof(*eth));
    struct tcp_header *tcp = (struct tcp_header *)(frame + sizeof(*eth) + sizeof(*ip));
    u8 *pl = frame + sizeof(*eth) + sizeof(*ip) + sizeof(*tcp);

    for (int i = 0; i < 6; i++) { eth->dest[i] = dest_mac[i]; eth->src[i] = our_mac[i]; }
    eth->ethertype = htons(ETHERTYPE_IP);

    u16 tcp_len = (u16)(sizeof(*tcp) + paylen);
    u16 ip_len  = (u16)(sizeof(*ip) + tcp_len);

    ip->version_ihl = 0x45;
    ip->tos = 0;
    ip->total_length = htons(ip_len);
    ip->id = 0;
    ip->flags_fragment = 0;
    ip->ttl = 64;
    ip->protocol = 6; /* TCP */
    ip->checksum = 0;
    ip->src_ip = htonl(our_ip);
    ip->dst_ip = htonl(dest_ip);
    ip->checksum = checksum16(ip, sizeof(*ip));

    tcp->src_port = htons(local_port);
    tcp->dst_port = htons(remote_port);
    tcp->seq = htonl(seq);
    tcp->ack = htonl(ack);
    tcp->data_offset = 5 << 4; /* 5 words = 20 bytes, no options */
    tcp->flags = flags;
    tcp->window = htons(8192);
    tcp->checksum = 0;
    tcp->urgent = 0;

    const u8 *psrc = payload;
    for (u32 i = 0; i < paylen; i++) pl[i] = psrc[i];

    /* TCP checksum covers a pseudo-header (src/dst IP, zero, protocol, TCP
       length) plus the TCP header and payload, none of which are contiguous
       with the pseudo-header in the real frame, so it's built separately. */
    u8 csum_buf[12 + sizeof(struct tcp_header) + TCP_MAX_PAYLOAD];
    *(u32 *)(csum_buf + 0) = htonl(our_ip);
    *(u32 *)(csum_buf + 4) = htonl(dest_ip);
    csum_buf[8] = 0;
    csum_buf[9] = 6; /* protocol TCP */
    *(u16 *)(csum_buf + 10) = htons(tcp_len);
    const u8 *tcp_bytes = (const u8 *)tcp;
    for (u32 i = 0; i < tcp_len; i++) csum_buf[12 + i] = tcp_bytes[i];
    tcp->checksum = checksum16(csum_buf, 12 + tcp_len);

    u32 frame_len = sizeof(*eth) + ip_len;
    if (frame_len < 60) frame_len = 60;
    return rtl8139_send(frame, frame_len);
}

/* Checks one already-received frame against an expected TCP connection;
   fills out_tcp/out_payload/out_paylen and returns 1 on a match. */
static int tcp_match(u32 dest_ip, u16 local_port, u16 remote_port, u8 *rx, u32 n,
                      struct tcp_header **out_tcp, u8 **out_payload, u32 *out_paylen) {
    if (n < sizeof(struct eth_header) + sizeof(struct ip_header) + sizeof(struct tcp_header)) return 0;
    struct eth_header *eth = (struct eth_header *)rx;
    if (eth->ethertype != htons(ETHERTYPE_IP)) return 0;
    struct ip_header *ip = (struct ip_header *)(rx + sizeof(*eth));
    if (ip->protocol != 6) return 0;
    if (ip->src_ip != htonl(dest_ip)) return 0;
    u32 ip_hlen = (u32)(ip->version_ihl & 0x0F) * 4;
    struct tcp_header *tcp = (struct tcp_header *)((u8 *)ip + ip_hlen);
    if (tcp->src_port != htons(remote_port) || tcp->dst_port != htons(local_port)) return 0;
    u32 tcp_hlen = (u32)((tcp->data_offset >> 4) & 0xF) * 4;
    u16 ip_total = htons(ip->total_length);
    if (ip_total < ip_hlen + tcp_hlen) return 0; /* malformed/truncated, ignore */

    *out_tcp = tcp;
    *out_payload = (u8 *)tcp + tcp_hlen;
    *out_paylen = ip_total - ip_hlen - tcp_hlen;
    return 1;
}

int tcp_get(u32 dest_ip, u16 dest_port, const void *request, u32 request_len,
            void *response, u32 response_maxlen) {
    if (request_len > TCP_MAX_PAYLOAD) return -1;
    u8 dest_mac[6];
    if (!resolve_next_hop(dest_ip, dest_mac)) return -1;

    u16 local_port = 44000;
    u32 our_seq = 0x1000; /* toy ISN, no peer to confuse since we open the connection */
    u32 their_seq = 0;

    if (!tcp_send_segment(dest_ip, dest_mac, local_port, dest_port, our_seq, 0, TCP_SYN, 0, 0)) return -1;
    our_seq++;

    u8 rx[1514];
    struct tcp_header *tcp;
    u8 *payload;
    u32 paylen;
    int got_synack = 0;
    for (int attempts = 0; attempts < WAN_TIMEOUT_ITERS && !got_synack; attempts++) {
        u32 n = rtl8139_receive(rx, sizeof(rx));
        if (n == 0) continue;
        if (!tcp_match(dest_ip, local_port, dest_port, rx, n, &tcp, &payload, &paylen)) continue;
        if ((tcp->flags & (TCP_SYN | TCP_ACK)) == (TCP_SYN | TCP_ACK) && htonl(tcp->ack) == our_seq) {
            their_seq = htonl(tcp->seq) + 1;
            got_synack = 1;
        }
    }
    if (!got_synack) return -1;

    tcp_send_segment(dest_ip, dest_mac, local_port, dest_port, our_seq, their_seq, TCP_ACK, 0, 0);
    tcp_send_segment(dest_ip, dest_mac, local_port, dest_port, our_seq, their_seq, TCP_PSH | TCP_ACK, request, request_len);
    our_seq += request_len;

    u32 total = 0;
    int got_fin = 0;
    for (int attempts = 0; attempts < WAN_TIMEOUT_ITERS && !got_fin && total < response_maxlen; attempts++) {
        u32 n = rtl8139_receive(rx, sizeof(rx));
        if (n == 0) continue;
        if (!tcp_match(dest_ip, local_port, dest_port, rx, n, &tcp, &payload, &paylen)) continue;
        u32 seg_seq = htonl(tcp->seq);
        if (seg_seq != their_seq) continue; /* out of order or a duplicate; no reassembly, just drop it */

        if (paylen > 0) {
            u32 copy = paylen;
            if (total + copy > response_maxlen) copy = response_maxlen - total;
            u8 *dst = (u8 *)response + total;
            for (u32 i = 0; i < copy; i++) dst[i] = payload[i];
            total += copy;
            their_seq += paylen;
            tcp_send_segment(dest_ip, dest_mac, local_port, dest_port, our_seq, their_seq, TCP_ACK, 0, 0);
        }
        if (tcp->flags & TCP_FIN) {
            their_seq += 1;
            tcp_send_segment(dest_ip, dest_mac, local_port, dest_port, our_seq, their_seq, TCP_ACK, 0, 0);
            got_fin = 1;
        }
    }

    /* ponytail: best-effort close, don't wait for the final ACK back. A
       one-shot GET doesn't need a clean TIME_WAIT, the server tears its
       side down regardless once it sees this FIN. */
    tcp_send_segment(dest_ip, dest_mac, local_port, dest_port, our_seq, their_seq, TCP_FIN | TCP_ACK, 0, 0);

    return (int)total;
}
