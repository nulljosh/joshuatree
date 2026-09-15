/* Ethernet + ARP + IPv4 + UDP, just enough of each to get one datagram onto
   the wire and resolve a MAC first. All three headers are defined here
   together rather than split into eth.c/arp.c/ip.c/udp.c since the roadmap
   scopes them as one "minimal stack" item and they're small enough that
   splitting them now would just be more files to keep in sync for no
   present benefit; split later if any one of them grows real complexity. */
#include "net.h"
#include "rtl8139.h"
#include "irq.h"

typedef unsigned int   u32;
typedef unsigned short u16;
typedef unsigned char  u8;

static u16 htons(u16 v) { return (u16)((v << 8) | (v >> 8)); }
static u32 htonl(u32 v) { return (v << 24) | ((v & 0xFF00) << 8) | ((v & 0xFF0000) >> 8) | (v >> 24); }

static u8 our_mac[6];
static u32 our_ip = 0;

/* Every wait loop in this file used to be a plain iteration count, tuned
   by guessing how many empty polls a given wait "should" need. That's
   fundamentally broken: how long N iterations take in real time depends on
   host CPU load, which varies (found exactly this way: the same 50,000,000
   iteration budget covered a ~300ms DNS reply fine one run and, under
   heavier host load from a local LLM generating text, let a real inbound
   SYN sit unanswered long enough that curl's own timeout gave up first).
   ticks() is the PIT-driven counter from v1 (nominally 100/sec), a real
   improvement since it measures actual elapsed time instead of counting
   meaningless loop spins, but it still isn't a perfect wall clock in this
   environment: confirmed by direct measurement (a debug print inside the
   wait loop) that ticks() genuinely advances the whole time, just at
   roughly 40/sec instead of 100 under heavy host CPU load, because QEMU's
   own process wasn't getting scheduled by the host often enough to
   deliver its virtual PIT interrupt on time. Not a kernel bug, an
   environment one, budgets below have real margin built in for it. */
#define LAN_TIMEOUT_TICKS         600   /* ~5-15s depending on host load: same-LAN ARP is near-instant either way */
#define WAN_TIMEOUT_TICKS        2000   /* ~20-50s: a real internet round trip */
#define SLOW_REPLY_TIMEOUT_TICKS 15000  /* ~2.5-6min: local LLM generation time under load */
#define SERVE_TIMEOUT_TICKS      30000  /* ~5-12min: however long a human takes to connect, on a loaded host */

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
    u32 deadline = ticks() + LAN_TIMEOUT_TICKS;
    while (ticks() < deadline) {
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

/* arp_resolve only ever asks; nothing answered when something else asked
   "who has our_ip", which is exactly what a gateway does before it can
   deliver an inbound connection to us. Found this by testing tcp_serve_once
   against a real curl from the host through QEMU's hostfwd: the request
   never arrived, and pcap showed the gateway repeating "who-has 10.0.2.15"
   forever, we just never replied. Called from the server-side receive
   loops, not the client-side ones, since only a server needs to be findable
   by someone else first. */
static void arp_maybe_reply(const u8 *rx, u32 n) {
    if (n < sizeof(struct eth_header) + sizeof(struct arp_packet)) return;
    const struct eth_header *eth = (const struct eth_header *)rx;
    if (eth->ethertype != htons(ETHERTYPE_ARP)) return;
    const struct arp_packet *req = (const struct arp_packet *)(rx + sizeof(*eth));
    if (req->oper != htons(1)) return;              /* not a request */
    if (req->target_ip != htonl(our_ip)) return;     /* not asking about us */

    u8 frame[60] = {0};
    struct eth_header *reth = (struct eth_header *)frame;
    struct arp_packet *reply = (struct arp_packet *)(frame + sizeof(*reth));

    for (int i = 0; i < 6; i++) { reth->dest[i] = req->sender_mac[i]; reth->src[i] = our_mac[i]; }
    reth->ethertype = htons(ETHERTYPE_ARP);

    reply->htype = htons(1);
    reply->ptype = htons(0x0800);
    reply->hlen = 6; reply->plen = 4;
    reply->oper = htons(2); /* reply */
    for (int i = 0; i < 6; i++) { reply->sender_mac[i] = our_mac[i]; reply->target_mac[i] = req->sender_mac[i]; }
    reply->sender_ip = htonl(our_ip);
    reply->target_ip = req->sender_ip;

    rtl8139_send(frame, sizeof(frame));
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
    u32 deadline = ticks() + WAN_TIMEOUT_TICKS;
    while (ticks() < deadline) {
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

/* v30: a real TCP connect-scan primitive, kept separate from tcp_get
   rather than bolted onto it, since a scanner needs a short per-port
   timeout (a closed/filtered port must not cost tcp_get's ~20-50s
   WAN_TIMEOUT_TICKS wait) and never sends a request or reads a response,
   just classifies the SYN's answer. Returns 1 open (SYN-ACK seen, answered
   with RST instead of completing a real handshake, this is a scan, not a
   connection anyone needs to keep), 0 closed (a real RST came back), -1
   filtered/unreachable (timeout, no answer at all, the honest third state
   nmap itself reports, not something to collapse into "closed"). */
#define SCAN_TIMEOUT_TICKS 60 /* ~600ms, plenty for a real reply on a local/SLIRP network, fast enough to scan a real port list */
int tcp_probe_port(u32 dest_ip, u16 dest_port) {
    u8 dest_mac[6];
    if (!resolve_next_hop(dest_ip, dest_mac)) return -1;

    u16 local_port = 44100;
    u32 our_seq = 0x2000;
    if (!tcp_send_segment(dest_ip, dest_mac, local_port, dest_port, our_seq, 0, TCP_SYN, 0, 0)) return -1;
    our_seq++;

    u8 rx[1514];
    struct tcp_header *tcp;
    u8 *payload; u32 paylen;
    u32 deadline = ticks() + SCAN_TIMEOUT_TICKS;
    while (ticks() < deadline) {
        u32 n = rtl8139_receive(rx, sizeof(rx));
        if (n == 0) continue;
        if (!tcp_match(dest_ip, local_port, dest_port, rx, n, &tcp, &payload, &paylen)) continue;
        if (tcp->flags & TCP_RST) return 0; /* closed: the target itself said so */
        if ((tcp->flags & (TCP_SYN | TCP_ACK)) == (TCP_SYN | TCP_ACK)) {
            u32 their_seq = htonl(tcp->seq) + 1;
            tcp_send_segment(dest_ip, dest_mac, local_port, dest_port, our_seq, their_seq, TCP_RST, 0, 0); /* abort cleanly, this was only ever a probe */
            return 1; /* open */
        }
    }
    return -1; /* filtered/unreachable: nothing answered in time */
}

int tcp_get(u32 dest_ip, u16 dest_port, const void *request, u32 request_len,
            void *response, u32 response_maxlen) {
    if (request_len > TCP_MAX_PAYLOAD) return -1;
    u8 dest_mac[6];
    if (!resolve_next_hop(dest_ip, dest_mac)) return -1;

    /* v75: a fresh local port per connection. A fixed 44000 was fine while
       every caller spoke to a different host once per boot (ip-api, then
       Open-Meteo); the map wallpaper opens four back-to-back connections
       to the same server, and the fourth SYN on an identical 4-tuple hit
       one SLIRP still held half-closed (this side sends its FIN and
       never waits for the final ACK, by design), reproducibly failing
       on tile 3 of 4 (`wallerr=http 3`). Ephemeral range, wraps. */
    static u16 next_port = 44000;
    u16 local_port = next_port++;
    if (next_port >= 60000) next_port = 44000;
    u32 our_seq = 0x1000 + ((u32)local_port << 8); /* toy ISN, varied per connection so a stale segment from the last one can't match */
    u32 their_seq = 0;

    if (!tcp_send_segment(dest_ip, dest_mac, local_port, dest_port, our_seq, 0, TCP_SYN, 0, 0)) return -1;
    our_seq++;

    u8 rx[1514];
    struct tcp_header *tcp;
    u8 *payload;
    u32 paylen;
    int got_synack = 0;
    u32 synack_deadline = ticks() + WAN_TIMEOUT_TICKS;
    while (ticks() < synack_deadline && !got_synack) {
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
    u32 data_deadline = ticks() + SLOW_REPLY_TIMEOUT_TICKS;
    while (ticks() < data_deadline && !got_fin && total < response_maxlen) {
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

/* ---- TCP server side: one connection at a time, passive open. Waits for
   a SYN on port, handshakes, discards whatever request comes in (there's
   only one thing to serve), sends response, closes. The client's MAC
   comes straight off the SYN frame's own Ethernet header, no ARP needed,
   this kernel already has the one piece of information ARP exists to
   provide. ---- */
int tcp_serve_once(u16 port, const void *response, u32 response_len) {
    u8 rx[1514];
    u32 client_ip = 0;
    u16 client_port = 0;
    u8 client_mac[6];
    u32 their_seq = 0;

    int got_syn = 0;
    u32 syn_deadline = ticks() + SERVE_TIMEOUT_TICKS;
    while (ticks() < syn_deadline && !got_syn) {
        u32 n = rtl8139_receive(rx, sizeof(rx));
        if (n == 0) continue;
        arp_maybe_reply(rx, n); /* the gateway can't deliver anything to us until it knows our MAC */
        if (n < sizeof(struct eth_header) + sizeof(struct ip_header) + sizeof(struct tcp_header)) continue;

        struct eth_header *eth = (struct eth_header *)rx;
        if (eth->ethertype != htons(ETHERTYPE_IP)) continue;
        struct ip_header *ip = (struct ip_header *)(rx + sizeof(*eth));
        if (ip->protocol != 6) continue;
        u32 ip_hlen = (u32)(ip->version_ihl & 0x0F) * 4;
        struct tcp_header *tcp = (struct tcp_header *)((u8 *)ip + ip_hlen);
        if (tcp->dst_port != htons(port)) continue;
        if ((tcp->flags & (TCP_SYN | TCP_ACK)) != TCP_SYN) continue; /* a pure SYN opens a new connection */

        for (int i = 0; i < 6; i++) client_mac[i] = eth->src[i];
        client_ip = htonl(ip->src_ip);
        client_port = htons(tcp->src_port);
        their_seq = htonl(tcp->seq) + 1;
        got_syn = 1;
    }
    if (!got_syn) return 0;

    u16 local_port = port;
    u32 our_seq = 0x2000; /* toy ISN, same reasoning as tcp_get's */

    tcp_send_segment(client_ip, client_mac, local_port, client_port, our_seq, their_seq, TCP_SYN | TCP_ACK, 0, 0);
    our_seq++;

    struct tcp_header *tcp;
    u8 *payload;
    u32 paylen;
    int got_request = 0;
    u32 request_deadline = ticks() + WAN_TIMEOUT_TICKS;
    while (ticks() < request_deadline && !got_request) {
        u32 n = rtl8139_receive(rx, sizeof(rx));
        if (n == 0) continue;
        arp_maybe_reply(rx, n);
        if (!tcp_match(client_ip, local_port, client_port, rx, n, &tcp, &payload, &paylen)) continue;
        if (htonl(tcp->seq) != their_seq) continue;
        if (paylen > 0) { their_seq += paylen; got_request = 1; } /* don't care what it says, only one page to serve */
    }
    if (!got_request) return 0;

    tcp_send_segment(client_ip, client_mac, local_port, client_port, our_seq, their_seq, TCP_ACK, 0, 0);

    /* Stop-and-wait, one segment outstanding at a time: send a chunk, wait
       for its ACK before sending the next. No pipelining, no window
       scaling, but correct, and a real page is bigger than one segment
       (found immediately: a single-segment cap made anything beyond a toy
       string silently truncate). */
    const u8 *body = response;
    u32 sent = 0;
    while (sent < response_len) {
        u32 chunk = response_len - sent;
        if (chunk > TCP_MAX_PAYLOAD) chunk = TCP_MAX_PAYLOAD;

        tcp_send_segment(client_ip, client_mac, local_port, client_port, our_seq, their_seq, TCP_PSH | TCP_ACK, body + sent, chunk);
        u32 expect_ack = our_seq + chunk;

        int got_ack = 0;
        u32 ack_deadline = ticks() + WAN_TIMEOUT_TICKS;
        while (ticks() < ack_deadline && !got_ack) {
            u32 n = rtl8139_receive(rx, sizeof(rx));
            if (n == 0) continue;
            if (!tcp_match(client_ip, local_port, client_port, rx, n, &tcp, &payload, &paylen)) continue;
            if ((tcp->flags & TCP_ACK) && htonl(tcp->ack) == expect_ack) got_ack = 1;
        }
        if (!got_ack) return 0; /* real failure, the client never got this chunk, don't claim success */

        our_seq = expect_ack;
        sent += chunk;
    }

    tcp_send_segment(client_ip, client_mac, local_port, client_port, our_seq, their_seq, TCP_FIN | TCP_ACK, 0, 0);
    our_seq++;

    return 1;
}
