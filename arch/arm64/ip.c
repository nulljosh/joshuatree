/* The shared IP stack on ARM64. drivers/net.c (ARP, IPv4, UDP, DNS, TCP, DHCP) and drivers/http.c are the same files
   the i386 kernel builds; here they sit on drivers/nic.h, which main.c answers with virtio-net today and the Pi's own
   card later. This file is the rest of what they need (a 100 Hz clock, a console) and the boot-time proof: lease an
   address, then POST to the host and read the reply. Every line it prints stays under 53 columns for the Pi screen. */
#include "../../drivers/nic.h"
#include "../../drivers/net.h"
#include "../../drivers/http.h"

#ifndef NETPORT
#define NETPORT 0   /* the host port for the boot POST; 0 skips it (make -C arch/arm64 NETPORT=8080 turns it on) */
#endif

void kputs(const char *s);   /* main.c */
void kdec(unsigned v);

unsigned int ticks(void) {   /* the generic timer, read as 100 ticks a second like the i386 PIT */
    unsigned long f, t;
    __asm__ volatile ("mrs %0, cntfrq_el0\n mrs %1, cntpct_el0" : "=r"(f), "=r"(t));
    if (!f) f = 54000000;
    return (unsigned int)(t / (f / 100));
}
void serial_puts(const char *s) { kputs(s); }

static void put_ip(unsigned ip) {
    for (int s = 24; s >= 0; s -= 8) { kdec((ip >> s) & 255); if (s) kputs("."); }
}

void net_stack_demo(void) {
    if (!net_init(0x0A00020Fu)) return;   /* no card: say nothing. 10.0.2.15 is only the no-DHCP fallback */
    if (net_get_gateway()) {
        kputs("net dhcp "); put_ip(net_get_ip());
        kputs(" gw "); put_ip(net_get_gateway()); kputs("\n");
    } else kputs("net dhcp FAIL\n");
    if (!NETPORT) return;
    static const char body[] = "{\"from\":\"joshua tree arm64\"}";
    static char reply[512];
    char host[16]; unsigned gw = net_get_gateway() ? net_get_gateway() : 0x0A000202u, n = 0;
    for (int s = 24; s >= 0; s -= 8) {   /* the gateway as a dotted quad: http.c takes an IP literal and skips DNS */
        unsigned v = (gw >> s) & 255;
        if (v >= 100) host[n++] = (char)('0' + v / 100);
        if (v >= 10) host[n++] = (char)('0' + v / 10 % 10);
        host[n++] = (char)('0' + v % 10);
        if (s) host[n++] = '.';
    }
    host[n] = 0;
    int got = http_post_timeout(host, "/jt", NETPORT, body, sizeof body - 1, reply, sizeof reply, 500);   /* ~5 s */
    if (got < 0 || !http_last_status()) {
        kputs("net http FAIL "); kputs(net_error_name(net_last_error())); kputs("\n");
        return;
    }
    kputs("net http "); kdec((unsigned)http_last_status());
    kputs(" "); kdec((unsigned)got); kputs(" bytes\n");
}

/* ---- The clock. The Pi has no battery clock, so the time comes from the network once there is one: a plain HTTP HEAD
   request (port 80, no TLS needed) and the Date: line of the reply, which every web server sends in UTC. Seconds are good
   enough for a menu-bar clock; ticks() carries it forward from there. A later NTP version can be exact. */
static unsigned long clock_utc0; static unsigned clock_tick0; static int clock_ok;
static int month_of(const char *m) {
    static const char *const n[12] = {"Jan","Feb","Mar","Apr","May","Jun","Jul","Aug","Sep","Oct","Nov","Dec"};
    for (int i = 0; i < 12; i++) if (m[0] == n[i][0] && m[1] == n[i][1] && m[2] == n[i][2]) return i + 1;
    return 0;
}
static unsigned long days_from_civil(long y, unsigned m, unsigned d) {   /* days since 1970-01-01, proleptic Gregorian */
    y -= m <= 2; long era = (y >= 0 ? y : y - 399) / 400; unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1, doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return (unsigned long)(era * 146097 + (long)doe - 719468);
}
static unsigned num(const char *p, int n) { unsigned v = 0; for (int i = 0; i < n && p[i] >= '0' && p[i] <= '9'; i++) v = v * 10 + (unsigned)(p[i] - '0'); return v; }
void net_clock_sync(void) {
    unsigned ip = 0;
    if (!net_get_gateway()) return;
    for (int t = 0; t < 4 && !ip; t++)   /* the first lookup can be lost to ARP or a busy router: try the DNS server, then the gateway, twice */
        if (!dns_resolve("www.google.com", t % 2 == 0 && net_get_dns() ? net_get_dns() : net_get_gateway(), &ip)) ip = 0;
    if (!ip) { kputs("Internet: could not look up a web address (DNS)\n"); return; }
    static const char req[] = "HEAD / HTTP/1.0\r\nHost: www.google.com\r\nConnection: close\r\n\r\n";
    static char rep[1024];
    int got = tcp_get_timeout(ip, 80, req, sizeof req - 1, rep, sizeof rep - 1, 500);
    if (got <= 0) { kputs("Internet: the time server did not answer\n"); return; }
    rep[got] = 0;
    for (int i = 0; i + 6 < got; i++) {
        if ((rep[i] == 'D' || rep[i] == 'd') && rep[i + 1] == 'a' && rep[i + 2] == 't' && rep[i + 3] == 'e' && rep[i + 4] == ':') {
            const char *d = rep + i + 5; while (*d == ' ') d++;
            while (*d && *d != ',') d++;   /* weekday */
            if (*d == ',') d++; while (*d == ' ') d++;
            unsigned day = num(d, 2), mon = month_of(d + 3), year = num(d + 7, 4), hh = num(d + 12, 2), mm = num(d + 15, 2), ss = num(d + 18, 2);
            if (!mon || year < 2024) break;
            clock_utc0 = days_from_civil(year, mon, day) * 86400UL + hh * 3600UL + mm * 60UL + ss;
            clock_tick0 = ticks(); clock_ok = 1;
            kputs("Internet: online, clock set\n");
            return;
        }
    }
    kputs("Internet: the reply had no time in it\n");
}
/* Seconds since 1970 in UTC, or 0 while the clock is unset. */
unsigned long net_clock_utc(void) { return clock_ok ? clock_utc0 + (unsigned long)((unsigned)(ticks() - clock_tick0) / 100) : 0; }
