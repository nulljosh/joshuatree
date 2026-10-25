/* The i386 desktop's antialiased UI text, out of kernel.c since 4.23.7: gui_aa_char (the DejaVu Sans glyph
   blended per physical pixel) and its mono variant, and the wx_* physical-resolution text the Weather window,
   the Calendar year view and gui_icon_text draw with. i386 only. */
#include "libc.h"
#include "window.h"
#include "gui_prims.h"
#include "gui_paint.h"
#include "desktop.h"
#include "../lib/text_ink.h"
#include "ttf.h"
#include "ttf_render.h"
#include "editor_fonts.h"
#include "uitext.h"

/* v50: DejaVu Sans, not Mono. Direct feedback: system UI text (menu bar,
   dock hover labels, titlebars) read as monospace/typewriter, not the
   proportional humanist look real macOS chrome uses (SF/Helvetica);
   turned out that was literally true, v44 hardcoded family index 2
   (Mono) for every string in the OS, the exact "mono outside a char
   grid" mismatch this project's own design rule warns about. DejaVu Sans
   (family 0) is the closest already-embedded substitute for SF/Helvetica,
   no new font asset needed, same glyph table the Notes editor's own
   "Sans" option already ships and proves out. Positioning is unaffected
   either way: font_draw_string's own fixed 8px-logical (16px physical)
   advance per character, not the glyph's natural width, is what places
   every character in this kernel, on purpose (see font_set_aa's own
   note), so this is a pure typeface swap. Proportional Sans glyphs run
   wider than Mono at a few characters ('M','W'); the existing `x >= px +
   16` clamp below already clips rather than collides into the next
   cell, same safety net Mono relied on, nothing new to add. */
/* v77: texttest mirrors every line to serial so tools/textspacing-check.sh can read the verdict headless, the pngtest pattern. */
#define GUI_AA_GLYPH(c) (&editor_glyphs[((0 * 2 + 0) * 4 + 2) * 95 + ((c) - 32)]) /* DejaVu Sans, regular, 24px */
/* v77: the real per-glyph advance in physical px, the metric the fixed
   16px cell ignored (see font_draw_string's own note). Degree ring and
   anything outside 32..126 keep the old cell so nothing else moves. */
int gui_aa_advance(unsigned char c){
    if (c < 32 || c > 126) return 16;
    return GUI_AA_GLYPH(c)->advance;
}
void gui_aa_char(unsigned char c, int px, int py, unsigned int fg, int bg, int cell){
    if (c < 32 || c > 126) c = (c == 0xF8) ? 176 : '?'; /* 0xF8 is the CP437 degree sign the weather uses */
    const struct editor_glyph *g;
    if (c == 176) { /* degree: DejaVu has it, but the table only carries 32..126; draw a small ring instead */
        if (bg >= 0) for (int j = 0; j < 32; j++) for (int i = 0; i < cell; i++) window_pixel_phys(px + i, py + j, (unsigned int)bg);
        for (int j = 0; j < 9; j++) for (int i = 0; i < 9; i++) { int dx = i - 4, dy = j - 4; int d2 = dx*dx + dy*dy; if (d2 >= 5 && d2 <= 12) window_pixel_phys(px + 3 + i, py + 8 + j, fg); } /* a ring at cap height, where a degree sign sits */
        return;
    }
    g = GUI_AA_GLYPH(c);
    if (bg >= 0) for (int j = 0; j < 32; j++) for (int i = 0; i < cell; i++) window_pixel_phys(px + i, py + j, (unsigned int)bg);
    int ox = px + g->left, oy = py + g->top - 2; /* `top` is measured from the line box's top (see editor_layout), not a baseline; the 24px face was sized for a 36px line box, ours is 32 */
    for (int row = 0; row < g->height; row++){
        for (int col = 0; col < g->width; col++){
            int a = editor_pixels[g->offset + row * g->width + col];
            if (!a) continue;
            int x = ox + col, y = oy + row;
            if (x < px || x >= px + cell) continue; /* keep inside the cell so neighbours never overdraw each other */
            unsigned int d = window_get_pixel_phys(x, y);
            a = text_ink(a, fg, d);
            if (!a) continue;
            unsigned int r = (((fg >> 16) & 0xFF) * a + ((d >> 16) & 0xFF) * (255 - a)) / 255;
            unsigned int gg = (((fg >> 8) & 0xFF) * a + ((d >> 8) & 0xFF) * (255 - a)) / 255;
            unsigned int b = ((fg & 0xFF) * a + (d & 0xFF) * (255 - a)) / 255;
            window_pixel_phys(x, y, (r << 16) | (gg << 8) | b);
        }
    }
}

/* Mono glyph for the two real character grids (terminal, keyrate), now
   drawn through the same runtime-TTF path Notes uses (kernel/ttf_render.h)
   instead of a baked PIL bitmap: DejaVu Sans Mono, rasterized at physical
   resolution with the shared glyph cache and coverage/ink blend. The mono
   face's own advance is genuinely fixed-width, so its rounded ttf_advance
   is used to pick the px size that lands on this cell exactly (computed
   once, cached), then every glyph is left-aligned and clipped to the
   caller's fixed cell the same way the old bitmap path was, keeping
   columns perfectly aligned -- the pitch itself is set by the caller
   (term_render/keyrate advance by a fixed logical step), this only has to
   not spill past its own cell. */
static float gui_aa_mono_px(int cell){
    static int cached_cell = -1;
    static float cached_px = 0;
    if (cell != cached_cell) { cached_px = ttfr_mono_px_for_cell(cell); cached_cell = cell; }
    return cached_px;
}
void gui_aa_char_mono(unsigned char c, int px, int py, unsigned int fg, int bg, int cell){
    if (c < 32 || c > 126) c = '?';
    if (bg >= 0) for (int j = 0; j < 32; j++) for (int i = 0; i < cell; i++) window_pixel_phys(px + i, py + j, (unsigned int)bg);
    float mono_px = gui_aa_mono_px(cell);
    ttf_glyph_t *g = ttfr_glyph(TTF_FACE_MONO, c, mono_px);
    if (!g || !g->coverage) return;
    int base_x = px, base_y = py + ttf_ascent(ttfr_face(TTF_FACE_MONO), mono_px);
    ttfr_blend_glyph(g, base_x, base_y, fg, px, px + cell);
}

/* Physical-resolution text. Everything below draws at physical
   resolution through the same DejaVu Sans coverage glyphs the rest of the
   GUI text uses (editor_glyphs), so the window gets a real size hierarchy:
   16/20/24/28 px faces drawn 1:1, and the hero numeral scaled up from the
   28 px face. No gradient anywhere: flat cream surface, flat cards. */
static const int WX_CAPTOP[4] = {3, 4, 5, 6}; /* line-box top to cap top, per face size, physical px */
static int wx_font_px(int size, int mul){ return (16 + 4 * size) * mul; }
static int wx_char_adv(unsigned char c, int size, int bold, int mul){
    if (c == 0xF8) return wx_font_px(size, mul) * 2 / 5;
    if (c < 32 || c > 126) c = '?';
    return editor_glyphs[((0 * 2 + bold) * 4 + size) * 95 + (c - 32)].advance * mul;
}
/* Width in physical px. */
static int wx_text_w(const char *s, int size, int bold, int mul){
    int w = 0; for (; *s; s++) w += wx_char_adv((unsigned char)*s, size, bold, mul); return w;
}
static void wx_blend(int x, int y, unsigned int fg, int a){
    if (a <= 0) return;
    if (a >= 255) { window_pixel_phys(x, y, fg); return; }
    unsigned int d = window_get_pixel_phys(x, y);
    unsigned int r = (((fg >> 16) & 0xFF) * a + ((d >> 16) & 0xFF) * (255 - a)) / 255;
    unsigned int g = (((fg >> 8) & 0xFF) * a + ((d >> 8) & 0xFF) * (255 - a)) / 255;
    unsigned int b = ((fg & 0xFF) * a + (d & 0xFF) * (255 - a)) / 255;
    window_pixel_phys(x, y, (r << 16) | (g << 8) | b);
}
/* Degree sign: the glyph table stops at 126, so draw a supersampled ring
   sized to the face, sitting at cap height. px,py = cap top left, physical. */
static void wx_degree(int px, int py, int fpx, unsigned int fg){
    int ro = fpx * 13 / 100, ri = fpx * 7 / 100;
    if (ro < 3) ro = 3;
    if (ri >= ro - 1) ri = ro - 2;
    if (ri < 1) ri = 1;
    int cx = px + ro + fpx / 20, cy = py + ro;
    for (int y = -ro - 1; y <= ro + 1; y++) for (int x = -ro - 1; x <= ro + 1; x++) {
        int in = 0;
        for (int sy = 0; sy < 4; sy++) for (int sx = 0; sx < 4; sx++) {
            int ux = x * 8 + sx * 2 - 3, uy = y * 8 + sy * 2 - 3; /* eighths of a pixel */
            int d2 = ux * ux + uy * uy;
            if (d2 <= ro * ro * 64 && d2 >= ri * ri * 64) in++;
        }
        wx_blend(cx + x, cy + y, fg, in * 255 / 16);
    }
}
/* Draws s with its cap top at logical (lx, ly). size 0..3 = 16/20/24/28 px
   face, mul = integer upscale (1 = the face's own pixels). Upscaled glyphs
   are bilinear-sampled from the coverage bitmap and then re-thresholded
   around 50% with a slope of `mul`, which puts a one-pixel anti-aliased
   edge back on the enlarged outline instead of a blur. Returns the width
   in logical px, rounded up. */
static int wx_text(const char *s, int lx, int ly, int size, int bold, int mul, unsigned int fg){
    int sc = (int)window_scale();
    int px = lx * sc, py = ly * sc, x0 = px;
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == 0xF8) { wx_degree(px, py, wx_font_px(size, mul), fg); px += wx_char_adv(c, size, bold, mul); continue; }
        if (c < 32 || c > 126) c = '?';
        const struct editor_glyph *g = &editor_glyphs[((0 * 2 + bold) * 4 + size) * 95 + (c - 32)];
        const unsigned char *src = &editor_pixels[g->offset];
        int ox = px + g->left * mul, oy = py + (g->top - WX_CAPTOP[size]) * mul;
        if (mul == 1) {
            for (int r = 0; r < g->height; r++) for (int q = 0; q < g->width; q++) {
                int a = src[r * g->width + q];
                if (a) wx_blend(ox + q, oy + r, fg, text_ink(a, fg, window_get_pixel_phys(ox + q, oy + r)));
            }
        } else {
            for (int dy = -mul; dy < (g->height + 1) * mul; dy++) for (int dx = -mul; dx < (g->width + 1) * mul; dx++) {
                int u = (dx * 256 + 128) / mul - 128, v = (dy * 256 + 128) / mul - 128; /* source coords, 24.8 */
                int ux = u >> 8, vy = v >> 8, fx = u & 255, fy = v & 255;
                int a00 = (ux >= 0 && ux < g->width && vy >= 0 && vy < g->height) ? src[vy * g->width + ux] : 0;
                int a10 = (ux + 1 >= 0 && ux + 1 < g->width && vy >= 0 && vy < g->height) ? src[vy * g->width + ux + 1] : 0;
                int a01 = (ux >= 0 && ux < g->width && vy + 1 >= 0 && vy + 1 < g->height) ? src[(vy + 1) * g->width + ux] : 0;
                int a11 = (ux + 1 >= 0 && ux + 1 < g->width && vy + 1 >= 0 && vy + 1 < g->height) ? src[(vy + 1) * g->width + ux + 1] : 0;
                int a = ((a00 * (256 - fx) + a10 * fx) * (256 - fy) + (a01 * (256 - fx) + a11 * fx) * fy) >> 16;
                a = 128 + (a - 128) * mul;
                if (a > 255) a = 255;
                wx_blend(ox + dx, oy + dy, fg, a);
            }
        }
        px += g->advance * mul;
    }
    return (px - x0 + sc - 1) / sc;
}
static int wx_text_lw(const char *s, int size, int bold, int mul){ int sc = (int)window_scale(); return (wx_text_w(s, size, bold, mul) + sc - 1) / sc; }

/* gui_paint.h's icon text, through wx_text, the physical-resolution DejaVu path the Weather window uses. The Pi
   draws its window labels with it; nothing on i386 calls it since the Calendar tile became a plain picture. */
void gui_icon_text(const char *s, int lx, int ly, int face, int mul, unsigned int fg){ wx_text(s, lx, ly, face, 1, mul, fg); }
int gui_icon_text_w(const char *s, int face, int mul){ return wx_text_lw(s, face, 1, mul); }

/* "18°" style degrees into out. */
char *wx_put_int(char *o, int v){
    if (v < 0) { *o++ = '-'; v = -v; }
    char d[8]; int n = 0; if (!v) d[n++] = '0'; while (v && n < 7) { d[n++] = (char)('0' + v % 10); v /= 10; }
    while (n) *o++ = d[--n];
    return o;
}
