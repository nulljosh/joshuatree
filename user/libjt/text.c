/* libjt antialiased text: the kernel ink curve over the 4-bit glyph atlas. */
#include "text.h"
#include "../../lib/text_ink.h"
#include "aafont.h"

struct face { const struct aa_glyph *g; int n, ascent, height; };

static int face_info(int face, struct face *f) {
    switch (face) {
    case JT_FACE_BOLD:    f->g = aa_BOLD_glyphs;    f->n = AA_BOLD_N;    f->ascent = AA_BOLD_ASCENT;    f->height = AA_BOLD_HEIGHT;    return 1;
    case JT_FACE_DISPLAY: f->g = aa_DISPLAY_glyphs; f->n = AA_DISPLAY_N; f->ascent = AA_DISPLAY_ASCENT; f->height = AA_DISPLAY_HEIGHT; return 2;
    default:              f->g = aa_BODY_glyphs;    f->n = AA_BODY_N;    f->ascent = AA_BODY_ASCENT;    f->height = AA_BODY_HEIGHT;    return 0;
    }
}

/* Glyph for byte c: body and bold run 0x20..0x7E then 0xB0; display is
   0-9, '-', 0xB0, ' ' in that order. Anything else falls back to '?' or space. */
static const struct aa_glyph *find(const struct face *f, int kind, unsigned char c) {
    int i;
    if (kind == 2) {
        i = c >= '0' && c <= '9' ? c - '0' : c == '-' ? 10 : c == 0xB0 ? 11 : 12;
    } else {
        i = c >= 0x20 && c <= 0x7E ? c - 0x20 : c == 0xB0 ? 0x7F - 0x20 : '?' - 0x20;
    }
    return &f->g[i];
}

int jt_text_height(int face) { struct face f; face_info(face, &f); return f.height; }
int jt_text_ascent(int face) { struct face f; face_info(face, &f); return f.ascent; }

int jt_text_width(int face, const char *s) {
    struct face f; int kind = face_info(face, &f), w = 0;
    for (; *s; s++) w += find(&f, kind, (unsigned char)*s)->adv;
    return w;
}

int jt_text_draw(struct jt_window_info *w, int face, int x, int y, unsigned rgb, const char *s) {
    struct face f; int kind = face_info(face, &f);
    int fr = (int)(rgb >> 16 & 255), fg = (int)(rgb >> 8 & 255), fb = (int)(rgb & 255);
    int base = y + f.ascent, W = (int)w->width, H = (int)w->height;
    for (; *s; s++) {
        const struct aa_glyph *g = find(&f, kind, (unsigned char)*s);
        const unsigned char *p = aa_pool + g->off;
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
        x += g->adv;
    }
    return x;
}
