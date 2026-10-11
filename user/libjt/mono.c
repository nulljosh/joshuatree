/* libjt mono text, see text.h. Same 4-bit coverage blend as text.c, over the
   separate aamono.h atlas (DejaVu Sans Mono, ASCII, one size). Fixed advance. */
#include "text.h"
#include "../../lib/text_ink.h"
#include "aamono.h"

int jt_mono_height(void) { return AA_MONO_HEIGHT; }

int jt_mono_draw(struct jt_window_info *w, int x, int y, unsigned rgb, const char *s) {
    int fr = (int)(rgb >> 16 & 255), fg = (int)(rgb >> 8 & 255), fb = (int)(rgb & 255);
    int base = y + AA_MONO_ASCENT, W = (int)w->width, H = (int)w->height;
    for (; *s; s++) {
        unsigned char ch = (unsigned char)*s;
        const struct aam_glyph *g = &aa_MONO_glyphs[ch >= 0x20 && ch <= 0x7E ? ch - 0x20 : '?' - 0x20];
        const unsigned char *p = aa_mono_pool + g->off;
        for (int r = 0; r < g->h; r++) {
            int py = base + g->yoff + r;
            for (int c = 0; c < g->w; c++) {
                int n = r * g->w + c, v = (p[n >> 1] >> ((n & 1) ? 0 : 4)) & 15;
                int px = x + g->xoff + c;
                if (!v || px < 0 || py < 0 || px >= W || py >= H) continue;
                int a = v * 17;
                a = a > 224 ? 255 : a * 8 / 7;
                unsigned *d = &w->pixels[(unsigned)py * w->width + (unsigned)px];
                unsigned o = *d;
                a = text_ink(a, rgb, o);
                unsigned rr = (unsigned)(fr * a + (int)(o >> 16 & 255) * (255 - a)) / 255;
                unsigned gg = (unsigned)(fg * a + (int)(o >> 8 & 255) * (255 - a)) / 255;
                unsigned bb = (unsigned)(fb * a + (int)(o & 255) * (255 - a)) / 255;
                *d = rr << 16 | gg << 8 | bb;
            }
        }
        x += JT_MONO_ADV;
    }
    return x;
}
