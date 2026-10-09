#!/usr/bin/env python3
"""Host test of the ARM clock: verified HTTPS only, bounded Date parsing and no rollback. No QEMU or live network."""
import pathlib, subprocess, tempfile

root = pathlib.Path(__file__).resolve().parents[2]
source = r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
#define BUILD_UTC 1791504000UL /* 2026-10-09 00:00 UTC */
#include "arch/arm64/clock.c"
static unsigned tick = 100;
static int fail_tls, status = 200;
static char response[4096];
unsigned ticks(void) { return tick; }
void kputs(const char *s) { (void)s; }
unsigned net_get_gateway(void) { return 1; }
int http_status_of(const char *raw, unsigned total) { (void)raw; (void)total; return status; }
int http_body_start(const char *raw, unsigned total) {
    for (unsigned i = 0; i + 3 < total; i++) if (!memcmp(raw + i, "\r\n\r\n", 4)) return (int)i + 4;
    return -1;
}
int https_fetch(const char *host, unsigned short port, const void *req, unsigned len, char *out, unsigned max) {
    assert(!strcmp(host, "www.google.com") && port == 443);
    assert(len > 5 && !memcmp(req, "HEAD ", 5));
    if (fail_tls) return -1;
    unsigned n = (unsigned)strlen(response); assert(n <= max); memcpy(out, response, n); return (int)n;
}
static void sync_date(const char *date) {
    snprintf(response, sizeof response, "HTTP/1.0 200 OK\r\nDate: %s\r\n\r\n", date);
    net_clock_sync();
}
int main(void) {
    assert(net_clock_floor() == BUILD_UTC && net_clock_utc() == 0);
    const char *valid = "Fri, 09 Oct 2026 12:34:56 GMT";
    assert(date_utc(valid, 29) == 1791549296UL);
    for (unsigned n = 0; n < 29; n++) assert(date_utc(valid, n) == 0);
    assert(!date_utc(valid, 30));
    const char *bad[] = {"Bad, 09 Oct 2026 12:34:56 GMT", "Fri, 00 Oct 2026 12:34:56 GMT", "Fri, 32 Oct 2026 12:34:56 GMT",
        "Fri, 09 Xxx 2026 12:34:56 GMT", "Fri, 09 Oct 2026 24:34:56 GMT", "Fri, 09 Oct 2026 12:60:56 GMT",
        "Fri, 09 Oct 2026 12:34:60 GMT", "Fri, 09 Oct 20x6 12:34:56 GMT", "Fri, 09 Oct 2026 12:34:56 PST",
        "Fri, 29 Feb 2025 12:34:56 GMT", "Fri, 31 Apr 2026 12:34:56 GMT"};
    for (unsigned i = 0; i < sizeof bad / sizeof *bad; i++) assert(!date_utc(bad[i], 29));
    assert(date_utc("Thu, 29 Feb 2024 00:00:00 GMT", 29) == 1709164800UL);
    sync_date("Thu, 08 Oct 2026 23:59:59 GMT"); assert(!net_clock_utc());
    fail_tls = 1; sync_date(valid); assert(!net_clock_utc()); fail_tls = 0;
    status = 500; sync_date(valid); assert(!net_clock_utc()); status = 200;
    strcpy(response, "HTTP/1.0 200 OK\r\n\r\nDate: Fri, 09 Oct 2026 12:34:56 GMT\r\n");
    net_clock_sync(); assert(!net_clock_utc());
    strcpy(response, "HTTP/1.0 200 OK\r\nDate: Fri, 09 Oct 2026 12:34:56 GMT\r\nDate: Fri, 09 Oct 2026 13:34:56 GMT\r\n\r\n");
    net_clock_sync(); assert(!net_clock_utc());
    sync_date(valid); assert(net_clock_utc() == 1791549296UL);
    tick += 100; assert(net_clock_utc() == 1791549297UL);
    sync_date(valid); assert(net_clock_utc() == 1791549297UL); /* reject rollback even after a good sync */
    sync_date("Fri, 09 Oct 2026 13:34:56 GMT"); assert(net_clock_utc() == 1791552896UL);
    fail_tls = 1; sync_date("Fri, 09 Oct 2026 14:34:56 GMT"); assert(net_clock_utc() == 1791552896UL);
    puts("PASS: HTTPS-only clock, strict bounded dates, build floor, monotonic resync and failure preservation");
}
'''
with tempfile.TemporaryDirectory(prefix="jt-clock-") as tmp:
    c = pathlib.Path(tmp, "test.c"); c.write_text(source)
    exe = str(pathlib.Path(tmp, "clock"))
    subprocess.run(["clang", "-std=c11", "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined",
                    "-I", str(root), str(c), "-o", exe], check=True)
    subprocess.run([exe], check=True, timeout=15)
