/* Smooth text for the ARM build: the same DejaVu faces and the same rasterizer (drivers/ttf.c) the i386 desktop uses,
   blended onto the framebuffer. This file is built with the floating point unit allowed; main.c is not, so everything
   crossing the boundary is an integer (sizes in tenths of a pixel). Each glyph is rasterized into the heap, drawn, then
   the heap is rolled back: the bump heap never frees, and a log of a few hundred characters would eat it. */
#include "../../drivers/ttf.h"
unsigned long heap_mark(void);
void heap_release(unsigned long mark);

static ttf_font_t *face[3];   /* 0 mono, 1 sans bold, 2 sans */

int text_init(void) {
    face[0] = ttf_load_face(TTF_FACE_MONO);
    face[1] = ttf_load_face(TTF_FACE_SANS_BOLD);
    face[2] = ttf_load_face(TTF_FACE_SANS);
    return face[0] && face[1] && face[2];
}

/* Draws one string with its baseline at `baseline`, returns the pen x after it. `fg` is already in the framebuffer's
   byte order; each of its three colour lanes is blended on its own, so the order does not matter here. */
int text_draw(int which, const char *s, int x, int baseline, int px10, unsigned fg, unsigned *fb, unsigned pitch, int w, int h) {
    ttf_font_t *f = face[which];
    if (!f) return x;
    float px = (float)px10 / 10.0f;
    for (; *s; s++) {
        unsigned long m = heap_mark();
        ttf_glyph_t g;
        if (ttf_glyph(f, (unsigned char)*s, px, &g) == 0) {
            for (int j = 0; j < g.height; j++) {
                int yy = baseline + g.yoff + j;
                if (yy < 0 || yy >= h) continue;
                for (int i = 0; i < g.width; i++) {
                    int xx = x + g.xoff + i;
                    unsigned a = g.coverage[j * g.width + i];
                    if (!a || xx < 0 || xx >= w) continue;
                    unsigned d = fb[yy * pitch + xx], o = 0;
                    for (int sh = 0; sh < 24; sh += 8) {
                        int dl = (int)(d >> sh & 0xFF), fl = (int)(fg >> sh & 0xFF);
                        o |= (unsigned)(dl + (fl - dl) * (int)a / 255) << sh;
                    }
                    fb[yy * pitch + xx] = o;
                }
            }
            x += g.advance;
        }
        heap_release(m);
    }
    return x;
}
