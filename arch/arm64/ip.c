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
