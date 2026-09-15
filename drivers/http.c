#include "http.h"
#include "kheap.h"
#include "net.h"

typedef unsigned int u32;

/* SLIRP's fixed built-in DNS proxy, same one nettest already uses. Not a
   parameter since there's no resolv.conf-equivalent to read it from yet. */
#define SLIRP_DNS_IP 0x0A000203u

/* Recognizes a plain dotted-quad ("10.0.2.2") without touching the network,
   so a literal IP (like the host machine's gateway address) never takes a
   pointless round trip through dns_resolve, which would just fail on it.
   Returns 1 and fills out on a clean full-string match, 0 otherwise (falls
   through to a real DNS lookup). */
static int parse_ipv4_literal(const char *s, u32 *out) {
    u32 ip = 0;
    for (int octet = 0; octet < 4; octet++) {
        if (octet > 0) { if (*s != '.') return 0; s++; }
        int val = 0, digits = 0;
        while (*s >= '0' && *s <= '9') { val = val * 10 + (*s - '0'); s++; digits++; if (val > 255) return 0; }
        if (digits == 0) return 0;
        ip = (ip << 8) | (u32)val;
    }
    if (*s != 0) return 0;
    *out = ip;
    return 1;
}

static int resolve_host(const char *host, u32 *ip) {
    if (parse_ipv4_literal(host, ip)) return 1;
    return dns_resolve(host, SLIRP_DNS_IP, ip);
}

/* Both http_get and http_post send a request buffer then strip the status
   line and headers down to just the body; shared here since that part is
   identical either way. */
static int http_body_only(u32 ip, unsigned short port, const char *req, u32 req_len,
                           void *body_out, u32 body_maxlen, char *raw, u32 raw_cap) {
    int total = tcp_get(ip, port, req, req_len, raw, raw_cap - 1);
    if (total < 0) return -1;

    int body_start = -1;
    for (int i = 0; i + 3 < total; i++) {
        if (raw[i] == '\r' && raw[i + 1] == '\n' && raw[i + 2] == '\r' && raw[i + 3] == '\n') {
            body_start = i + 4;
            break;
        }
    }
    if (body_start < 0) return 0;

    u32 body_len = (u32)(total - body_start);
    u32 copy = body_len < body_maxlen ? body_len : body_maxlen;
    char *out = body_out;
    for (u32 i = 0; i < copy; i++) out[i] = raw[body_start + i];
    return (int)copy;
}

int http_get(const char *host, const char *path, unsigned short port,
             void *body_out, unsigned int body_maxlen) {
    u32 ip;
    if (!resolve_host(host, &ip)) return -1;

    char req[512];
    u32 n = 0;
    const char *parts[5];
    parts[0] = "GET "; parts[1] = path; parts[2] = " HTTP/1.0\r\nHost: ";
    parts[3] = host;   parts[4] = "\r\nConnection: close\r\n\r\n";
    for (int p = 0; p < 5; p++) {
        const char *s = parts[p];
        while (*s && n < sizeof(req) - 1) req[n++] = *s++;
    }

    /* v75: heap-backed, sized to the caller's body cap. The old 2KB stack
       buffer silently capped every reply (headers + body) at 2047 bytes,
       fine for JSON one-liners, hopeless for a 37KB map tile. Headers
       from a CDN run ~700 bytes; 2048 of slack keeps that honest. */
    unsigned int raw_cap = body_maxlen + 2048;
    char *raw = kmalloc(raw_cap);
    if (!raw) return -1;
    int r = http_body_only(ip, port, req, n, body_out, body_maxlen, raw, raw_cap);
    kfree(raw);
    return r;
}

static void putn_into(char *buf, u32 *pos, u32 cap, u32 v) {
    char digits[12]; int nd = 0;
    if (v == 0) digits[nd++] = '0';
    while (v) { digits[nd++] = (char)('0' + v % 10); v /= 10; }
    while (nd) { if (*pos < cap) buf[(*pos)++] = digits[--nd]; }
}

int http_post(const char *host, const char *path, unsigned short port,
              const char *body, unsigned int body_len,
              void *response_out, unsigned int response_maxlen) {
    u32 ip;
    if (!resolve_host(host, &ip)) return -1;

    char req[1024];
    u32 n = 0;
    const char *parts[4];
    parts[0] = "POST "; parts[1] = path; parts[2] = " HTTP/1.0\r\nHost: "; parts[3] = host;
    for (int p = 0; p < 4; p++) {
        const char *s = parts[p];
        while (*s && n < sizeof(req) - 1) req[n++] = *s++;
    }
    const char *ct = "\r\nContent-Type: application/json\r\nContent-Length: ";
    while (*ct && n < sizeof(req) - 1) req[n++] = *ct++;
    putn_into(req, &n, sizeof(req) - 1, body_len);
    const char *tail = "\r\nConnection: close\r\n\r\n";
    while (*tail && n < sizeof(req) - 1) req[n++] = *tail++;
    for (unsigned int i = 0; i < body_len && n < sizeof(req) - 1; i++) req[n++] = body[i];

    char raw[8192]; /* headroom for a generated-HTML response, not just a short chat reply */
    return http_body_only(ip, port, req, n, response_out, response_maxlen, raw, sizeof(raw));
}
