/* The Pi has no battery clock. Bootstrap certificate checks at the build time, then accept time only from
   a verified HTTPS server. Never move behind the build or an already accepted clock. A stale image may need
   rebuilding when the time server's certificate was issued after the build. No plain HTTP fallback.
   shortcut: build time cannot detect certificates expired since the build; replace with persistent or signed fresh time. */
#include "../../drivers/net.h"
#include "../../drivers/http.h"
#include "tls.h"
#ifndef BUILD_UTC
#error BUILD_UTC must be supplied by the Makefile
#endif
unsigned ticks(void);
void kputs(const char *s);
static unsigned long clock_utc0;
static unsigned clock_tick0;
unsigned long net_clock_floor(void) { return BUILD_UTC; }
unsigned long net_clock_utc(void) {
    return clock_utc0 ? clock_utc0 + (unsigned long)((unsigned)(ticks() - clock_tick0) / 100) : 0;
}
static unsigned number(const char *p, unsigned n) {
    unsigned v = 0;
    for (unsigned i = 0; i < n; i++) {
        if (p[i] < '0' || p[i] > '9') return 99999;
        v = v * 10 + (unsigned)(p[i] - '0');
    }
    return v;
}
static unsigned long date_utc(const char *d, unsigned n) {
    /* IMF-fixdate only, exactly "Fri, 09 Oct 2026 12:34:56 GMT". Bound every read before parsing. */
    if (n != 29 || d[3] != ',' || d[4] != ' ' || d[7] != ' ' || d[11] != ' ' || d[16] != ' '
        || d[19] != ':' || d[22] != ':' || d[25] != ' ' || d[26] != 'G' || d[27] != 'M' || d[28] != 'T') return 0;
    static const char *const weekdays[] = {"Sun","Mon","Tue","Wed","Thu","Fri","Sat"};
    unsigned weekday = 0;
    for (unsigned i = 0; i < 7; i++)
        if (d[0] == weekdays[i][0] && d[1] == weekdays[i][1] && d[2] == weekdays[i][2]) weekday = 1;
    if (!weekday) return 0;
    static const char *const months[] = {"Jan","Feb","Mar","Apr","May","Jun","Jul","Aug","Sep","Oct","Nov","Dec"};
    unsigned mon = 0;
    for (unsigned i = 0; i < 12; i++)
        if (d[8] == months[i][0] && d[9] == months[i][1] && d[10] == months[i][2]) mon = i + 1;
    unsigned day = number(d + 5, 2), year = number(d + 12, 4), h = number(d + 17, 2), m = number(d + 20, 2), s = number(d + 23, 2);
    if (!mon || year < 2024 || year > 9999 || h > 23 || m > 59 || s > 59) return 0;
    static const unsigned mdays[] = {31,28,31,30,31,30,31,31,30,31,30,31};
    unsigned leap = year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
    if (!day || day > mdays[mon - 1] + (mon == 2 && leap)) return 0;
    long y = (long)year - (mon <= 2), era = y / 400;
    unsigned yoe = (unsigned)(y - era * 400), doy = (153 * (mon > 2 ? mon - 3 : mon + 9) + 2) / 5 + day - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return (unsigned long)(era * 146097 + (long)doe - 719468) * 86400UL + h * 3600UL + m * 60UL + s;
}
void net_clock_sync(void) {
    if (!net_get_gateway()) return;
    static const char req[] = "HEAD / HTTP/1.0\r\nHost: www.google.com\r\nConnection: close\r\n\r\n";
    static char rep[4096];
    int got = https_fetch("www.google.com", 443, req, sizeof req - 1, rep, sizeof rep);
    if (got <= 0 || http_status_of(rep, (unsigned)got) < 200 || http_status_of(rep, (unsigned)got) >= 400) {
        kputs("Internet: trusted time unavailable\n"); return;
    }
    int end = http_body_start(rep, (unsigned)got);
    if (end < 0) { kputs("Internet: the reply had no trusted time\n"); return; }
    unsigned long utc = 0;
    for (unsigned i = 0; i + 2 < (unsigned)end;) {
        unsigned j = i;
        while (j + 1 < (unsigned)end && !(rep[j] == '\r' && rep[j + 1] == '\n')) j++;
        if (j + 1 >= (unsigned)end) break;
        if (j - i >= 5 && (rep[i] | 32) == 'd' && (rep[i + 1] | 32) == 'a'
            && (rep[i + 2] | 32) == 't' && (rep[i + 3] | 32) == 'e' && rep[i + 4] == ':') {
            if (utc) { utc = 0; break; }   /* conflicting duplicate headers are not a clock */
            unsigned k = i + 5; while (k < j && (rep[k] == ' ' || rep[k] == '\t')) k++;
            utc = date_utc(rep + k, j - k);
            if (!utc) break;
        }
        i = j + 2;
    }
    unsigned long floor = net_clock_utc(); if (floor < BUILD_UTC) floor = BUILD_UTC;
    if (!utc || utc < floor) { kputs("Internet: trusted time refused\n"); return; }
    clock_utc0 = utc; clock_tick0 = ticks();
    kputs("Internet: online\n");
}
