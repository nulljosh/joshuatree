/* Two of the i386 desktop's shape painters, out of kernel.c since 4.20.5: an antialiased line in physical pixels (the
   Stocks chart) and the glossy top-to-bottom gradient rounded rectangle (app tiles). i386 only; the ARM build never
   links this file. */
#include "window.h"
#include "gui_prims.h"
#include "gui_paint.h"
#include "gui_shapes.h"

#define AA_BAND gui_aa_band

/* Antialiased line primitive: Wu-style coverage over a segment of given
   `width` (physical px, 1.5-2.0 reads best), drawn straight at PHYSICAL
   resolution via window_pixel_phys/window_get_pixel_phys -- the same
   physical path gui_aa_char uses -- so the coverage fringe blends against
   whatever is actually underneath instead of landing as flat scaled
   blocks. Coordinates are LOGICAL (same convention as stx_chart's other
   callers); scaled to physical internally by window_scale(), matching
   gui_fill_circle's no-offscreen-target physical path. For each physical
   pixel near the segment, coverage is the distance from the pixel center
   to the nearest point on the segment, falling off over a 1px band
   centered on the line's half-width -- exact endpoints included, so the
   line caps flat rather than growing fuzzy stubs past x0,y0/x1,y1. */
void gui_aa_line_phys(double fx0, double fy0, double fx1, double fy1, unsigned int color, double width){
    double dx = fx1 - fx0, dy = fy1 - fy0;
    double len = gui_line_sqrt(dx * dx + dy * dy);
    double halfw = width / 2.0;
    int pad = (int)halfw + 2;
    if (len < 0.0001) {
        int minx = (int)fx0 - pad, maxx = (int)fx0 + pad;
        int miny = (int)fy0 - pad, maxy = (int)fy0 + pad;
        for (int y = miny; y <= maxy; y++) for (int x = minx; x <= maxx; x++) {
            double ddx = (x + 0.5) - fx0, ddy = (y + 0.5) - fy0;
            double dist = gui_line_sqrt(ddx * ddx + ddy * ddy);
            double cov = halfw + 0.5 - dist;
            if (cov <= 0) continue; if (cov > 1) cov = 1;
            int a = (int)(cov * 255 + 0.5);
            unsigned int d = window_get_pixel_phys(x, y);
            unsigned int r = (((color >> 16) & 0xFF) * a + ((d >> 16) & 0xFF) * (255 - a)) / 255;
            unsigned int g = (((color >> 8) & 0xFF) * a + ((d >> 8) & 0xFF) * (255 - a)) / 255;
            unsigned int b = ((color & 0xFF) * a + (d & 0xFF) * (255 - a)) / 255;
            window_pixel_phys(x, y, (r << 16) | (g << 8) | b);
        }
        return;
    }
    double ux = dx / len, uy = dy / len;
    int minx = (int)(fx0 < fx1 ? fx0 : fx1) - pad, maxx = (int)(fx0 > fx1 ? fx0 : fx1) + pad;
    int miny = (int)(fy0 < fy1 ? fy0 : fy1) - pad, maxy = (int)(fy0 > fy1 ? fy0 : fy1) + pad;
    for (int y = miny; y <= maxy; y++) {
        for (int x = minx; x <= maxx; x++) {
            double px = x + 0.5, py = y + 0.5;
            double t = (px - fx0) * ux + (py - fy0) * uy;
            if (t < 0) t = 0; if (t > len) t = len;
            double cx = fx0 + ux * t, cy = fy0 + uy * t;
            double ddx = px - cx, ddy = py - cy;
            double dist = gui_line_sqrt(ddx * ddx + ddy * ddy);
            double cov = halfw + 0.5 - dist;
            if (cov <= 0) continue; if (cov > 1) cov = 1;
            int a = (int)(cov * 255 + 0.5);
            if (a <= 0) continue;
            unsigned int d = window_get_pixel_phys(x, y);
            unsigned int r = (((color >> 16) & 0xFF) * a + ((d >> 16) & 0xFF) * (255 - a)) / 255;
            unsigned int g = (((color >> 8) & 0xFF) * a + ((d >> 8) & 0xFF) * (255 - a)) / 255;
            unsigned int b = ((color & 0xFF) * a + (d & 0xFF) * (255 - a)) / 255;
            window_pixel_phys(x, y, (r << 16) | (g << 8) | b);
        }
    }
}

/* Same corner AA as gui_rounded_rect, but the fill itself is a real top-to-
   bottom gradient instead of one flat color, the classic glossy-icon look
   (lighter catching the light at top, darker at the bottom, real depth),
   direct follow-up after "rich... gradient with a bit of a 3D icon style,
   like Apple" feedback on the flat-color first pass. Every pixel here is
   still a genuine precomputed solid color (gui_lerp), no alpha channel
   this framebuffer doesn't have, same technique the wallpaper and every
   other AA edge in this file already uses. */
void gui_rounded_rect_gradient(int x, int y, int w, int h, unsigned int color_top, unsigned int color_bottom, unsigned int bg, int r){
    /* Settings retina pass: this is the same logical-space-then-block-
       replicate staircase gui_fill_circle's v82 fix and gui_draw_capsule's
       matching fix already found and fixed for circles/capsules -- the AA
       ramp below is computed once per LOGICAL pixel, and at window_scale()
       2 (every real dock-launched windowed app, including Settings)
       window_rect/window_pixel replicate each logical pixel into a flat
       2x2 physical block with no interpolation, so the ramp rasterizes as
       distinct flat terraces: the Wind switch's track edge, the grouped
       card's corners and the sidebar highlight's corners, all real macro
       staircasing (confirmed visually, /tmp/jt-settings-4x-*.png). This
       function is the one AA rounded-rect primitive every one of those
       three draws through, so one fix here covers all three. Same
       technique gui_rounded_rect_on_wallpaper's v79 fix uses: work in
       PHYSICAL pixels, SSxSS true subsample coverage per corner pixel
       instead of a single distance threshold, blend straight to `bg`
       (already a flat color for every caller here, no wallpaper sampling
       needed the way the tray's on-wallpaper variant does). The original
       logical-space path stays for callers with an offscreen target
       pushed (window_has_target() true, e.g. gui_render_icon_cached's own
       6x-supersampled icon buffer): that path already gets its real AA
       from the later box-downsample, exactly like every icon glyph. */
    if (!window_has_target() && window_scale() > 1) {
        int sc = (int)window_scale();
        int px0 = x * sc, py0 = y * sc, pw = w * sc, ph = h * sc, pr = r * sc;
        const int SS = 4, band = 3, margin = 2;
        if (ph > 2 * pr) {
            for (int py = pr; py < ph - pr; py++) {
                int ly = py / sc;
                window_fill_rect_phys(px0, py0 + py, pw, 1, gui_lerp(color_top, color_bottom, ly, h));
            }
        }
        for (int py = 0; py < ph; py++) {
            if (py >= pr && py < ph - pr) continue;
            int cy = py < pr ? pr : ph - 1 - pr;
            int oy = py - cy;
            int ly = py / sc;
            unsigned int row_col = gui_lerp(color_top, color_bottom, ly, h);
            for (int px = 0; px < pw; px++) {
                int cx = px < pr ? pr : (px >= pw - pr ? pw - 1 - pr : px);
                int ox = px - cx;
                int d2 = ox * ox + oy * oy;
                unsigned int col = row_col;
                if (d2 > (pr - band) * (pr - band)) {
                    if (d2 > (pr + margin) * (pr + margin)) continue;
                    int inside = 0;
                    for (int sy = 0; sy < SS; sy++) {
                        int subdy = oy * SS + sy * 2 + 1 - SS;
                        for (int sx = 0; sx < SS; sx++) {
                            int subdx = ox * SS + sx * 2 + 1 - SS;
                            long sd2 = (long)subdx * subdx + (long)subdy * subdy;
                            if (sd2 <= (long)(pr * SS) * (pr * SS)) inside++;
                        }
                    }
                    if (inside == 0) continue;
                    col = (inside >= SS * SS) ? row_col : gui_lerp(row_col, bg, SS * SS - inside, SS * SS);
                }
                window_pixel_phys(px0 + px, py0 + py, col);
            }
        }
        return;
    }
    for (int row = 0; row < h; row++)
        window_rect(x, row + y, w, 1, gui_lerp(color_top, color_bottom, row, h));
    /* v37, real long-standing bug fixed here, not a tweak: this loop used
       to measure each corner pixel's distance from the rect's own CORNER
       (dx*dx + dy*dy) and keep everything within r of it as fill. That is
       inverted. The corner pixel is the one furthest outside a rounded
       corner, not inside it, so the true corner stayed filled and the
       background got painted in an arc *beside* it, giving every tile a
       square corner with a notch cut out of its side. It was nearly
       invisible while r was a fixed 12px and stayed that way for many
       versions; making the radius proportional to icon size (22%, the
       iOS/macOS squircle ratio) blew it up to a 39px artifact on every
       dock icon, which is how it finally got caught, by measuring real
       pixel values out of a screendump rather than trusting the shape.
       Correct math: distance from the arc's CENTER, which sits at
       (r, r) inside each corner. */
    for (int dy = 0; dy <= r; dy++){
        for (int dx = 0; dx <= r; dx++){
            int ox = r - dx, oy = r - dy;      /* offset from the arc's centre */
            int d2 = ox * ox + oy * oy;
            int inner = r - AA_BAND;
            if (d2 <= inner * inner) continue;  /* comfortably inside the curve: leave the gradient alone */
            unsigned int top_local = gui_lerp(color_top, color_bottom, dy, h);
            unsigned int bot_local = gui_lerp(color_top, color_bottom, h - 1 - dy, h);
            if (d2 >= r * r) {                  /* outside the curve: this is background */
                window_pixel(x + dx,         y + dy,         bg);
                window_pixel(x + w - 1 - dx, y + dy,         bg);
                window_pixel(x + dx,         y + h - 1 - dy, bg);
                window_pixel(x + w - 1 - dx, y + h - 1 - dy, bg);
                continue;
            }
            int t = gui_isqrt(d2) - inner;      /* inside the AA band: blend out to background */
            window_pixel(x + dx,         y + dy,         gui_lerp(top_local, bg, t, AA_BAND));
            window_pixel(x + w - 1 - dx, y + dy,         gui_lerp(top_local, bg, t, AA_BAND));
            window_pixel(x + dx,         y + h - 1 - dy, gui_lerp(bot_local, bg, t, AA_BAND));
            window_pixel(x + w - 1 - dx, y + h - 1 - dy, gui_lerp(bot_local, bg, t, AA_BAND));
        }
    }
}

/* v44.1: antialiased. The old version rounded each row's half-width to a
   whole pixel, so both slanted edges were staircases (the pencil tip in
   every dock render). Exact half-width in 8.8 fixed point; the outermost
   pixel on each side is blended by its fractional coverage against what
   is already there. */
void gui_fill_triangle_down(int cx, int y0, int half_w, int h, unsigned int color){
    if (h <= 0) return;
    for (int row = 0; row < h; row++){
        int w256 = (half_w * 256 * (h - row)) / h;   /* half-width, 8.8 */
        int wi = w256 >> 8, frac = w256 & 255;
        if (wi > 0) window_rect(cx - wi + 1, y0 + row, 2 * wi - 1, 1, color);
        if (frac) {
            unsigned int l = window_get_pixel(cx - wi, y0 + row), r = window_get_pixel(cx + wi, y0 + row);
            window_pixel(cx - wi, y0 + row, gui_lerp(l, color, frac, 256));
            window_pixel(cx + wi, y0 + row, gui_lerp(r, color, frac, 256));
        }
    }
}
