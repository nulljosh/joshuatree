/* A DNS reply read with every step inside its n bytes (joshuatree 6.48.5). Before this, net.c walked the reply with
   no bounds at all: the IP header length, each name and each answer's rdlength came from the packet and could carry
   the read far past the 1514-byte frame, and a reply only needs the 16-bit query id (taken from the tick count) to be
   believed. Header only, so tools/fuzz-host/fuzz_dns.c tests the same code net.c runs. */
#ifndef DNS_H
#define DNS_H
static int dns_skip(const unsigned char *m, unsigned n, unsigned *p) {   /* past one name at *p: 1, or 0 when it runs out */
    while (*p < n && m[*p]) {
        if ((m[*p] & 0xC0) == 0xC0) { if (*p + 2 > n) return 0; *p += 2; return 1; }   /* a pointer ends the name */
        *p += (unsigned)m[*p] + 1;
    }
    if (*p >= n) return 0;
    (*p)++;
    return 1;
}
/* 1 with *ip_out set to the first A record; 0 for an answer with none (NXDOMAIN, or other types only); -1 when it is
   not the reply to txid, or is too short or broken (keep waiting). */
static int dns_answer(const unsigned char *m, unsigned n, unsigned short txid, unsigned *ip_out) {
    if (n < 12 || (unsigned short)(m[0] << 8 | m[1]) != txid) return -1;
    unsigned an = (unsigned)m[6] << 8 | m[7], p = 12;
    if (!an) return 0;
    if (!dns_skip(m, n, &p) || p + 4 > n) return -1;   /* the question, then QTYPE and QCLASS */
    p += 4;
    for (unsigned i = 0; i < an; i++) {
        if (!dns_skip(m, n, &p) || p + 10 > n) return -1;
        unsigned type = (unsigned)m[p] << 8 | m[p + 1], rdl = (unsigned)m[p + 8] << 8 | m[p + 9];
        p += 10;
        if (rdl > n - p) return -1;
        if (type == 1 && rdl == 4) { *ip_out = (unsigned)m[p] << 24 | (unsigned)m[p + 1] << 16 | (unsigned)m[p + 2] << 8 | m[p + 3]; return 1; }
        p += rdl;
    }
    return 0;
}
#endif
