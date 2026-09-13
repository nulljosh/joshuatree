#include "http.h"
#include "net.h"

typedef unsigned int u32;

/* SLIRP's fixed built-in DNS proxy, same one nettest already uses. Not a
   parameter since there's no resolv.conf-equivalent to read it from yet. */
#define SLIRP_DNS_IP 0x0A000203u

int http_get(const char *host, const char *path, unsigned short port,
             void *body_out, unsigned int body_maxlen) {
    u32 ip;
    if (!dns_resolve(host, SLIRP_DNS_IP, &ip)) return -1;

    char req[512];
    u32 n = 0;
    const char *parts[5];
    parts[0] = "GET "; parts[1] = path; parts[2] = " HTTP/1.0\r\nHost: ";
    parts[3] = host;   parts[4] = "\r\nConnection: close\r\n\r\n";
    for (int p = 0; p < 5; p++) {
        const char *s = parts[p];
        while (*s && n < sizeof(req) - 1) req[n++] = *s++;
    }

    char raw[2048];
    int total = tcp_get(ip, port, req, n, raw, sizeof(raw) - 1);
    if (total < 0) return -1;

    /* body starts right after the blank line ending the headers */
    int body_start = -1;
    for (int i = 0; i + 3 < total; i++) {
        if (raw[i] == '\r' && raw[i + 1] == '\n' && raw[i + 2] == '\r' && raw[i + 3] == '\n') {
            body_start = i + 4;
            break;
        }
    }
    if (body_start < 0) return 0; /* headers never finished within raw's cap, nothing usable */

    u32 body_len = (u32)(total - body_start);
    u32 copy = body_len < body_maxlen ? body_len : body_maxlen;
    char *out = body_out;
    for (u32 i = 0; i < copy; i++) out[i] = raw[body_start + i];
    return (int)copy;
}
