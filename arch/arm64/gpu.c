/* 13.2.0: the Pi 4's V3D GPU, brought up by hand. The first step to graphics acceleration: `gpu` powers it and reads back
   what it is; nothing draws on it yet, and boot never touches it.

   The sequence is Linux's, from the Pi 4's own device tree (bcm2711-rpi.dtsi: the PM block's "v3d" clock is firmware
   clock 5) and drivers/pmdomain/bcm/bcm2835-power.c (bcm2835_asb_power_on for BCM2835_POWER_DOMAIN_GRAFX_V3D; on a 2711
   the firmware keeps the GRAFX power switch on, so there is no power switch step):
     1. the bridge at 0x7ec11000 ("rpivid_asb") must say "brdg" (0x62726467) in its ID register, or this is not a 2711;
     2. the firmware turns clock 5 on, then off (mailbox tag 0x00038001; its top rate asked first with 0x00030004), so the
        reset sees clocks, as Linux's 1 us pulse does;
     3. with the clock off, PM_GRAFX gets V3DRSTN (bit 6), written with the PM password 0x5a000000;
     4. the clock goes on again at its top rate (0x00038001, 0x00038002);
     5. the bridge's master then slave port leave stop (bit 0 cleared) and must drop ACK (bit 1) within a millisecond;
     6. only then are the hub's and core 0's ident registers read: version = IDENT1 bits 3:0 * 10 + bits 7:4 (v3d_drv.c),
        42 on a 2711.
   A refused tag or a silent bridge stops the sequence before any V3D register is read, since a read of an unclocked
   block can hang the bus. The registers are v3d_regs.h's. QEMU's virt has no V3D and says so. */
#ifndef GPU_HOST_TEST
#include "kmain.h"
#define GR(a) REG(a)
#define GW(a, v) (REG(a) = (v))
#define GPU_FLUSH() do { if (fb) fb_flush(0, 0, (int)fb_w, (int)fb_h); } while (0)
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
/* core 0's registers for one render job (v3d_regs.h) */
#define GPU_MISCCFG 0x018
#define GPU_SLCACTL 0x024
#define GPU_L2TCACTL 0x030
#define GPU_L2TFLSTA 0x034
#define GPU_L2TFLEND 0x038
#define GPU_CT1CS 0x104
#define GPU_CT1CA 0x114
#define GPU_RFC 0x138
#define GPU_CT1QBA 0x164
#define GPU_CT1QEA 0x16c
/* the V3D sits on the 2711's SCB bus, which has no dma-ranges (bcm2711.dtsi): it sees ARM addresses as they are, not the
   0xC0000000 alias the legacy DMA engines need (audio.c) */
#define GPU_BUS(p) ((unsigned)(unsigned long)(p))

int gpu_up;   /* 1 once the ident read back as a V3D */
unsigned gpu_ver(unsigned ident1) { return (ident1 & 15) * 10 + ((ident1 >> 4) & 15); }

#if !defined(PI_BUILD) && !defined(GPU_HOST_TEST)
int gpu_bringup(void (*say)(const char *)) { say("gpu: no V3D on this machine\n"); return 0; }
int gpu_clear(void (*say)(const char *), unsigned rgba) { (void)rgba; say("gpu: no V3D on this machine\n"); return 0; }
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

/* The bring-up; each step's line goes to say, and to the screen at once: if a step hangs the bus on a board, the last line
   shown names it. Returns 1 when the GPU answered. */
static void (*gsay)(const char *);
static void show(const char *s) { gsay(s); GPU_FLUSH(); }
static void showl(void) { ga("\n"); show(gl); gn = 0; }
int gpu_bringup(void (*say)(const char *)) {
    gsay = say; gn = 0;
    if (gpu_up) { show("gpu: V3D already up\n"); return 1; }
    unsigned id = GR(GPU_ASB + GPU_ASB_ID);   /* Linux reads it at probe, before any clock */
    if (id != GPU_BRDG) { ga("gpu: no V3D bridge (id "); gx(id); ga(")"); showl(); return 0; }
    unsigned max = 0, got = 0;
    if (!gpu_clock(GPU_TAG_MAX_RATE, 0, &max) || !max || !gpu_clock(GPU_TAG_CLK_STATE, 1, &got) || !gpu_clock(GPU_TAG_CLK_STATE, 0, &got)) {
        show("gpu: the firmware refused the V3D clock\n"); return 0;
    }
    ga("gpu: lifting the V3D reset (PM_GRAFX "); gx(GR(GPU_PM_GRAFX)); ga(")"); showl();
    GW(GPU_PM_GRAFX, GPU_PM_PASSWORD | (GR(GPU_PM_GRAFX) & 0x00ffffffu) | GPU_PM_V3DRSTN);   /* with the clock off, as Linux */
    if (!gpu_clock(GPU_TAG_CLK_STATE, 1, &got) || !gpu_clock(GPU_TAG_CLK_RATE, max, &got)) { show("gpu: the firmware refused the V3D clock\n"); return 0; }
    ga("gpu: V3D clock "); gd(got / 1000000); ga(" MHz, opening the bridge"); showl();
    if (!gpu_asb_on(GPU_ASB + GPU_ASB_M) || !gpu_asb_on(GPU_ASB + GPU_ASB_S)) { show("gpu: the V3D bridge did not answer\n"); return 0; }
    show("gpu: reading the V3D ident\n");
    unsigned i1 = GR(GPU_HUB + 0x0c), i2 = GR(GPU_HUB + 0x10), c0 = GR(GPU_CORE0 + 0x00), v = gpu_ver(i1);
    if (v < 40 || v > 79) { ga("gpu: V3D did not identify (hub ident1 "); gx(i1); ga(")"); showl(); return 0; }
    ga("gpu: V3D "); gd(v / 10); ga("."); gd(v % 10); ga(", "); gd((i1 >> 8) & 15); ga((i1 >> 8 & 15) == 1 ? " core, " : " cores, ");
    ga(i2 & (1u << 8) ? "MMU, " : "no MMU, "); ga("L3 cache "); gd(i2 & 255); ga(" KB, core ident "); gx(c0); showl();
    GW(GPU_CORE0 + GPU_MISCCFG, 1);   /* v3d_init_core: OVRTMUOUT, and the L2T flush range covers all memory */
    GW(GPU_CORE0 + GPU_L2TFLSTA, 0);
    GW(GPU_CORE0 + GPU_L2TFLEND, ~0u);
    gpu_up = 1;
    show("gpu: ready\n");
    return 1;
}

/* 13.4.1: the first job the V3D runs, a render control list (RCL) that clears one 64x64 tile to a colour and stores it to
   memory, which the CPU then reads back. No binning, no shaders, MMU off. The packets are V3D 4.2's (Mesa's
   v3d_packet.xml, MIT), in the order Mesa's v3dx_rcl.c emits them: the common config first, the Z/stencil clear values
   last; a first tile that stores nothing and clears the tile buffer (Mesa's own workaround), then the tile again, stored
   as raster RGBA8. The caches go as Linux's v3d_gem.c does them: L2T flushed and the slices invalidated before, the TMU
   write cache and L2T cleaned after. Done is the render frame count moving (CT1 queues one frame). */
static unsigned char gpu_cl[128] __attribute__((aligned(256)));
static unsigned gpu_px[64 * 64] __attribute__((aligned(4096)));
static unsigned gcn;
static void cb(unsigned v) { gpu_cl[gcn++] = (unsigned char)v; }
static void cw(unsigned long long v, int n) { while (n--) { cb((unsigned)v & 255); v >>= 8; } }
static int gpu_wait(unsigned long reg, unsigned bit) {
    for (unsigned long t0 = gpu_now(); GR(reg) & bit;) if (gpu_now() - t0 > gpu_hz() / 100) return 0;
    return 1;
}
int gpu_clear(void (*say)(const char *), unsigned rgba) {
    gsay = say; gn = 0;
    if (!gpu_up) { show("gpu: not up, run gpu first\n"); return 0; }
    gcn = 0;
    cb(121); cw((64ull << 8) | (64ull << 24) | (1ull << 46) | (1ull << 47), 8);   /* COMMON: 64x64, 1 RT, early Z off, depth_24 */
    cb(121); cw(3 | ((unsigned long long)rgba << 8), 8);                          /* CLEAR_COLORS_PART1 for RT0 */
    cb(121); cw(1 | (2u << 6), 8);                                                 /* COLOR: RT0 32 bpp, internal type 8 */
    cb(121); cw(2 | (0x3f800000ull << 16), 8);                                     /* ZS_CLEAR_VALUES: Z 1.0, last */
    cb(122); cw((1ull << 16) | (1ull << 24) | (1ull << 32) | (1ull << 44), 8);    /* SUPERTILE_CFG: one tile, one supertile */
    cb(124); cw(0, 3); cb(26);                                                     /* TILE_COORDINATES 0,0; END_OF_LOADS */
    cb(29); cw(8, 12);                                                             /* STORE_TILE_BUFFER_GENERAL: none */
    cb(25); cb(3); cb(27);                                                         /* CLEAR_TILE_BUFFERS RTs and Z/S; END_OF_TILE */
    cb(124); cw(0, 3); cb(26);
    cb(29); cw((27ull << 12) | (256ull << 28) | (64ull << 48), 8); cw(GPU_BUS(gpu_px), 4);   /* RT0, raster rgba8, 256 B rows */
    cb(27); cb(19); cb(13);                                                        /* END_OF_TILE, FLUSH_VCD_CACHE, END_OF_RENDERING */
    for (unsigned i = 0; i < 64 * 64; i++) gpu_px[i] = 0;
    dcache_clean(gpu_cl, sizeof gpu_cl);
    dcache_clean(gpu_px, sizeof gpu_px);
    GW(GPU_CORE0 + GPU_L2TCACTL, 1);   /* L2TFLS, mode flush */
    if (!gpu_wait(GPU_CORE0 + GPU_L2TCACTL, 1)) { show("gpu: the L2T flush never finished\n"); return 0; }
    GW(GPU_CORE0 + GPU_SLCACTL, 0x0f0f0f0fu);
    unsigned f0 = GR(GPU_CORE0 + GPU_RFC);
    GW(GPU_CORE0 + GPU_CT1QBA, GPU_BUS(gpu_cl));
    GW(GPU_CORE0 + GPU_CT1QEA, GPU_BUS(gpu_cl) + gcn);   /* this write starts the job */
    for (unsigned long t0 = gpu_now(); GR(GPU_CORE0 + GPU_RFC) == f0;)
        if (gpu_now() - t0 > gpu_hz() / 10) {
            ga("gpu: the clear job hung (CT1CS "); gx(GR(GPU_CORE0 + GPU_CT1CS)); ga(", at "); gx(GR(GPU_CORE0 + GPU_CT1CA)); ga(")"); showl();
            return 0;
        }
    GW(GPU_CORE0 + GPU_L2TCACTL, 1u << 8);   /* TMUWCF, then L2TFLS in clean mode */
    if (!gpu_wait(GPU_CORE0 + GPU_L2TCACTL, 1u << 8)) { show("gpu: the TMU write cache never cleaned\n"); return 0; }
    GW(GPU_CORE0 + GPU_L2TCACTL, 1 | (2u << 1));
    if (!gpu_wait(GPU_CORE0 + GPU_L2TCACTL, 1)) { show("gpu: the L2T clean never finished\n"); return 0; }
    dcache_clean(gpu_px, sizeof gpu_px);   /* dc civac: drops any stale line before the CPU reads what the GPU wrote */
    unsigned bad = 0;
    for (unsigned i = 0; i < 64 * 64; i++) bad += gpu_px[i] != rgba;
    if (bad) { ga("gpu: the V3D ran but "); gd(bad); ga(" of 4096 pixels are wrong (first "); gx(gpu_px[0]); ga(")"); showl(); return 0; }
    ga("gpu: the V3D cleared 64x64 pixels to "); gx(rgba); showl();
    return 1;
}
#endif

#ifndef GPU_HOST_TEST
extern void (*cmd_out)(const char *s);
int gpu_command(const char *q, unsigned n) {   /* cmd.c: `gpu`, `gpu clear` */
    if (n < 3 || q[0] != 'g' || q[1] != 'p' || q[2] != 'u') return 0;
    if (n == 3) { gpu_bringup(cmd_out); return 1; }
    static const char c[] = " clear";
    if (n != 9) return 0;
    for (unsigned i = 0; i < 6; i++) if (q[3 + i] != c[i]) return 0;
    gpu_clear(cmd_out, 0xff2c50b5u);   /* terracotta #b5502c as RGBA8 bytes */
    return 1;
}
#endif
