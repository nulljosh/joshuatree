/* 13.3.0: how hot the Pi's chip is. `temp` asks the firmware (mailbox tag 0x00030006, get temperature, id 0: thousandths
   of a degree C) and the highest it allows before it slows the chip (0x0003000a, get max temperature), and prints both:
   "temp: 52.1 C, the chip slows down at 85.0 C". A fan on pins 4 and 6 (5V and ground) runs whenever the Pi has power,
   so nothing here drives a fan pin: the official fan's control wire sits on GPIO 14, our UART. Virt has no sensor. */
#ifndef HEAT_HOST_TEST
#include "kmain.h"
#endif

static char hl[64]; static unsigned hn;
static void ha(const char *s) { while (*s && hn < sizeof hl - 1) hl[hn++] = *s++; hl[hn] = 0; }
static void hd(unsigned v) { char t[12]; int i = 0; do t[i++] = (char)('0' + v % 10); while (v /= 10); while (i) { char c[2] = { t[--i], 0 }; ha(c); } }
static void hc(unsigned mc) { hd(mc / 1000); ha("."); hd(mc % 1000 / 100); ha(" C"); }   /* thousandths, shown to a tenth */

#if defined(PI_BUILD) || defined(HEAT_HOST_TEST)
static int heat_tag(unsigned tag, unsigned *mc) {   /* 1 with *mc, 0 refused: the call or the tag's own response bit */
    unsigned m[] = { 8 * 4, 0, tag, 8, 0, 0, 0, 0 };
    for (unsigned i = 0; i < 8; i++) mbox[i] = m[i];
    if (!mbox_call() || !(mbox[4] & 0x80000000u) || !mbox[6]) return 0;
    *mc = mbox[6];
    return 1;
}
const char *heat_line(void) {
    unsigned now, max;
    hn = 0;
    if (!heat_tag(0x00030006u, &now)) { ha("temp: the firmware did not say\n"); return hl; }
    ha("temp: "); hc(now);
    if (heat_tag(0x0003000au, &max)) { ha(", the chip slows down at "); hc(max); }
    ha("\n");
    return hl;
}
#else
const char *heat_line(void) { (void)hc; return "temp: no sensor on this machine\n"; }
#endif

#ifndef HEAT_HOST_TEST
extern void (*cmd_out)(const char *s);
int heat_command(const char *q, unsigned n) {   /* cmd.c: `temp` */
    if (n != 4 || q[0] != 't' || q[1] != 'e' || q[2] != 'm' || q[3] != 'p') return 0;
    cmd_out(heat_line());
    return 1;
}
#endif
