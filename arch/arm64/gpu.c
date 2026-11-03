/* 13.2.0: the Pi 4's V3D GPU, brought up by hand. The first step to graphics acceleration: `gpu` powers it and reads back
   what it is; nothing draws on it yet, and boot never touches it.

   The sequence is Linux's, from the Pi 4's own device tree (bcm2711-rpi.dtsi: the PM block's "v3d" clock is firmware
   clock 5) and drivers/pmdomain/bcm/bcm2835-power.c (bcm2835_asb_power_on for BCM2835_POWER_DOMAIN_GRAFX_V3D; on a 2711
   the firmware keeps the GRAFX power switch on, so there is no power switch step):
     1. the firmware turns clock 5 on at its top rate (mailbox tags 0x00038001 and 0x00038002, the rate from 0x00030004),
        after one off and on so the reset sees clocks, as Linux's 1 us pulse does;
     2. the bridge at 0x7ec11000 ("rpivid_asb") must say "brdg" (0x62726467) in its ID register, or this is not a 2711;
     3. PM_GRAFX gets V3DRSTN (bit 6), written with the PM password 0x5a000000;
     4. the bridge's master then slave port leave stop (bit 0 cleared) and must drop ACK (bit 1) within a millisecond;
     5. only then are the hub's and core 0's ident registers read: version = IDENT1 bits 3:0 * 10 + bits 7:4 (v3d_drv.c),
        42 on a 2711.
   A refused tag or a silent bridge stops the sequence before any V3D register is read, since a read of an unclocked
   block can hang the bus. The registers are v3d_regs.h's. QEMU's virt has no V3D and says so. */
#ifndef GPU_HOST_TEST
#include "kmain.h"
#define GR(a) REG(a)
#define GW(a, v) (REG(a) = (v))
__attribute__((unused)) static unsigned long gpu_now(void) { unsigned long c; __asm__ volatile ("isb\n mrs %0, cntpct_el0" : "=r"(c)); return c; }
__attribute__((unused)) static unsigned long gpu_hz(void) { unsigned long f; __asm__ volatile ("mrs %0, cntfrq_el0" : "=r"(f)); return f ? f : 54000000; }
#endif

#define GPU_TAG_MAX_RATE 0x00030004u
#define GPU_TAG_CLK_STATE 0x00038001u
#define GPU_TAG_CLK_RATE 0x00038002u
#define GPU_CLK_V3D 5u
#define GPU_PM_GRAFX 0xFE10010CUL
#define GPU_PM_PASSWORD 0x5a000000u
#define GPU_PM_V3DRSTN (1u << 6)
#define GPU_ASB 0xFEC11000UL
#define GPU_ASB_S 0x08
#define GPU_ASB_M 0x0c
#define GPU_ASB_ID 0x20
#define GPU_BRDG 0x62726467u
#define GPU_HUB 0xFEC00000UL
#define GPU_CORE0 0xFEC04000UL

int gpu_up;   /* 1 once the ident read back as a V3D */
unsigned gpu_ver(unsigned ident1) { return (ident1 & 15) * 10 + ((ident1 >> 4) & 15); }

#if !defined(PI_BUILD) && !defined(GPU_HOST_TEST)
int gpu_bringup(void (*say)(const char *)) { say("gpu: no V3D on this machine\n"); return 0; }
#else
/* One clock tag for clock 5, value then a zero (SET_CLOCK_RATE's skip-turbo). 1 with *out the answer, 0 refused: the call
   or the tag's own response bit (bit 31 of the size word, as power.h checks it). */
static int gpu_clock(unsigned tag, unsigned val, unsigned *out) {
    unsigned m[] = { 9 * 4, 0, tag, 12, 0, GPU_CLK_V3D, val, 0, 0 };
    for (unsigned i = 0; i < 9; i++) mbox[i] = m[i];
    if (!mbox_call() || !(mbox[4] & 0x80000000u) || mbox[5] != GPU_CLK_V3D) return 0;
    *out = mbox[6];
    return 1;
}

static int gpu_asb_on(unsigned long reg) {   /* bcm2835_asb_control(.., true): leave stop, wait for ACK to drop */
    GW(reg, GPU_PM_PASSWORD | (GR(reg) & ~1u));
    for (unsigned long t0 = gpu_now(); GR(reg) & 2u;) if (gpu_now() - t0 > gpu_hz() / 1000) return 0;
    return 1;
}

static char gl[96]; static unsigned gn;
static void ga(const char *s) { while (*s && gn < sizeof gl - 1) gl[gn++] = *s++; gl[gn] = 0; }
static void gd(unsigned v) { char t[12]; int i = 0; do t[i++] = (char)('0' + v % 10); while (v /= 10); while (i) { char c[2] = { t[--i], 0 }; ga(c); } }
static void gx(unsigned v) { ga("0x"); for (int s = 28; s >= 0; s -= 4) { char c[2] = { "0123456789abcdef"[(v >> s) & 15], 0 }; ga(c); } }

/* The bring-up; each step's line goes to say. Returns 1 when the GPU answered. */
int gpu_bringup(void (*say)(const char *)) {
    if (gpu_up) { say("gpu: V3D already up\n"); return 1; }
    unsigned max = 0, got = 0;
    if (!gpu_clock(GPU_TAG_MAX_RATE, 0, &max) || !max) { say("gpu: the firmware refused the V3D clock\n"); return 0; }
    if (!gpu_clock(GPU_TAG_CLK_STATE, 1, &got) || !gpu_clock(GPU_TAG_CLK_STATE, 0, &got) ||
        !gpu_clock(GPU_TAG_CLK_STATE, 1, &got) || !gpu_clock(GPU_TAG_CLK_RATE, max, &got)) { say("gpu: the firmware refused the V3D clock\n"); return 0; }
    gn = 0; ga("gpu: V3D clock "); gd(got / 1000000); ga(" MHz\n"); say(gl);
    unsigned id = GR(GPU_ASB + GPU_ASB_ID);
    if (id != GPU_BRDG) { gn = 0; ga("gpu: no V3D bridge (id "); gx(id); ga(")\n"); say(gl); return 0; }
    GW(GPU_PM_GRAFX, GPU_PM_PASSWORD | (GR(GPU_PM_GRAFX) & 0x00ffffffu) | GPU_PM_V3DRSTN);
    if (!gpu_asb_on(GPU_ASB + GPU_ASB_M) || !gpu_asb_on(GPU_ASB + GPU_ASB_S)) { say("gpu: the V3D bridge did not answer\n"); return 0; }
    unsigned i1 = GR(GPU_HUB + 0x0c), i2 = GR(GPU_HUB + 0x10), c0 = GR(GPU_CORE0 + 0x00), v = gpu_ver(i1);
    if (v < 40 || v > 79) { gn = 0; ga("gpu: V3D did not identify (hub ident1 "); gx(i1); ga(")\n"); say(gl); return 0; }
    gn = 0; ga("gpu: V3D "); gd(v / 10); ga("."); gd(v % 10); ga(", "); gd((i1 >> 8) & 15); ga((i1 >> 8 & 15) == 1 ? " core, " : " cores, ");
    ga(i2 & (1u << 8) ? "MMU, " : "no MMU, "); ga("L3 cache "); gd(i2 & 255); ga(" KB, core ident "); gx(c0); ga("\n"); say(gl);
    gpu_up = 1;
    say("gpu: ready\n");
    return 1;
}
#endif

#ifndef GPU_HOST_TEST
extern void (*cmd_out)(const char *s);
int gpu_command(const char *q, unsigned n) {   /* cmd.c: `gpu` */
    if (n != 3 || q[0] != 'g' || q[1] != 'p' || q[2] != 'u') return 0;
    gpu_bringup(cmd_out);
    return 1;
}
#endif
