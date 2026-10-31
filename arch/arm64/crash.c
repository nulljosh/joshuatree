/* crash.c: cut out of main.c (the god-file split). The crash screen: an unexpected exception at EL1 reported on the UART and drawn as a panel, then a quiet halt. Built as part of main.o (the Makefile's MAINSRC) with
   main.c's own flags; what it shares with main.c is declared in kmain.h. */
#include "kmain.h"
/* ---- The crash screen. An unexpected exception at EL1 (a kernel bug; the deliberate EL0 faults are answered in
   exc_el0_sync) ends here: the report goes out on the UART first, then onto the screen as a red panel over whatever was
   drawn, then the core sleeps in a wfe loop for good. Nothing here uses floating point or the console (which may be
   the thing that faulted): the UART is written raw and the panel is the 8x16 VGA font, integer stores only, cleaned
   out of the data cache so a real GPU shows it. A fault inside this code halts quietly rather than looping. ---- */
#define CRASH_BG 0x00900000
#define CRASH_ROWS 12
#define CRASH_COLS 100
static char crash_txt[CRASH_ROWS][CRASH_COLS];
static unsigned crash_row, crash_col;
static void crash_c(char c) { if (crash_row < CRASH_ROWS && crash_col < CRASH_COLS - 1) crash_txt[crash_row][crash_col++] = c; }
static void crash_s(const char *s) { while (*s) crash_c(*s++); }
static void crash_x(unsigned long v) { crash_s("0x"); for (int i = 60; i >= 0; i -= 4) crash_c("0123456789abcdef"[(v >> i) & 15]); }
static void crash_nl(void) { if (crash_row < CRASH_ROWS) crash_txt[crash_row][crash_col] = 0; crash_row++; crash_col = 0; }
static const char *crash_class(unsigned ec) {
    switch (ec) {
    case 0x00: return "unknown reason";   case 0x01: return "wfi or wfe trapped";   case 0x07: return "floating point access trapped";
    case 0x0e: return "illegal execution state";   case 0x15: return "svc";   case 0x20: case 0x21: return "instruction abort";
    case 0x22: return "pc alignment fault";   case 0x24: case 0x25: return "data abort";   case 0x26: return "sp alignment fault";
    case 0x2c: return "floating point exception";   case 0x2f: return "serror";   case 0x3c: return "brk";
    default: return "exception";
    }
}
void crash(const struct frame *f) {
    static int crashing;
    __asm__ volatile ("msr daifset, #3" ::: "memory");   /* no interrupt may run while we report */
    if (crashing++) for (;;) __asm__ volatile ("wfe");   /* a second fault, in here: stay quiet and halted */
    con_live = 0;   /* console_putc only keeps the log from now on: the screen is ours */
    unsigned long far; __asm__ volatile ("mrs %0, far_el1" : "=r"(far));
    unsigned ec = (unsigned)(f->esr >> 26), fsc = (unsigned)(f->esr & 0x3f);
    crash_s("KERNEL CRASH: "); crash_s(crash_class(ec));
    if (ec == 0x20 || ec == 0x21 || ec == 0x24 || ec == 0x25) {
        crash_s((fsc & 0x3c) == 0x0c ? ", permission fault level " : (fsc & 0x3c) == 0x04 ? ", translation fault level " : (fsc & 0x3c) == 0x08 ? ", access flag fault level " : ", fault code ");
        crash_c((char)('0' + ((fsc & 0x3c) == 0x0c || (fsc & 0x3c) == 0x04 || (fsc & 0x3c) == 0x08 ? (fsc & 3) : fsc % 10)));
    }
    crash_nl();
    crash_s("ESR "); crash_x(f->esr); crash_s(" EC "); crash_x(ec); crash_nl();
    crash_s("FAR "); crash_x(far); crash_nl();
    crash_s("ELR "); crash_x(f->elr); crash_nl();
    crash_s("last console lines:"); crash_nl();
    unsigned end = con_p.len;
    if (end && con_p.log[end - 1] == '\n') end--;
    unsigned start = end; int nl = 0;
    while (start > 0) { if (con_p.log[start - 1] == '\n' && ++nl >= 5) break; start--; }
    for (unsigned i = start; i < end; i++) { if (con_p.log[i] == '\n') crash_nl(); else if (con_p.log[i] != '\r') crash_c(con_p.log[i]); }
    crash_nl();
    crash_s("halted: this core sleeps in a wfe loop"); crash_nl();
    for (unsigned r = 0; r < CRASH_ROWS && r < crash_row; r++) {   /* the UART first: it works whatever state the screen is in */
        for (const char *p = crash_txt[r]; *p; p++) con_tx(*p);
        con_tx('\n');
    }
    if (fb) {
        fb_defer = fb_mute = 0;   /* a fault inside a batch: the crash screen goes out now */
        int s = ((int)fb_h * 10 / 600 + 5) / 10; if (s < 1) s = 1;
        int px = 16 * s, py = 32 * s, pw = (int)fb_w - 32 * s, ph = 16 * s * (CRASH_ROWS + 2);
        fb_rect(px, py, pw, ph, CRASH_BG);
        unsigned white = fb_color(0x00ffffff);
        int cols = (pw - 16 * s) / (8 * s);
        for (unsigned r = 0; r < CRASH_ROWS && r < crash_row; r++)
            for (int c = 0; c < cols && crash_txt[r][c]; c++) {
                char ch = crash_txt[r][c];
                if (ch < VGAFONT_FIRST || ch > VGAFONT_LAST) continue;
                const unsigned char *g = vgafont + (ch - VGAFONT_FIRST) * 16;
                int gx0 = px + 8 * s + c * 8 * s, gy0 = py + 8 * s + (int)r * 16 * s;
                for (int gy = 0; gy < 16; gy++) for (int gx = 0; gx < 8; gx++) if (g[gy] & (0x80 >> gx))
                    for (int a = 0; a < s; a++) for (int b = 0; b < s; b++) fb[(unsigned)(gy0 + gy * s + a) * fb_pitch + (unsigned)(gx0 + gx * s + b)] = white;
            }
        fb_flush(px, py, pw, ph);   /* out of the data cache to RAM: the GPU reads RAM */
    }
    for (;;) __asm__ volatile ("wfe");
}

/* Desktop slice 2: the dock is the i386 desktop's own painters (kernel/gui_paint.c) and layout (kernel/dock_geom.c),
   linked into this build. They draw through five window.h calls, answered here over the framebuffer: a 1080p or
   taller screen is the i386 desktop's 960x540 grid at scale 2, anything smaller is scale 1. */
#include "../../drivers/window.h"
#include "../../lib/ease.h"
#include "../../drivers/png.h"
#include "../../kernel/dock_geom.h"
#include "../../kernel/gui_paint.h"
#include "../../kernel/icon_art.h"
const unsigned char *const *const icon_art = ICON_ART;   /* keys.c draws the Launchpad from the same art: one copy of the PNGs */
const unsigned int *const icon_art_len = ICON_ART_LEN;
#include "../../kernel/boot_mark.h"
int dock_scale_pct = 7;   /* the i386 default (Settings can change it there; nothing does here yet) */
unsigned int window_scale(void) { return fb_h >= 1080 ? 2 : 1; }
unsigned int window_width(void) { return fb_w / window_scale(); }
unsigned int window_height(void) { return fb_h / window_scale(); }
/* Offscreen painting. On the Pi the framebuffer is the live scanout: a window repainted in place shows every half-drawn
   state (the cleared well, then the text going back) as flicker. off_begin copies the window's rectangle into a
   window-sized buffer and points fb at it, shifted so the same screen coordinates land inside it (fb_pitch becomes the
   window's width); the painters then draw as usual. off_end points fb back and copies the finished rectangle to the real
   screen row by row, top to bottom, in one pass. The buffer is one bump-heap block (2.3 MB for the 900x630 window at
   1080p), taken once at boot and kept; if the heap cannot give it, off_begin says 0 and the caller paints in place. */
unsigned *off_buf, *off_real;
static unsigned off_cap, off_real_pitch, off_x, off_y, off_w, off_h;
static int clip_x0, clip_y0, clip_x1 = 1 << 30, clip_y1 = 1 << 30;   /* window_pixel_phys stays inside the buffer while it is the target */
static void off_copy_row(unsigned *d, const unsigned *s, unsigned n) {
    unsigned long *dl = (unsigned long *)d; const unsigned long *sl = (const unsigned long *)s;
    unsigned i = 0;
    if ((((unsigned long)d | (unsigned long)s) & 7) == 0) for (; i + 2 <= n; i += 2) *dl++ = *sl++;
    for (d += i, s += i; i < n; i++) *d++ = *s++;
}
void off_reserve(int w, int h) { off_buf = kmalloc((unsigned)(w * h) * 4); off_cap = off_buf ? (unsigned)(w * h) : 0; }   /* once at boot, before any heap_mark: a mark's release would otherwise rewind over a block taken inside it */
int off_begin(int x, int y, int w, int h) {   /* physical pixels */
    if (off_on || !fb) return 0;
    if (x < 0 || y < 0 || w <= 0 || h <= 0 || x + w > (int)fb_w || y + h > (int)fb_h) return 0;
    if (!off_buf || (unsigned)(w * h) > off_cap) return 0;
    off_x = (unsigned)x; off_y = (unsigned)y; off_w = (unsigned)w; off_h = (unsigned)h;
    for (unsigned j = 0; j < off_h; j++) off_copy_row(off_buf + j * off_w, fb + (off_y + j) * fb_pitch + off_x, off_w);
    off_real = fb; off_real_pitch = fb_pitch;
    fb = (unsigned *)((unsigned long)off_buf - ((unsigned long)off_y * off_w + off_x) * 4);
    fb_pitch = off_w;
    clip_x0 = x; clip_y0 = y; clip_x1 = x + w; clip_y1 = y + h;
    off_on = 1;
    return 1;
}
void off_end(void) {
    if (!off_on) return;
    fb = off_real; fb_pitch = off_real_pitch; off_on = 0;
    clip_x0 = clip_y0 = 0; clip_x1 = clip_y1 = 1 << 30;
    for (unsigned j = 0; j < off_h; j++) off_copy_row(fb + (off_y + j) * fb_pitch + off_x, off_buf + j * off_w, off_w);
    fb_flush((int)off_x, (int)off_y, (int)off_w, (int)off_h);
}
unsigned long off_screen_hash(int x, int y, int w, int h) {   /* FNV-1a over a rectangle of the REAL screen, whichever buffer fb points at (the trace build's proof) */
    unsigned *r = off_on ? off_real : fb; unsigned p = off_on ? off_real_pitch : fb_pitch; unsigned long v = 1469598103934665603UL;
    for (int j = y; j < y + h; j++) for (int i = x; i < x + w; i++) v = (v ^ r[(unsigned)j * p + (unsigned)i]) * 1099511628211UL;
    return v;
}
void window_pixel_phys(int px, int py, unsigned int color) {
    if (px >= clip_x0 && py >= clip_y0 && px < clip_x1 && py < clip_y1 && px >= 0 && py >= 0 && px < (int)fb_w && py < (int)fb_h) fb[(unsigned)py * fb_pitch + (unsigned)px] = fb_color(color);
}
unsigned int window_get_pixel_phys(int px, int py) {
    return px >= clip_x0 && py >= clip_y0 && px < clip_x1 && py < clip_y1 && px >= 0 && py >= 0 && px < (int)fb_w && py < (int)fb_h ? fb_color(fb[(unsigned)py * fb_pitch + (unsigned)px]) : 0;
}
void window_fill_rect_phys(int px, int py, int w, int h, unsigned int color) {
    for (int y = py; y < py + h; y++) for (int x = px; x < px + w; x++) window_pixel_phys(x, y, color);
}
/* Slice 3: the logical calls the circle, capsule and window frame painters make. There is never an offscreen target
   here, and a logical pixel is a window_scale() square, as in drivers/window.c. */
int window_has_target(void) { return 0; }
void window_pixel(int x, int y, unsigned int color) { int s = (int)window_scale(); window_fill_rect_phys(x * s, y * s, s, s, color); }
void window_rect(int x, int y, int w, int h, unsigned int color) { int s = (int)window_scale(); window_fill_rect_phys(x * s, y * s, w * s, h * s, color); }
/* The painters' text: DejaVu Sans at 12 on the logical grid, the i386 UI size (a 24 pixel face in a 32 pixel line at
   scale 2), with the baseline 12.5 below the line box's top. Nothing draws before text_init has loaded the faces. */
void gui_text(const char *s, int x, int y, unsigned int fg) {
    int k = (int)window_scale();
    if (text_ok) text_draw(2, s, x * k, y * k + 25 * k / 2, 120 * k, fb_color(fg), fb, fb_pitch, (int)fb_w, (int)fb_h);
}
int gui_text_width(const char *s) { int k = (int)window_scale(); return text_ok ? text_width(2, s, 120 * k) / k : 0; }
/* The icon text (window labels): bold sans, face 0..3 a 16, 20, 24 or 28 physical pixel face times
   mul, as i386's wx_text, with the line box's top at logical ly; DejaVu's ascent puts the baseline 93% of a face down. */
void gui_icon_text(const char *s, int lx, int ly, int face, int mul, unsigned int fg) {
    int k = (int)window_scale(), px = (16 + 4 * face) * mul;
    if (text_ok) text_draw(1, s, lx * k, ly * k + px * 93 / 100, px * 10, fb_color(fg), fb, fb_pitch, (int)fb_w, (int)fb_h);
}
int gui_icon_text_w(const char *s, int face, int mul) { int k = (int)window_scale(); return text_ok ? (text_width(1, s, (16 + 4 * face) * mul * 10) + k - 1) / k : 0; }
/* The wallpaper is already on the screen, so reading the framebuffer is reading the wallpaper, as long as the dock is
   painted before anything else covers its band. */
unsigned int gui_wallpaper_sample(int px, int py, int sway) { (void)sway; return window_get_pixel_phys(px, py); }
/* 2.25: the brand mark in the menu bar's corner, as on i386 (gui_draw_mark_sized): the small copy of the scribbled
   tree from kernel/boot_mark.h, box-filtered to T physical pixels and blended in ink over what is there, its ground
   (menu_mark_acc, 3.0) in terracotta. Drawn bigger than stored (the boot screen, 220 px at 1080p), it blends the four
   nearest stored pixels instead, so the curves stay smooth rather than stepping in 2 to 3 pixel blocks. */
static unsigned mark_lerp_n(const unsigned char *m, int n, int x0, int y0, int wx, int wy) {
    int x1 = x0 + 1 < n ? x0 + 1 : x0, y1 = y0 + 1 < n ? y0 + 1 : y0;
    return (unsigned)(((m[y0 * n + x0] * (256 - wx) + m[y0 * n + x1] * wx) * (256 - wy)
                     + (m[y1 * n + x0] * (256 - wx) + m[y1 * n + x1] * wx) * wy) >> 16);
}
static unsigned mark_lerp(const unsigned char *m, int x0, int y0, int wx, int wy) { return mark_lerp_n(m, MENU_MARK_W, x0, y0, wx, wy); }
#ifdef QUIET_BOOT
/* The splash's mark: the 160 px stored copy (the menu's 80 px one, stretched 2.75x, was the smudge on the Pi), ink over a
   known flat ground written straight to the screen, so nothing depends on what was there before. */
void splash_mark(int cx, int cy, int T, unsigned ink, unsigned bg) {
    int ox = cx - T / 2, oy = cy - T / 2, n = BOOT_MARK_W;
    for (int row = 0; row < T; row++) for (int col = 0; col < T; col++) {
        int fx = (2 * col + 1) * n * 128 / T - 128, fy = (2 * row + 1) * n * 128 / T - 128;
        if (fx < 0) fx = 0;
        if (fy < 0) fy = 0;
        unsigned a = mark_lerp_n(boot_mark_cov, n, fx >> 8, fy >> 8, fx & 255, fy & 255);
        if (!a) continue;
        unsigned out = 0;
        for (int sh = 0; sh <= 16; sh += 8) out |= ((((ink >> sh) & 0xFF) * a + ((bg >> sh) & 0xFF) * (255 - a)) / 255) << sh;
        fb_rect(ox + col, oy + row, 1, 1, out);
    }
    fb_flush(ox, oy, T, T);   /* out of the data cache to RAM: the GPU reads RAM, so unflushed rows reached the Pi screen as torn slivers */
}
#endif
void menu_mark_paint(int cx, int cy, int T, unsigned ink) {
    int ox = cx - T / 2, oy = cy - T / 2;
    for (int row = 0; row < T; row++) for (int col = 0; col < T; col++) {
        unsigned a, g;
        if (T > MENU_MARK_W) {   /* sample point in 1/256 stored pixels, centre to centre */
            int fx = (2 * col + 1) * MENU_MARK_W * 128 / T - 128, fy = (2 * row + 1) * MENU_MARK_H * 128 / T - 128;
            if (fx < 0) fx = 0;
            if (fy < 0) fy = 0;
            a = mark_lerp(menu_mark_cov, fx >> 8, fy >> 8, fx & 255, fy & 255);
            g = mark_lerp(menu_mark_acc, fx >> 8, fy >> 8, fx & 255, fy & 255);
        } else {
            int c0 = col * MENU_MARK_W / T, c1 = (col + 1) * MENU_MARK_W / T, r0 = row * MENU_MARK_H / T, r1 = (row + 1) * MENU_MARK_H / T;
            if (c1 == c0) c1 = c0 + 1;
            if (r1 == r0) r1 = r0 + 1;
            int sum = 0, gsum = 0, n = (c1 - c0) * (r1 - r0);
            for (int y = r0; y < r1; y++) for (int x = c0; x < c1; x++) { sum += menu_mark_cov[y * MENU_MARK_W + x]; gsum += menu_mark_acc[y * MENU_MARK_W + x]; }
            a = (unsigned)(sum / n); g = (unsigned)(gsum / n);
        }
        if (!a) continue;
        unsigned d = window_get_pixel_phys(ox + col, oy + row), out = 0;
        for (int sh = 0; sh <= 16; sh += 8)
            out |= ((((ink >> sh) & 0xFF) * (a - g) + ((MARK_GROUND >> sh) & 0xFF) * g + ((d >> sh) & 0xFF) * (255 - a)) / 255) << sh;
        window_pixel_phys(ox + col, oy + row, out);
    }
}
/* The live Calendar tile shows a month and day once network time is available. */
unsigned long net_clock_utc(void);   /* ip.c: UTC seconds once the network has told us, 0 before */
unsigned long civil_days(long y, unsigned m, unsigned d);
void clock_local(unsigned long utc, unsigned *hh, unsigned *mm, unsigned *mon, unsigned *day);
long clock_year;   /* the local year of the last clock_local call */
#ifdef CAL_TEST_UTC
unsigned long cal_utc(void) { return CAL_TEST_UTC; }   /* the caltest build: a fixed date for arm64-calicon-check.py */
#elif defined(MUSIC_TEST_UTC)
unsigned long cal_utc(void) { return MUSIC_TEST_UTC + cnt_now() / cnt_hz(); }   /* the musictest build: a clock that ticks, which paces Music's `music: at` lines (arm64-music-check.py) */
#else
unsigned long cal_utc(void) { return net_clock_utc(); }
#endif
long clock_offset(unsigned long utc);   /* seconds to subtract from UTC for Vancouver wall time (the one time-zone path) */
unsigned long cal_local(void) { unsigned long u = cal_utc(); return u ? u - (unsigned long)clock_offset(u) : 0; }   /* 0 stays 0: no clock yet */
int cal_shown = -1;   /* the day the tile shows, as a y/m/d number; -1 is the plain art */
int cal_today(void) {   /* today's y/m/d number, or -1 with no clock */
    unsigned long u = cal_utc(); if (!u) return -1;
    unsigned h, m, mo, dd; clock_local(u, &h, &m, &mo, &dd);
    return (int)(clock_year * 12 + (long)mo - 1) * 32 + (int)dd;
}
void cal_overlay(unsigned *tile, int pw, int today) {
    static const char *const months[] = {"JAN", "FEB", "MAR", "APR", "MAY", "JUN", "JUL", "AUG", "SEP", "OCT", "NOV", "DEC"};
    unsigned mo = (unsigned)(today / 32 % 12), d = (unsigned)(today % 32);
    char day[3]; int n = 0;
    if (d >= 10) day[n++] = '0' + d / 10;
    day[n++] = '0' + d % 10; day[n] = 0;
    for (int y = pw * 49 / 128; y < pw * 103 / 128; y++)
        for (int x = pw * 24 / 128; x < pw * 104 / 128; x++) tile[y * pw + x] = 0x00F3F3F6;
    int month_size = pw * 16 / 10, day_size = pw * 38 / 10;
    text_draw(1, months[mo], (pw - text_width(1, months[mo], month_size)) / 2,
              pw * 44 / 128, month_size, 0x00FFFFFF, tile, pw, pw, pw);
    text_draw(1, day, (pw - text_width(1, day, day_size)) / 2,
              pw * 96 / 128, day_size, 0x001C1C1E, tile, pw, pw, pw);
}
int dock_tile(int slot, int shadow) {   /* one slot's art onto the dock; shadow 0 repaints the tile alone, its corners keep the old shadow */
    static const int order[GUI_ICON_COUNT] = GUI_DOCK_DEFAULT_ORDER;
    int size = DOCK_ICON, pw = size * (int)window_scale(), drawn = 0;
    int icon = order[slot], cx = gui_slot_x(slot) + size / 2, cy_bottom = gui_dock_y0() + DOCK_PAD + size;
    if (shadow) gui_draw_icon_shadow(cx, cy_bottom, size);
    unsigned long mark = heap_mark();   /* the bump heap never frees: the decode and the tile are rolled back */
    unsigned int *tile = kmalloc((unsigned)(pw * pw) * 4);
    unsigned char *art = 0;
    unsigned aw = 0, ah = 0, ach = 0;
    if (tile && png_decode(ICON_ART[icon], ICON_ART_LEN[icon], &art, &aw, &ah, &ach) == 0 && art && aw == ICON_ART_SIZE && ah == ICON_ART_SIZE && ach == 4) {
        gui_icon_art_scale(art, tile, pw, DOCK_TRAY_COLOR);
        if (icon == 2) { cal_shown = cal_today(); if (cal_shown >= 0) cal_overlay(tile, pw, cal_shown); }   /* ICON_ART[2], Calendar */
        gui_blit_tile(tile, cx - size / 2, cy_bottom - size, size, DOCK_TRAY_COLOR);
        drawn = 1;
    }
    heap_release(mark);
    return drawn;
}
int dock_mag_on(void);   /* 6.3.0, below */
static unsigned dots_shown;   /* 4.26.0: the slots whose running-app dot is on the screen now */
void dock_paint(void) {
    dots_shown = 0;   /* a fresh dock has none: dock_dots_tick draws them again on the next pass */
    gui_draw_dock_tray();
    int drawn = 0;
    for (int slot = 0; slot < GUI_ICON_COUNT; slot++) drawn += dock_tile(slot, 1);
    uart_puts("M1d dock "); uart_dec((unsigned)drawn); uart_puts(" icons\n");
}
/* Slice 3: the hover label, the i386 one (gui_draw_dock_label). The first hover keeps a copy of the dock's band, so
   moving to another slot or to none puts the plain band back before the next label; nothing is allocated until then.
   The pointer drives it (slice 4, below); the dockhover test build calls it once at boot. Names follow APPS[] in
   kernel/kernel.c for GUI_DOCK_DEFAULT_ORDER (arm64-m1c-check.py compares them). */
const char *const dock_names[GUI_ICON_COUNT] = {"Apps", "Burrow", "Mail", "Calendar", "Notes", "Reminders", "Terminal", "Samantha", "Weather", "Stocks", "Trash"};
unsigned *dock_band, *dock_mem;   /* dock_mem is the band copy's memory, taken at boot like the Browser's buffer so no mark's release can rewind over it */
int cur_on;           /* slice 4: the pointer's arrow is on the screen */
void cur_hide(void);
void cur_show(void);
/* The band copy starts DOCK_BAND_LIFT above band_top: the label's capsule sits right at band_top and its soft edge reaches
   about five pixels higher, which a copy from band_top itself left behind over the line above the dock (6.3.0). */
int dock_band_y(void) { return (gui_dock_band_top() - DOCK_BAND_LIFT) * (int)window_scale(); }
void dock_hover(int slot) {
    int top = dock_band_y(), rows = (int)fb_h - top;
    int arrow = cur_on;
    cur_hide();   /* the band copy must never hold the arrow, or every later restore paints a ghost of it */
    if (!dock_band) {
        unsigned long mark = heap_mark();
        dock_band = dock_mem ? dock_mem : kmalloc((unsigned)rows * fb_pitch * 4);
        if (!dock_band) { heap_release(mark); uart_puts("oom dock hover\n"); return; }
        for (unsigned i = 0; i < (unsigned)rows * fb_pitch; i++) dock_band[i] = fb[(unsigned)top * fb_pitch + i];
    } else for (unsigned i = 0; i < (unsigned)rows * fb_pitch; i++) fb[(unsigned)top * fb_pitch + i] = dock_band[i];
    if (slot >= 0 && slot < GUI_ICON_COUNT) {
        gui_draw_dock_label(gui_slot_x(slot) + DOCK_ICON / 2, gui_dock_y0(), dock_names[slot]);
        con_quiet = 1;   /* the UART only: a pointer sweeping the dock would fill the Console */
        uart_puts("M1d hover "); uart_dec((unsigned)slot); uart_putc(' '); uart_puts(dock_names[slot]); uart_putc('\n');
    }
    fb_flush(0, top, (int)fb_w, rows);
    if (arrow) cur_show();
}
/* 4.26.0: the running-app dots (Joshua, 2026-10-07: "a little light below the icon, like macOS, so if the Console is open
   you can see it"). A tile is open when its EL0 app has a slot, and the Terminal's when the Console or Terminal window is
   up (they share one window). The main loop calls this every pass; it draws only when the set changes, and writes the
   same pixels into the hover band copy so a hover label's restore keeps them. */
static int slot_open(int slot) {
    if (slot == 6) return con_live;
    if (slot == 0 || slot == 7 || slot == GUI_ICON_COUNT - 1) return 0;   /* Apps (the Launchpad), Samantha (the Terminal's sam), Trash */
    const char *n = dock_app_name(slot);
    for (unsigned k = 0; k < APP_SLOTS; k++) {
        if (!apps[k].cur) continue;
        const char *a = apps[k].cur->name; unsigned i = 0;
        while (n[i] && (n[i] | 32) == a[i]) i++;
        if (!n[i] && !a[i]) return 1;
    }
    return 0;
}
#define DOT_BOX 41   /* the pixels a dot can touch, (DOCK_DOT_R + 3) * scale either side of its centre, up to scale 4 */
static unsigned dot_under[GUI_ICON_COUNT][DOT_BOX * DOT_BOX];   /* what each dot covers: put back exactly when it goes */
void dock_dots_tick(void) {
    if (dock_mag_on()) { dock_mag_tick(); return; }   /* 6.3.0: no dots under a magnified dock (the next pass after it draws them); 6.4.0: its grow-in */
    if (!fb || login_live) return;
#ifdef QUIET_BOOT
    if (splash_on) return;
#endif
    unsigned m = 0;
    for (int s = 0; s < GUI_ICON_COUNT; s++) if (slot_open(s)) m |= 1u << s;
    if (m == dots_shown) return;
    int arrow = cur_on, sc = (int)window_scale(), top = dock_band_y(), r = (DOCK_DOT_R + 3) * sc;
    if (2 * r + 1 > DOT_BOX) return;
    cur_hide();
    for (int s = 0; s < GUI_ICON_COUNT; s++) {
        if (!((m ^ dots_shown) >> s & 1)) continue;
        int on = m >> s & 1, cx = (gui_slot_x(s) + DOCK_ICON / 2) * sc, cy = (gui_dock_y0() + DOCK_PAD + DOCK_ICON + DOCK_DOT_DY) * sc;
        for (int y = -r; y <= r; y++) for (int x = -r; x <= r; x++) {   /* save under it before it is drawn, or put that back */
            int px = cx + x, py = cy + y;
            if (py < 0 || py >= (int)fb_h || px < 0 || px >= (int)fb_w) continue;
            unsigned *u = &dot_under[s][(y + r) * DOT_BOX + (x + r)], *f = &fb[(unsigned)py * fb_pitch + (unsigned)px];
            if (on) *u = *f; else *f = *u;
        }
        if (on) gui_draw_dock_dot(s);
        if (dock_band) for (int y = cy - r; y <= cy + r; y++) for (int x = cx - r; x <= cx + r; x++)
            if (y >= top && y < (int)fb_h && x >= 0 && x < (int)fb_w) dock_band[(unsigned)(y - top) * fb_pitch + (unsigned)x] = fb[(unsigned)y * fb_pitch + (unsigned)x];
        fb_flush(cx - r, cy - r, 2 * r + 1, 2 * r + 1);   /* physical pixels, like dock_hover's */
    }
    if (arrow) cur_show();
    dots_shown = m;
    char b[GUI_ICON_COUNT + 12] = "dock dots "; int n = 10;   /* one 0 or 1 per slot, for tools/checks/dockdots-check.py */
    for (int k = 0; k < GUI_ICON_COUNT; k++) b[n++] = (char)('0' + (m >> k & 1));
    b[n++] = '\n'; b[n] = 0;
    con_quiet = 1; uart_puts(b);
}

/* 6.3.0: the magnifying dock (the Mac OS X 2000 introduction). With the pointer on the dock, the tiles within three slots of
   it grow, up to 1.6 times at the pointer (1.3 on Small, none when Settings turns it off, 7.4.0), on a smooth falloff; the tray widens to hold them, and the layout shifts so the
   tile under the pointer stays under it. Tiles grow upward into the room between the tray and the lowest window (MAG_ROOM:
   windows end at band_top - 22), with the hover label above the grown tile, so nothing a window draws is ever covered.
   The rows from MAG_ROOM above the tray to the bottom of the screen are copied once when the pointer arrives and put back
   exactly when it leaves. The decoded icon art is kept from the first hover, so a move redraws without a PNG decode. */
extern int dock_mag;   /* 7.4.0, settings.c: 0 off, 1 small, 2 large */
static const int mag_pct[3] = { 100, 130, 160 };   /* at the pointer */
#define MAG_REACH 300   /* hundredths of a slot either side that grow */
#define MAG_ROOM  46    /* logical px above the tray: wallpaper only (the lowest window ends band_top - 22) */
#define MAG_IN_MS 150   /* 6.4.0: the grow-in on arrival, on the shared ease_out */
static unsigned char *mag_art[GUI_ICON_COUNT];
static unsigned *mag_copy;
static int mag_on, mag_slot = -2, mag_mx, mag_amt = 1000;   /* mag_amt: how far grown in, thousandths */
static unsigned long mag_t0;
static int mag_skip_in;   /* the next arrival is fully grown at once (dock_magnify_now) */
static unsigned long mag_ms(void) { unsigned long f, c; __asm__ volatile ("mrs %0, cntfrq_el0\n mrs %1, cntpct_el0" : "=r"(f), "=r"(c)); return c / ((f ? f : 54000000) / 1000); }
int dock_mag_on(void) { return mag_on; }
void dock_mag_forget(void) { mag_on = 0; mag_slot = -2; }   /* the desktop was repainted: the copy is stale */
static int mag_cache(void) {
    static const int order[GUI_ICON_COUNT] = GUI_DOCK_DEFAULT_ORDER;
    for (int s = 0; s < GUI_ICON_COUNT; s++) if (!mag_art[s]) {
        unsigned char *art = 0; unsigned aw = 0, ah = 0, ach = 0;
        if (png_decode(ICON_ART[order[s]], ICON_ART_LEN[order[s]], &art, &aw, &ah, &ach) || !art || aw != ICON_ART_SIZE || ah != ICON_ART_SIZE || ach != 4) return 0;
        mag_art[s] = art;
    }
    return 1;
}
/* The pointer is on the dock: the tray's own rows, from a tile's width before the first to a tile's width after the last. */
int dock_mag_zone(int lx, int ly) {
    if (spot_live || !dock_mag) return 0;   /* the Launchpad card and Spotlight reach into the room above the tray */
    int y0 = gui_dock_y0(), x0 = gui_dock_x0();
    return ly >= y0 && ly < y0 + DOCK_ICON + 2 * DOCK_PAD && lx >= x0 - DOCK_ICON && lx < x0 + gui_dock_w() + DOCK_ICON;
}
void dock_unmagnify(void) {
    if (!mag_on) return;
    int sc = (int)window_scale(), top = (gui_dock_y0() - MAG_ROOM) * sc, rows = (int)fb_h - top;
    int arrow = cur_on; cur_hide();
    for (unsigned i = 0; i < (unsigned)rows * fb_pitch; i++) fb[(unsigned)top * fb_pitch + i] = mag_copy[i];
    fb_flush(0, top, (int)fb_w, rows);
    mag_on = 0; mag_slot = -2;
    con_quiet = 1; uart_puts("dock magnify off\n");
    if (arrow) cur_show();
}
void dock_magnify_now(int mx, int slot) { mag_skip_in = 1; dock_magnify(mx, slot); mag_skip_in = 0; }   /* after a click on a tile: no second grow-in */
void dock_mag_tick(void) { if (mag_on && mag_amt < 1000) dock_magnify(mag_mx, mag_slot); }   /* 6.4.0: the grow-in, from the main loop */
void dock_magnify(int mx, int slot) {
    int sc = (int)window_scale(), size = DOCK_ICON, step = DOCK_ICON + DOCK_GAP, y0 = gui_dock_y0();
    int top = (y0 - MAG_ROOM) * sc, rows = (int)fb_h - top, cy_bottom = y0 + DOCK_PAD + size;
    if (top < 0 || !fb) return;
    int arrow = cur_on; fb_hold(); cur_hide();   /* the band and the arrow reach the screen as one copy: no blink of the arrow on a grow-in frame */
    if (!mag_on) {
        if (!mag_cache()) { if (arrow) cur_show(); fb_release(); return; }
        if (dock_band) dock_hover(-1);                      /* any label off first: the copy holds the plain dock */
        if (!mag_copy) mag_copy = kmalloc((unsigned)rows * fb_pitch * 4);
        if (!mag_copy) { if (arrow) cur_show(); fb_release(); uart_puts("oom dock magnify\n"); return; }
        for (unsigned i = 0; i < (unsigned)rows * fb_pitch; i++) mag_copy[i] = fb[(unsigned)top * fb_pitch + i];
        mag_on = 1; mag_t0 = mag_skip_in ? 0 : mag_ms(); mag_skip_in = 0;
    } else for (unsigned i = 0; i < (unsigned)rows * fb_pitch; i++) fb[(unsigned)top * fb_pitch + i] = mag_copy[i];
    unsigned long e = mag_ms() - mag_t0;
    int was = mag_amt; mag_amt = ease_out(e >= MAG_IN_MS ? 1000 : (int)(e * 1000 / MAG_IN_MS)); mag_mx = mx;
    int sz[GUI_ICON_COUNT], left[GUI_ICON_COUNT], cum = 0;
    for (int i = 0; i < GUI_ICON_COUNT; i++) {                /* each tile's size from its distance to the pointer */
        int c = gui_slot_x(i) + size / 2, d = (mx > c ? mx - c : c - mx) * 100 / step, w = d < MAG_REACH ? MAG_REACH - d : 0;
        int s = size + size * (mag_pct[dock_mag] - 100) / 100 * w * w / (MAG_REACH * MAG_REACH) * mag_amt / 1000;
        if (s > size + MAG_ROOM - 26 + DOCK_PAD) s = size + MAG_ROOM - 26 + DOCK_PAD;   /* the grown tile and its label stay inside the room */
        sz[i] = s; left[i] = cum; cum += s + DOCK_GAP;
    }
    int total = cum - DOCK_GAP, x0 = gui_slot_x(0), end = gui_slot_x(GUI_ICON_COUNT - 1) + size;
    int ax = mx < x0 ? x0 : mx >= end ? end - 1 : mx, k = (ax - x0) / step, frac = (ax - x0) % step;   /* where the pointer sits, in slots */
    int anchor = left[k] + (frac > size ? sz[k] + (frac - size) : frac * sz[k] / size);   /* the same place in the grown layout */
    int shift = ax - anchor;
    gui_draw_dock_tray_at(shift - DOCK_PAD, total + 2 * DOCK_PAD);
    for (int i = 0; i < GUI_ICON_COUNT; i++) {
        int lx = shift + left[i], cx = lx + sz[i] / 2, pw = sz[i] * sc;
        gui_draw_icon_shadow(cx, cy_bottom, sz[i]);
        unsigned long mark = heap_mark();
        unsigned *tile = kmalloc((unsigned)(pw * pw) * 4);
        if (tile) {
            gui_icon_art_scale(mag_art[i], tile, pw, DOCK_TRAY_COLOR);
            if (i == 3 && cal_shown >= 0) cal_overlay(tile, pw, cal_shown);   /* slot 3, the live Calendar */
            gui_blit_tile(tile, lx, cy_bottom - sz[i], sz[i], DOCK_TRAY_COLOR);
        }
        heap_release(mark);
        if (slot_open(i)) gui_draw_dock_dot_at(cx);
    }
    if (slot >= 0 && slot < GUI_ICON_COUNT) gui_draw_dock_label(shift + left[slot] + sz[slot] / 2, y0 + 8 * mag_amt / 1000 - (sz[slot] - size), dock_names[slot]);   /* frame zero of a grow-in is dock_hover's spot; grown, 2 above the tile, inside MAG_ROOM */
    fb_flush(0, top, (int)fb_w, rows);
    if (slot != mag_slot || (mag_amt == 1000 && was < 1000)) {   /* a new tile, or the grow-in done */
        mag_slot = slot; con_quiet = 1;
        uart_puts("dock magnify "); uart_dec((unsigned)(slot < 0 ? 99 : slot)); uart_puts(" size "); uart_dec((unsigned)(slot >= 0 ? sz[slot] : 0));
        uart_puts(" of "); uart_dec((unsigned)size); uart_putc('\n');
        if (slot >= 0) { con_quiet = 1; uart_puts("M1d hover "); uart_dec((unsigned)slot); uart_putc(' '); uart_puts(dock_names[slot]); uart_putc('\n'); }
    }
    if (arrow) cur_show();
    fb_release();
}
