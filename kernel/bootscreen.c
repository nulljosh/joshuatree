/* The brand mark, the boot splash and the About window, out of kernel.c since 4.20.9. */
#include "libc.h"
#include "serial.h"
#include "window.h"
#include "font.h"
#include "irq.h"
#include "task.h"
#include "gui_prims.h"
#include "gui_paint.h"
#include "gui_shapes.h"
#include "desktop.h"
#include "version.h"
#include "ease.h"
#include "boot_mark.h"
#include "pmm.h"
#include "mouse.h"
#include "ring3app.h"
#include "bootscreen.h"

extern int gui_close_was_click;   /* kernel.c: the last close came from a click, not a key */

/* A brief boot splash instead of cutting straight to the desktop with no
   transition at all, the same beat every real OS gives a fresh boot: the
   logo shows immediately, and a thin progress bar only appears once that
   hold crosses a full second, so a genuinely fast boot (which this one
   almost always is) never shows a bar filling for no real reason, just
   the logo for a beat. Runs off ticks() (real PIT time, ~100Hz, already
   confirmed via the shell's own "sleep 1s" = sleep_ticks(100)), not a
   frame-counted loop, so it holds the same real duration regardless of
   how fast this machine happens to render each frame. */
/* The real landing brand mark (landing/logo.svg), not the old stick-figure
   gui_draw_logo primitive. boot_mark.h (tools/gen/gen_boot_mark.py) carries
   its rasterized 8-bit coverage; blended at PHYSICAL resolution via
   window_pixel_phys, the same pattern gui_aa_char uses for text. cx,cy are
   LOGICAL center coords, converted to physical here. */
static void gui_blend_cov(const unsigned char *cov, const unsigned char *acc, int cw, int ch, int cx, int cy, int size,
                          unsigned int ink, const unsigned char *rev, int lo, int hi){
    /* rev (boot_mark_rev) is the draw-on map: only pixels whose first frame is in (lo, hi] are blended, so the
       splash paints each pixel once as the pen reaches it. NULL draws everything. 3.0: acc is the part of each
       pixel's coverage that is the terracotta ground, blended in MARK_GROUND; the rest takes the ink. */
    int sc = window_has_target() ? 1 : (int)window_scale(); if (sc < 1) sc = 1;
    int T = size * sc; /* box-filtered from the stored coverage; T == the stored size is a straight copy */
    int ox = cx * sc - T / 2, oy = cy * sc - T / 2;
    for (int row = 0; row < T; row++){
        for (int col = 0; col < T; col++){
            int c0 = col * cw / T, c1 = (col + 1) * cw / T, r0 = row * ch / T, r1 = (row + 1) * ch / T;
            if (c1 == c0) c1 = c0 + 1; /* drawn bigger than stored: take the nearest source pixel instead of none */
            if (r1 == r0) r1 = r0 + 1;
            int sum = 0, gsum = 0, n = (c1 - c0) * (r1 - r0);
            for (int yy = r0; yy < r1; yy++) for (int xx = c0; xx < c1; xx++) { sum += cov[yy * cw + xx]; gsum += acc[yy * cw + xx]; }
            int a = sum / n, gnd = gsum / n;
            if (!a) continue;
            if (rev) { /* a scaled pixel appears with the earliest source pixel under it */
                int t = 0;
                for (int yy = r0; yy < r1; yy++) for (int xx = c0; xx < c1; xx++){ int v = rev[yy * cw + xx]; if (v && (!t || v < t)) t = v; }
                if (t <= lo || t > hi) continue;
            }
            int x = ox + col, y = oy + row;
            unsigned int d = window_get_pixel_phys(x, y), out = 0;
            for (int sh = 0; sh <= 16; sh += 8)
                out |= ((((ink >> sh) & 0xFF) * (a - gnd) + ((MARK_GROUND >> sh) & 0xFF) * gnd + ((d >> sh) & 0xFF) * (255 - a)) / 255) << sh;
            window_pixel_phys(x, y, out);
        }
    }
}
/* 2.25: small sizes (menu bar, About) take the bold copy, the tree drawn down its middle; the splash's thin
   one-line copy would fade to a grey smudge at 20 px. */
void gui_draw_mark_sized(int cx, int cy, int size, unsigned int ink){
    if (size <= 48) gui_blend_cov(menu_mark_cov, menu_mark_acc, MENU_MARK_W, MENU_MARK_H, cx, cy, size, ink, 0, 0, 0);
    else gui_blend_cov(boot_mark_cov, boot_mark_acc, BOOT_MARK_W, BOOT_MARK_H, cx, cy, size, ink, 0, 0, 0);
}
/* Draws the splash mark's pixels first shown in steps (lo, hi] of the draw-on. */
static void gui_draw_boot_mark_steps(int cx, int cy, unsigned int ink, int lo, int hi){
    int sc = window_has_target() ? 1 : (int)window_scale(); if (sc < 1) sc = 1;
    gui_blend_cov(boot_mark_cov, boot_mark_acc, BOOT_MARK_W, BOOT_MARK_H, cx, cy, BOOT_MARK_W / sc, ink, boot_mark_rev, lo, hi);
}

/* 2.41: the splash draws the tree on, trunk first and the terracotta ground last (3.0), the way a pen would, over BOOT_DRAW_TICKS,
   then the bar fills over BOOT_BAR_TICKS (the old 1.0 s splash plus 0.2 s). The pen eases out (fast then slow),
   and each frame blends only the pixels it newly reaches, so there is no per-frame clear. */
#define BOOT_DRAW_TICKS 80
#define BOOT_BAR_TICKS  40
void gui_draw_boot_screen(void){
    unsigned int bg = 0x00000000; /* pure black boot background, direct request */
    window_clear(bg);
    int cx = (int)window_width() / 2, cy = (int)window_height() / 2; /* v45.2: centred on the real window; 400 was the 800-wide centre and sat left of centre at 960 */

    unsigned int start = ticks();
    int bar_w = 160, bar_h = 6, bar_x = (int)window_width() / 2 - bar_w / 2, bar_y = (int)window_height() / 2 + 70;
    int bar_track_drawn = 0, drawn = 0;
    for (;;) {
        unsigned int elapsed = ticks() - start;
        unsigned int p = elapsed >= BOOT_DRAW_TICKS ? 1000 : elapsed * 1000 / BOOT_DRAW_TICKS; /* 0..1000 */
        int step = ease_out((int)p) * BOOT_MARK_STEPS / 1000; /* 6.4.0: the shared curve (lib/ease.h) */
        if (step > drawn) { /* v1.6.20: the real landing/logo.svg mark, white ink on the plain black screen, its ground terracotta */
            gui_draw_boot_mark_steps(cx, cy - 10, 0x00FFFFFF, drawn, step); drawn = step;
        }
        if (elapsed >= BOOT_DRAW_TICKS) {
            if (!bar_track_drawn) { window_rect(bar_x, bar_y, bar_w, bar_h, gui_blend(bg, 0x00FFFFFF)); bar_track_drawn = 1; }
            unsigned int since_bar = elapsed - BOOT_DRAW_TICKS;
            int fill = since_bar >= BOOT_BAR_TICKS ? bar_w : (int)(bar_w * since_bar / BOOT_BAR_TICKS);
            window_rect(bar_x, bar_y, fill, bar_h, 0x00FFFFFF); /* v0.76.48: was the same hardcoded maroon as the old logo, direct report, white to match the boot screen */
        }
        if (elapsed >= BOOT_DRAW_TICKS + BOOT_BAR_TICKS) break;
        window_present(); __asm__ volatile ("hlt");
    }
}

/* A real about panel, not a placeholder: actual physical memory stats
   straight from pmm (the same real physical memory manager the rest of
   this kernel allocates through) and real uptime off ticks(), the same
   PIT tick counter every other real-time feature in this file already
   uses.

   v0.76.13: direct request -- a real macOS "About This Mac" dialog is a
   small, centered card, not a full-screen takeover; this used to
   window_clear() the entire desktop and left-align every line from the
   screen edge. Now draws a real small floating card (ABOUT_W x ABOUT_H,
   centered both ways on screen, matching gui_multiwin_draw_one's own
   rounded-card style for visual consistency with the rest of this
   kernel's real floating windows) over the live desktop -- gui_draw_desktop
   is called first to clear the Apple-menu dropdown and show the real
   wallpaper/dock behind the card, the same way a real dialog sits over a
   real desktop. Every line of text is centered horizontally within the
   card via font_string_width, and the whole text block is centered
   vertically within the card too, not just left-pinned under the
   titlebar. Doesn't call the shared gui_wait_close() (its own hint text
   is hardcoded to the full screen's bottom-left, which would float
   oddly outside this small card) -- a local close-wait loop mirrors its
   exact same real mechanics (mouse_click_edge/kbd_pop/Escape) instead. */
void gui_launch_about(void){
    int ABOUT_W = 360, ABOUT_H = 220;
    int bx = ((int)window_width() - ABOUT_W) / 2;
    int by = ((int)window_height() - ABOUT_H) / 2;

    gui_draw_desktop(-1, -1, 0, 0); /* clears the Apple-menu dropdown, real wallpaper+dock behind the card */
    gui_rounded_rect_on_wallpaper(bx, by, ABOUT_W, ABOUT_H, 0x00FAF8F6, 18);
    gui_fill_circle(bx + 26, by + 20, 6, 0x00FF5F57, 0x00FAF8F6);
    gui_fill_circle(bx + 46, by + 20, 6, 0x00FFD64A, 0x00FAF8F6);
    gui_fill_circle(bx + 66, by + 20, 6, 0x00D8D4CE, 0x00FAF8F6);
    gui_close_glyph(bx + 26, by + 20, 0x00FF5F57);
    font_draw_string("-", bx + 43, by + 12, 0x00624A20, -1);
    { const char *title = "About Joshua Tree";
      font_draw_string(title, bx + (ABOUT_W - font_string_width(title)) / 2, by + 12, 0x00555555, -1); }

    char buf[64]; int n;
    unsigned int total_kb = pmm_total_frames() * 4, free_kb = pmm_free_frames() * 4;
    n = 0; buf[n++] = 'M'; buf[n++] = 'e'; buf[n++] = 'm'; buf[n++] = 'o'; buf[n++] = 'r'; buf[n++] = 'y'; buf[n++] = ':'; buf[n++] = ' ';
    n += app_utoa(free_kb, buf + n);
    buf[n++] = 'K'; buf[n++] = ' '; buf[n++] = 'f'; buf[n++] = 'r'; buf[n++] = 'e'; buf[n++] = 'e'; buf[n++] = ' '; buf[n++] = 'o'; buf[n++] = 'f'; buf[n++] = ' ';
    n += app_utoa(total_kb, buf + n);
    buf[n++] = 'K'; buf[n] = 0;
    char mem_line[64]; { int p = 0; const char *s = buf; while (*s) mem_line[p++] = *s++; mem_line[p] = 0; }

    /* v0.76.12: real bug, this line was hardcoded to "Version 0.42.1"
       for 30+ real version bumps despite JT_VERSION_STR (drivers/version.h,
       generated from the real VERSION file at build time) already
       existing and already used elsewhere (the boot serial log). */
    char version_line[32]; { int p = 0; const char *v = "Version " JT_VERSION_STR; while (*v && p < (int)sizeof(version_line) - 1) version_line[p++] = *v++; version_line[p] = 0; }

    const char *tagline = "A freestanding i386 kernel, written from scratch.";
    const char *lines[3] = { tagline, mem_line, version_line };
    unsigned int colors[3] = { 0x001C1C1E, 0x00884B16, 0x0075726E };
    int line_h = 24;
    gui_draw_mark_sized(bx + ABOUT_W / 2, by + 56, 32, 0x001C1C1E); /* 2.25: the mark heads the card, as the logo does on a Mac's About box */
    int text_top = by + (ABOUT_H - 3 * line_h) / 2 + 22; /* the text block sits under the mark */
    for (int i = 0; i < 3; i++) {
        int x = bx + (ABOUT_W - font_string_width(lines[i])) / 2; /* real horizontal centering per line */
        font_draw_string(lines[i], x, text_top + i * line_h, colors[i], -1);
    }

    { const char *hint = "esc or click to go back";
      font_draw_string(hint, bx + (ABOUT_W - font_string_width(hint)) / 2, by + ABOUT_H - 26, 0x0075726E, -1); }

    /* Local close-wait, not the shared gui_wait_close(): its own hint
       text is hardcoded to the full screen's bottom-left, which would
       float outside this small card. Same real mechanics otherwise
       (mouse_click_edge_sync/kbd_pop/Escape), copied rather than
       parameterized since this is the only call site that needs its own
       hint position. */
    window_present(); sleep_ticks(5);
    mouse_click_edge_sync();
    for (;;) {
        gui_app_mouse_tick();
        int sc = kbd_pop();
        if (sc >= 0 && !(sc & 0x80) && kbd_map(sc) == 27) { gui_close_was_click = 0; return; }
        if (mouse_click_edge()) { gui_close_was_click = 1; return; }
        window_present(); __asm__ volatile ("hlt");
    }
}
