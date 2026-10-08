#include "http.h"
#include "kheap.h"
#include "net.h"
#include "irq.h"

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
int http_resolve_host(const char *host, unsigned int *ip) { return resolve_host(host, ip); }

/* "HTTP/1.x NNN": the three digits after the first space, 0 when there is no status line. */
int http_status_of(const char *raw, unsigned int total) {
    if (total >= 12 && raw[0] == 'H' && raw[1] == 'T' && raw[2] == 'T' && raw[3] == 'P') {
        unsigned int i = 4;
        while (i < total && raw[i] != ' ' && raw[i] != '\r') i++;
        if (i + 3 < total && raw[i] == ' ' && raw[i+1] >= '0' && raw[i+1] <= '9' && raw[i+2] >= '0' && raw[i+2] <= '9' && raw[i+3] >= '0' && raw[i+3] <= '9')
            return (raw[i+1] - '0') * 100 + (raw[i+2] - '0') * 10 + (raw[i+3] - '0');
    }
    return 0;
}
/* Where the body starts (after the blank line), -1 when the headers never ended. */
int http_body_start(const char *raw, unsigned int total) {
    for (unsigned int i = 0; i + 3 < total; i++)
        if (raw[i] == '\r' && raw[i + 1] == '\n' && raw[i + 2] == '\r' && raw[i + 3] == '\n') return (int)i + 4;
    return -1;
}

/* Both http_get and http_post send a request buffer then strip the status
   line and headers down to just the body; shared here since that part is
   identical either way. */
static int last_status = 0;
/* 1.9.23: net.c is a one-connection stack and this file keeps one status.
   The desktop (Burrow, Weather) and a ring-3 SYS_HTTP_GET (which runs with
   interrupts on while it waits) can both be inside a fetch, so a second
   entry while one is in flight fails fast instead of sharing the socket. */
static int in_flight = 0;
int http_last_status(void) { return last_status; }

/* 2.0.0: a ring-3 fetch used to fail with -EIO the instant the desktop (the Weather window's geocode,
   ip-api and forecast calls, each able to sit out a 20 s DNS or connect wait) held the one connection,
   and every fetch after it failed the same way until that call ended: Samantha's 72 face frames hit it
   from about the 23rd. net.c is still one connection at a time, so the honest fix is to queue behind
   the holder for a bounded time instead of treating it as a network error. Needs interrupts on (the
   caller sets IF) so ticks advance and the desktop's own fetch keeps running to completion. Returns 1
   once the stack is free, 0 if it was still held when max_ticks ran out (the caller says -EBUSY). */
int http_wait_idle(unsigned int max_ticks) {
    unsigned int deadline = ticks() + max_ticks;
    while (in_flight) {
        if ((int)(ticks() - deadline) >= 0) return 0;
#if defined(__i386__)
        __asm__ volatile ("sti; hlt"); /* kernel only: the host fuzz build never calls this */
#endif
    }
    return 1;
}

static int http_body_only(u32 ip, unsigned short port, const char *req, u32 req_len,
                           void *body_out, u32 body_maxlen, char *raw, u32 raw_cap, u32 reply_timeout_ticks) {
    last_status = 0;
    int total = tcp_get_timeout(ip, port, req, req_len, raw, raw_cap - 1, reply_timeout_ticks);
    if (total < 0) return -1;

    last_status = http_status_of(raw, (u32)total);
    int body_start = http_body_start(raw, (u32)total);
    if (body_start < 0) return 0;

    u32 body_len = (u32)(total - body_start);
    u32 copy = body_len < body_maxlen ? body_len : body_maxlen;
    char *out = body_out;
    for (u32 i = 0; i < copy; i++) out[i] = raw[body_start + i];
    return (int)copy;
}

static int http_get_locked(u32 ip, const char *host, const char *path, unsigned short port, void *body_out, unsigned int body_maxlen, unsigned int reply_timeout_ticks);
static int http_post_locked(u32 ip, const char *host, const char *path, unsigned short port, const char *body, unsigned int body_len, void *response_out, unsigned int response_maxlen, unsigned int reply_timeout_ticks);
int http_get(const char *host, const char *path, unsigned short port,
             void *body_out, unsigned int body_maxlen) {
    return http_get_timeout(host, path, port, body_out, body_maxlen, 0);
}

int http_get_timeout(const char *host, const char *path, unsigned short port,
                     void *body_out, unsigned int body_maxlen, unsigned int reply_timeout_ticks) {
    u32 ip;
    last_status = 0;
    if (in_flight) return -1;
    in_flight = 1;
    int r = -1;
    if (resolve_host(host, &ip)) r = http_get_locked(ip, host, path, port, body_out, body_maxlen, reply_timeout_ticks);
    in_flight = 0;
    return r;
}
static int http_get_locked(u32 ip, const char *host, const char *path, unsigned short port,
                           void *body_out, unsigned int body_maxlen, unsigned int reply_timeout_ticks) {

    char req[1024]; /* 2.10.0: the Weather request line alone is ~400 bytes; was 512 */
    u32 n = 0;
    /* v0.73.2: real bug, found chasing the satellite-wallpaper task's
       "wallerr=http" failure. A raw `nc` replay of this exact request
       against Google's real tile server got a 403 "unusual traffic" bot
       page; the same request with one added User-Agent header got a real
       200. This kernel sent literally no User-Agent on any HTTP request,
       ever, which plenty of hosts tolerate (OpenTopoMap, Open-Meteo,
       ip-api all do) but Google's abuse detection doesn't. Real, honest
       identification, not a spoofed browser string. */
    const char *parts[6];
    parts[0] = "GET "; parts[1] = path; parts[2] = " HTTP/1.0\r\nHost: ";
    parts[3] = host;   parts[4] = "\r\nUser-Agent: JoshuaTree/1.0\r\nConnection: close\r\n\r\n";
    parts[5] = 0;
    for (int p = 0; parts[p]; p++) {
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
    int r = http_body_only(ip, port, req, n, body_out, body_maxlen, raw, raw_cap, reply_timeout_ticks);
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
    return http_post_timeout(host, path, port, body, body_len, response_out, response_maxlen, 0);
}

/* Bearer for the next POST only (SYS_HTTP_POST sets it for /api/mail/send); cleared as soon as
   the request is built, so no later request can carry it. */
static const char *post_bearer = 0;
void http_post_set_bearer(const char *token) { post_bearer = (token && token[0]) ? token : 0; }

int http_post_timeout(const char *host, const char *path, unsigned short port,
                       const char *body, unsigned int body_len,
                       void *response_out, unsigned int response_maxlen,
                       unsigned int reply_timeout_ticks) {
    u32 ip;
    if (in_flight) return -1;
    in_flight = 1;
    int r = -1;
    if (resolve_host(host, &ip)) r = http_post_locked(ip, host, path, port, body, body_len, response_out, response_maxlen, reply_timeout_ticks);
    in_flight = 0;
    return r;
}
static int http_post_locked(u32 ip, const char *host, const char *path, unsigned short port,
                            const char *body, unsigned int body_len,
                            void *response_out, unsigned int response_maxlen,
                            unsigned int reply_timeout_ticks) {

    /* v85 (chat history / /api/chat): this used to be a fixed 1024-byte
       stack buffer, which silently truncated ANY POST body over roughly
       900 bytes (headers eat the rest) regardless of how big a buffer the
       caller passed to json/kernel.c above it, so growing kernel.c's own
       req_body was a no-op against this real second cap, found by
       actually tracing the call chain rather than assuming a bigger
       caller buffer alone was enough. Heap-backed now, sized to the
       caller's real body_len plus a fixed small allowance for the
       request-line/header bytes this function itself writes (path can be
       long; ~200 bytes of slack covers any realistic path plus headers),
       the same pattern http_get already uses for its own raw response
       buffer just below. */
    const char *bearer = post_bearer;
    post_bearer = 0;
    u32 bearer_len = 0;
    while (bearer && bearer[bearer_len]) bearer_len++;
    u32 req_cap = body_len + 256 + (bearer ? bearer_len + 32 : 0);
    char *req = kmalloc(req_cap);
    if (!req) return -1;
    u32 n = 0;
    const char *parts[4];
    parts[0] = "POST "; parts[1] = path; parts[2] = " HTTP/1.0\r\nHost: "; parts[3] = host;
    for (int p = 0; p < 4; p++) {
        const char *s = parts[p];
        while (*s && n < req_cap - 1) req[n++] = *s++;
    }
    if (bearer) {
        const char *ah = "\r\nAuthorization: Bearer ";
        while (*ah && n < req_cap - 1) req[n++] = *ah++;
        for (u32 bi = 0; bi < bearer_len && n < req_cap - 1; bi++) req[n++] = bearer[bi];
    }
    const char *ct = "\r\nContent-Type: application/json\r\nContent-Length: ";
    while (*ct && n < req_cap - 1) req[n++] = *ct++;
    putn_into(req, &n, req_cap - 1, body_len);
    const char *tail = "\r\nConnection: close\r\n\r\n";
    while (*tail && n < req_cap - 1) req[n++] = *tail++;
    for (unsigned int i = 0; i < body_len && n < req_cap - 1; i++) req[n++] = body[i];

    /* Same heap-backed raw-response sizing http_get uses: big enough for
       the caller's response_maxlen plus real header overhead, not a fixed
       8192 that used to cap every POST reply (a longer chat answer or a
       bigger generated page) regardless of what the caller actually
       asked for. */
    u32 raw_cap = response_maxlen + 2048;
    char *raw = kmalloc(raw_cap);
    if (!raw) { kfree(req); return -1; }
    int r = http_body_only(ip, port, req, n, response_out, response_maxlen, raw, raw_cap, reply_timeout_ticks);
    kfree(req);
    kfree(raw);
    return r;
}
