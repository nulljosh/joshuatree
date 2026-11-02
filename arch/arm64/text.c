/* Smooth text for the ARM build: the same DejaVu faces and the same rasterizer (drivers/ttf.c) the i386 desktop uses,
   blended onto the framebuffer. This file is built with the floating point unit allowed; main.c is not, so everything
   crossing the boundary is an integer (sizes in tenths of a pixel). Each glyph is rasterized into the heap, drawn, then
   the heap is rolled back: the bump heap never frees, and a log of a few hundred characters would eat it. */
#include "../../drivers/ttf.h"
#include "../../lib/text_ink.h"   /* the coverage curve i386 and libjt use, so the Pi's text is as dense and sharp as theirs */
unsigned long heap_mark(void);
void heap_release(unsigned long mark);
void uart_puts(const char *s);

static ttf_font_t *face[3];   /* 0 mono, 1 sans bold, 2 sans */
#include "world_font.h"           /* 9.2.0: the faces past Latin-1, for lang.c's words (tools/gen/gen_world_font.py) */
#define WORLD_N (int)(sizeof world_faces / sizeof world_faces[0])
static ttf_font_t *world[WORLD_N];

/* 9.2.0: one character of UTF-8. A byte that does not start a whole sequence is taken as Latin-1, as every string was before. */
static unsigned utf8_next(const char **sp) {
    const unsigned char *s = (const unsigned char *)*sp;
    unsigned c = s[0], n = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC2 ? 1 : 0;
    for (unsigned i = 1; i <= n; i++) if ((s[i] & 0xC0) != 0x80) n = 0;
    if (!n) { *sp += 1; return c; }
    unsigned cp = c & (0x3F >> n);
    for (unsigned i = 1; i <= n; i++) cp = cp << 6 | (s[i] & 0x3F);
    *sp += n + 1;
    return cp;
}
static ttf_font_t *face_for(int which, unsigned cp) {   /* the face asked for, else the first world face with the glyph */
    if (cp < 0x100 || ttf_has(face[which], cp)) return face[which];
    for (int i = 0; i < WORLD_N; i++) if (ttf_has(world[i], cp)) return world[i];
    return face[which];
}

int text_init(void) {
    face[0] = ttf_load_face(TTF_FACE_MONO);
    face[1] = ttf_load_face(TTF_FACE_SANS_BOLD);
    face[2] = ttf_load_face(TTF_FACE_SANS);
    for (int i = 0; i < WORLD_N; i++) world[i] = ttf_load(world_faces[i]);
    { extern void lang_selftest(void) __attribute__((weak)); if (lang_selftest) lang_selftest(); }   /* lang.c, where it is linked */
    int ok = (face[0] != 0) + (face[1] != 0) + (face[2] != 0);   /* 4.27.0: the i386 boot's "fonts N of N ok" line, for the Pi's three faces */
    char line[] = "fonts 0 of 3 ok\n"; line[6] = (char)('0' + ok);
    uart_puts(line);
    return ok == 3;
}

/* Draws one string with its baseline at `baseline`, returns the pen x after it. `fg` is already in the framebuffer's
   byte order; each of its three colour lanes is blended on its own, so the order does not matter here. */
int text_draw(int which, const char *s, int x, int baseline, int px10, unsigned fg, unsigned *fb, unsigned pitch, int w, int h) {
    ttf_font_t *f = face[which];
    if (!f) return x;
    float px = (float)px10 / 10.0f;
    while (*s) {
        unsigned cp = utf8_next(&s);
        unsigned long m = heap_mark();
        ttf_glyph_t g;
        if (ttf_glyph(face_for(which, cp), cp, px, &g) == 0) {
            for (int j = 0; j < g.height; j++) {
                int yy = baseline + g.yoff + j;
                if (yy < 0 || yy >= h) continue;
                for (int i = 0; i < g.width; i++) {
                    int xx = x + g.xoff + i;
                    unsigned a = g.coverage[j * g.width + i];
                    if (!a || xx < 0 || xx >= w) continue;
                    unsigned d = fb[yy * pitch + xx], o = 0;
                    a = (unsigned)text_ink((int)a, fg, d);
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

int text_chars(const char *s) { int n = 0; while (*s) { utf8_next(&s); n++; } return n; }   /* 9.2.0: characters, not bytes */
int text_covers(const char *s) {   /* 9.2.0: every character of s has a glyph in Tree Sans or a world face */
    while (*s) { unsigned cp = utf8_next(&s); if (cp >= 0x100 && !ttf_has(face_for(2, cp), cp)) return 0; }
    return 1;
}

int text_width(int which, const char *s, int px10) {   /* pen advance of a whole string, for centring */
    ttf_font_t *f = face[which];
    int w = 0;
    if (f) while (*s) { unsigned cp = utf8_next(&s); w += ttf_advance(face_for(which, cp), cp, (float)px10 / 10.0f); }
    return w;
}

/* Rasterizes 'M' in the console face and checks the result looks like an M: a sane box and advance for the size, ink
   in nearly every column and a fair share of the box covered. Answers the measurements too, for the boot log. */
int text_selftest(int px10, int *w, int *h, int *adv) {
    *w = *h = *adv = 0;
    if (!face[0]) return 0;
    float px = (float)px10 / 10.0f;
    unsigned long m = heap_mark();
    ttf_glyph_t g;
    int ok = 0;
    if (ttf_glyph(face[0], 'M', px, &g) == 0) {
        *w = g.width; *h = g.height; *adv = g.advance;
        int ink = 0, cols = 0;
        for (int i = 0; i < g.width; i++) {
            int any = 0;
            for (int j = 0; j < g.height; j++) if (g.coverage[j * g.width + i] > 64) { ink++; any = 1; }
            cols += any;
        }
        ok = g.width >= px * 0.35f && g.width <= px && g.height >= px * 0.5f && g.height <= px
          && g.advance >= px * 0.45f && g.advance <= px * 0.75f && cols >= g.width - 2 && ink * 5 >= g.width * g.height;
    }
    heap_release(m);
    return ok;
}
