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
static int boot_on, win_have = 1, boot_seen; static unsigned long win_nb = 0, win_na = ~0UL;
void tls_time_bootstrap(int on) { boot_on = on; }
int tls_time_window(unsigned long *nb, unsigned long *na) { *nb = win_nb; *na = win_na; return win_have; }
int https_fetch(const char *host, unsigned short port, const void *req, unsigned len, char *out, unsigned max) {
    assert(!strcmp(host, "www.google.com") && port == 443);
    boot_seen = boot_on;   /* 13.9.3: the fetch runs with the chain checked without the time only while the clock is unset */
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
    /* 13.9.3: a card older than the time server's certificate. The chain is checked without the time, so the Date
       counts only inside its validity window, and an unchecked (resumed) chain sets nothing. */
    win_nb = 1791549297UL; sync_date(valid); assert(boot_seen && !boot_on && !net_clock_utc()); win_nb = 0;
    win_na = 1791549295UL; sync_date(valid); assert(!net_clock_utc()); win_na = ~0UL;
    win_have = 0; sync_date(valid); assert(!net_clock_utc()); win_have = 1;
    sync_date(valid); assert(net_clock_utc() == 1791549296UL);
    sync_date("Fri, 09 Oct 2026 12:44:56 GMT"); assert(!boot_seen && !boot_on);   /* once set, normal checks at the clock */
    clock_utc0 = 1791549296UL;
    tick += 100; assert(net_clock_utc() == 1791549297UL);
    sync_date(valid); assert(net_clock_utc() == 1791549297UL); /* reject rollback even after a good sync */
    sync_date("Fri, 09 Oct 2026 13:34:56 GMT"); assert(net_clock_utc() == 1791552896UL);
    fail_tls = 1; sync_date("Fri, 09 Oct 2026 14:34:56 GMT"); assert(net_clock_utc() == 1791552896UL);
    tick = 0xfffffff0U; unsigned long before_wrap = net_clock_utc();
    tick += 200; assert(net_clock_utc() == before_wrap + 2);
    puts("PASS: HTTPS-only clock, strict bounded dates, build floor, monotonic resync and failure preservation");
}
'''
with tempfile.TemporaryDirectory(prefix="jt-clock-") as tmp:
    c = pathlib.Path(tmp, "test.c"); c.write_text(source)
    exe = str(pathlib.Path(tmp, "clock"))
    subprocess.run(["clang", "-std=c11", "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined",
                    "-I", str(root), str(c), "-o", exe], check=True)
    subprocess.run([exe], check=True, timeout=15)
