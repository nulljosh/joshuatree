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
int gpu_fill(void (*say)(const char *), unsigned *scr, unsigned pitch, int x, int y, int w, int h, unsigned px) { (void)scr; (void)pitch; (void)x; (void)y; (void)w; (void)h; (void)px; say("gpu: no V3D on this machine\n"); return 0; }
int gpu_copy(void (*say)(const char *), unsigned *src, unsigned spitch, unsigned *dst, unsigned dpitch, int w, int h) { (void)src; (void)spitch; (void)dst; (void)dpitch; (void)w; (void)h; say("gpu: no V3D on this machine\n"); return 0; }
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

/* 13.4.1: the first job the V3D runs, a render control list (RCL) that clears tiles to a colour and stores them to memory.
   No binning, no shaders, MMU off. The packets are V3D 4.2's (Mesa's v3d_packet.xml, MIT), in the order Mesa's v3dx_rcl.c
   emits them: the common config first, the Z/stencil clear values last; a first tile that stores nothing and clears the
   tile buffer (Mesa's own workaround), then each real tile stored as raster RGBA8. The caches go as Linux's v3d_gem.c does
   them: L2T flushed and the slices invalidated before, the TMU write cache and L2T cleaned after. Done is the render frame
   count moving (CT1 queues one frame).
   13.20.0 (v14, slice 1): the same job over any rectangle of 64x64 tiles. The store's address is the rectangle's top left
   and its stride the row length in bytes; the V3D adds each tile's place from its coordinates and clips the last row and
   column to the frame. After a stored tile, a CLEAR_TILE_BUFFERS sets the buffer back to the clear colour for the next.
   `gpu fill` points it at the screen's back buffer, so the V3D paints pixels you can see. */
#define GPU_MAX 2048   /* frame side, in pixels: 32x32 tiles at most */
/* 13.22.0: sized for the worst case, a 2048x2048 copy: 1024 tiles at 32 bytes each plus the frame setup (it was 24 KiB,
   which a copy past about 765 tiles overran into the statics after it). cb drops bytes past the end and gpu_paint refuses the job. */
static unsigned char gpu_cl[34 * 1024] __attribute__((aligned(256)));
static unsigned gpu_px[64 * 64] __attribute__((aligned(4096)));
static unsigned gcn;
static void cb(unsigned v) { if (gcn < sizeof gpu_cl) gpu_cl[gcn] = (unsigned char)v; gcn++; }
static void cw(unsigned long long v, int n) { while (n--) { cb((unsigned)v & 255); v >>= 8; } }
static int gpu_wait(unsigned long reg, unsigned bit) {
    for (unsigned long t0 = gpu_now(); GR(reg) & bit;) if (gpu_now() - t0 > gpu_hz() / 100) return 0;
    return 1;
}
/* One LOAD_TILE_BUFFER_GENERAL (packet 30, V3D 4.2) for RT0: the tile's pixels come from the source rectangle's top left,
   raster rgba8, row stride spitch * 4. The V3D adds each tile's place, as it does for the store. */
static void cload(unsigned *src, unsigned spitch, unsigned h) {
    cb(30); cw(((unsigned long long)27 << 12) | ((unsigned long long)spitch * 4 << 28) | ((unsigned long long)h << 48), 8); cw(GPU_BUS(src), 4);
}
/* Paint w x h pixels at dst (rows pitch pixels apart). With src (rows spitch apart), each tile is loaded from src first and
   no clear goes between tiles, so the result is src's pixels; with src NULL the tiles start from rgba, as before.
   1 done, 0 with a line said. */
static int gpu_paint(unsigned *dst, unsigned pitch, unsigned w, unsigned h, unsigned rgba, unsigned *src, unsigned spitch) {
    if (!gpu_up) { show("gpu: not up, run gpu first\n"); return 0; }
    if (!w || !h || w > GPU_MAX || h > GPU_MAX || w > pitch || ((unsigned long)dst & 15)) { show("gpu: bad rectangle\n"); return 0; }
    if (src && (spitch < w || spitch > 65536 || ((unsigned long)src & 15))) { show("gpu: bad rectangle\n"); return 0; }
    unsigned tw = (w + 63) / 64, th = (h + 63) / 64;
    unsigned long span = ((unsigned long)(h - 1) * pitch + w) * 4;
    unsigned long sspan = src ? ((unsigned long)(h - 1) * spitch + w) * 4 : 0;
    gcn = 0;
    cb(121); cw(((unsigned long long)w << 8) | ((unsigned long long)h << 24) | (1ull << 46) | (1ull << 47), 8);   /* COMMON: w x h, 1 RT, early Z off, depth_24 */
    cb(121); cw(3 | ((unsigned long long)rgba << 8), 8);                          /* CLEAR_COLORS_PART1 for RT0 */
    cb(121); cw(1 | (2u << 6), 8);                                                 /* COLOR: RT0 32 bpp, internal type 8 */
    cb(121); cw(2 | (0x3f800000ull << 16), 8);                                     /* ZS_CLEAR_VALUES: Z 1.0, last */
    cb(122); cw(((unsigned long long)tw << 16) | ((unsigned long long)th << 24) | ((unsigned long long)tw << 32) | ((unsigned long long)th << 44), 8);   /* SUPERTILE_CFG: 1x1 supertiles */
    cb(124); cw(0, 3); cb(26);                                                     /* TILE_COORDINATES 0,0; END_OF_LOADS */
    cb(29); cw(8, 12);                                                             /* STORE_TILE_BUFFER_GENERAL: none */
    cb(25); cb(3); cb(27);                                                         /* CLEAR_TILE_BUFFERS RTs and Z/S; END_OF_TILE */
    for (unsigned ty = 0; ty < th; ty++)
        for (unsigned tx = 0; tx < tw; tx++) {
            cb(124); cw(tx | ty << 12, 3);                                         /* TILE_COORDINATES */
            if (src) cload(src, spitch, h);                                        /* this tile's pixels from the source */
            cb(26);                                                                /* END_OF_LOADS */
            cb(29); cw((27ull << 12) | ((unsigned long long)pitch * 4 << 28) | ((unsigned long long)h << 48), 8); cw(GPU_BUS(dst), 4);   /* RT0, raster rgba8 */
            if (tx + 1 < tw || ty + 1 < th) { if (!src) { cb(25); cb(3); } cb(27); }   /* the next tile starts from the clear colour, or from its load */
        }
    cb(27); cb(19); cb(13);                                                        /* END_OF_TILE, FLUSH_VCD_CACHE, END_OF_RENDERING */
    dcache_clean(gpu_cl, sizeof gpu_cl);
    dcache_clean(dst, span);   /* no dirty CPU line left to land on top of what the GPU writes */
    if (src) dcache_clean(src, sspan);   /* nor under what it reads */
    GW(GPU_CORE0 + GPU_L2TCACTL, 1);   /* L2TFLS, mode flush */
    if (!gpu_wait(GPU_CORE0 + GPU_L2TCACTL, 1)) { show("gpu: the L2T flush never finished\n"); return 0; }
    GW(GPU_CORE0 + GPU_SLCACTL, 0x0f0f0f0fu);
    unsigned f0 = GR(GPU_CORE0 + GPU_RFC);
    GW(GPU_CORE0 + GPU_CT1QBA, GPU_BUS(gpu_cl));
    if (gcn > sizeof gpu_cl) { show("gpu: job too big\n"); return 0; }   /* never reached at GPU_MAX, kept so a bigger list cannot run past the buffer */
    GW(GPU_CORE0 + GPU_CT1QEA, GPU_BUS(gpu_cl) + gcn);   /* this write starts the job */
    for (unsigned long t0 = gpu_now(); GR(GPU_CORE0 + GPU_RFC) == f0;)
        if (gpu_now() - t0 > gpu_hz() / 10) {
            ga("gpu: the job hung (CT1CS "); gx(GR(GPU_CORE0 + GPU_CT1CS)); ga(", at "); gx(GR(GPU_CORE0 + GPU_CT1CA)); ga(")"); showl();
            return 0;
        }
    GW(GPU_CORE0 + GPU_L2TCACTL, 1u << 8);   /* TMUWCF, then L2TFLS in clean mode */
    if (!gpu_wait(GPU_CORE0 + GPU_L2TCACTL, 1u << 8)) { show("gpu: the TMU write cache never cleaned\n"); return 0; }
    GW(GPU_CORE0 + GPU_L2TCACTL, 1 | (2u << 1));
    if (!gpu_wait(GPU_CORE0 + GPU_L2TCACTL, 1)) { show("gpu: the L2T clean never finished\n"); return 0; }
    dcache_clean(dst, span);   /* dc civac: drops any stale line before the CPU reads what the GPU wrote */
    return 1;
}
int gpu_clear(void (*say)(const char *), unsigned rgba) {
    gsay = say; gn = 0;
    for (unsigned i = 0; i < 64 * 64; i++) gpu_px[i] = 0;
    if (!gpu_paint(gpu_px, 64, 64, 64, rgba, 0, 0)) return 0;
    unsigned bad = 0;
    for (unsigned i = 0; i < 64 * 64; i++) bad += gpu_px[i] != rgba;
    if (bad) { ga("gpu: the V3D ran but "); gd(bad); ga(" of 4096 pixels are wrong (first "); gx(gpu_px[0]); ga(")"); showl(); return 0; }
    ga("gpu: the V3D cleared 64x64 pixels to "); gx(rgba); showl();
    return 1;
}
/* 13.20.0: the V3D paints a w x h rectangle of the screen at x,y (x a multiple of 4) in the screen's own pixel format, then
   the CPU checks every pixel and shows it. */
int gpu_fill(void (*say)(const char *), unsigned *scr, unsigned pitch, int x, int y, int w, int h, unsigned px) {
    gsay = say; gn = 0;
    if (!scr || x < 0 || y < 0 || w <= 0 || h <= 0 || (x & 3)) { show("gpu: bad rectangle\n"); return 0; }
    unsigned *dst = scr + (unsigned long)y * pitch + (unsigned)x;
    if (!gpu_paint(dst, pitch, (unsigned)w, (unsigned)h, px, 0, 0)) return 0;
    unsigned bad = 0;
    for (int j = 0; j < h; j++) for (int i = 0; i < w; i++) bad += dst[(unsigned long)j * pitch + (unsigned)i] != px;
    if (bad) { ga("gpu: the V3D ran but "); gd(bad); ga(" pixels are wrong"); showl(); return 0; }
    ga("gpu: the V3D painted "); gd((unsigned)w); ga("x"); gd((unsigned)h); ga(" pixels on the screen"); showl();
    return 1;
}
/* 13.20.1 (v14, slice 2): the V3D copies a w x h rectangle from src (rows spitch apart) to dst (rows dpitch apart). Each tile
   is loaded from the source and stored to the destination, so the copy is the compositing primitive. The CPU then checks
   every pixel against the source. */
int gpu_copy(void (*say)(const char *), unsigned *src, unsigned spitch, unsigned *dst, unsigned dpitch, int w, int h) {
    gsay = say; gn = 0;
    if (!src || !dst || w <= 0 || h <= 0 || spitch < (unsigned)w || dpitch < (unsigned)w) { show("gpu: bad rectangle\n"); return 0; }
    if (!gpu_paint(dst, dpitch, (unsigned)w, (unsigned)h, 0, src, spitch)) return 0;
    unsigned bad = 0;
    for (int j = 0; j < h; j++) for (int i = 0; i < w; i++) bad += dst[(unsigned long)j * dpitch + (unsigned)i] != src[(unsigned long)j * spitch + (unsigned)i];
    if (bad) { ga("gpu: the V3D ran but "); gd(bad); ga(" pixels are wrong"); showl(); return 0; }
    ga("gpu: the V3D copied "); gd((unsigned)w); ga("x"); gd((unsigned)h); ga(" pixels"); showl();
    return 1;
}
#endif

#ifndef GPU_HOST_TEST
extern void (*cmd_out)(const char *s);
static int gpu_word(const char *q, const char *w) { for (; *w; w++, q++) if (*q != *w) return 0; return 1; }
int gpu_command(const char *q, unsigned n) {   /* cmd.c: `gpu`, `gpu clear`, `gpu fill`, `gpu copy` */
    if (n < 3 || q[0] != 'g' || q[1] != 'p' || q[2] != 'u') return 0;
    if (n == 3) { gpu_bringup(cmd_out); return 1; }
    if (n == 9 && gpu_word(q + 3, " clear")) { gpu_clear(cmd_out, 0xff2c50b5u); return 1; }   /* terracotta #b5502c as RGBA8 bytes */
    if (n != 8) return 0;
    if (gpu_word(q + 3, " copy")) {   /* the top-left third of the back buffer, one third to the right */
        int w = (int)fb_w / 3 & ~15, h = (int)fb_h / 3;
        if (gpu_copy(cmd_out, fb, fb_pitch, fb + w, fb_pitch, w, h)) fb_flush(w, 0, w, h);
        return 1;
    }
    if (!gpu_word(q + 3, " fill")) return 0;
    /* a terracotta panel in the middle of the screen, a third of each side; the next redraw of that spot paints over it */
    int w = (int)fb_w / 3 & ~15, h = (int)fb_h / 3, x = ((int)fb_w - w) / 2 & ~15, y = ((int)fb_h - h) / 2;
    if (gpu_fill(cmd_out, fb, fb_pitch, x, y, w, h, fb_color(0xb5502cu) | 0xff000000u)) fb_flush(x, y, w, h);
    return 1;
}
#endif
