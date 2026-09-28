#include "gui_prims.h"

/* 1-bit blending. This framebuffer has no alpha channel, and a real
   mid-tone would break the engraved black-ink-on-cream look anyway, so a
   blend here is never a new colour. It is a pair {a, b} plus a coverage
   level 0..16, and the pixel writers in drivers/window.c decide per pixel
   which of the two to lay down by comparing that level against a 4x4
   Bayer cell. At a distance the pattern reads as the intended grey; up
   close every pixel is genuinely one of the two source colours.

   A blend result has to travel through the same unsigned int every fill
   primitive already takes, so it is a handle: top nibble 0xD, low bits an
   index into a small pair table (heap, not BSS: the ring-3 window sits
   16KB past the end of BSS). Plain 0x00RRGGBB colours never carry the tag
   and pass through untouched. */

extern void *kmalloc(unsigned int size);

#define DITHER_SLOTS 4096u
struct dither_pair { unsigned int a, b; unsigned char level, used; };
static struct dither_pair *pairs = 0;
static unsigned int pairs_live = 0;

/* Classic 4x4 Bayer matrix, thresholds 0..15. */
static const unsigned char bayer4[4][4] = {
    {  0,  8,  2, 10 },
    { 12,  4, 14,  6 },
    {  3, 11,  1,  9 },
    { 15,  7, 13,  5 },
};

unsigned int gui_dither_pick(int x, int y, unsigned int a, unsigned int b, int t, int max){
    if (max <= 0 || t <= 0) return a;
    if (t >= max) return b;
    int level = (t * 16 + max / 2) / max; /* 0..16 */
    return level > bayer4[y & 3][x & 3] ? b : a;
}

unsigned int gui_dither_flat(unsigned int a, unsigned int b, int t, int max){
    int ar = (int)((a >> 16) & 0xFF), ag = (int)((a >> 8) & 0xFF), ab = (int)(a & 0xFF);
    int br = (int)((b >> 16) & 0xFF), bg2 = (int)((b >> 8) & 0xFF), bb = (int)(b & 0xFF);
    if (max <= 0) return a;
    int r = ar + (br - ar) * t / max;
    int g = ag + (bg2 - ag) * t / max;
    int bl = ab + (bb - ab) * t / max;
    if (r < 0) r = 0;
    if (r > 255) r = 255;
    if (g < 0) g = 0;
    if (g > 255) g = 255;
    if (bl < 0) bl = 0;
    if (bl > 255) bl = 255;
    return ((unsigned int)r << 16) | ((unsigned int)g << 8) | (unsigned int)bl;
}

/* A handle used as an endpoint of another blend (icon shading does this:
   gui_blend(gui_blend(bg, black), bg)). Two pairs cannot nest in one
   handle, so collapse the inner one to whichever endpoint carries more
   weight; the outer blend then dithers against a real colour. */
static unsigned int dither_endpoint(unsigned int c){
    if (!GUI_DITHER_IS(c) || !pairs) return c;
    struct dither_pair *p = &pairs[c & (DITHER_SLOTS - 1)];
    if (!p->used) return 0;
    return p->level >= 8 ? p->b : p->a;
}

unsigned int gui_dither(unsigned int a, unsigned int b, int t, int max){
    a = dither_endpoint(a); b = dither_endpoint(b);
    if (max <= 0 || t <= 0 || a == b) return a;
    if (t >= max) return b;
    int level = (t * 16 + max / 2) / max;
    if (level <= 0) return a;
    if (level >= 16) return b;
    if (!pairs) {
        pairs = (struct dither_pair *)kmalloc(DITHER_SLOTS * sizeof(struct dither_pair));
        if (!pairs) return gui_dither_flat(a, b, t, max);
        for (unsigned int i = 0; i < DITHER_SLOTS; i++) pairs[i].used = 0;
    }
    unsigned int h = (a * 2654435761u) ^ (b * 40503u) ^ ((unsigned int)level * 97u);
    for (unsigned int probe = 0; probe < 32; probe++) {
        unsigned int i = (h + probe) & (DITHER_SLOTS - 1);
        struct dither_pair *p = &pairs[i];
        if (!p->used) {
            /* table nearly full: a flat tone beats clobbering a live handle */
            if (pairs_live + 64 >= DITHER_SLOTS) return gui_dither_flat(a, b, t, max);
            p->a = a; p->b = b; p->level = (unsigned char)level; p->used = 1; pairs_live++;
            return GUI_DITHER_TAG | i;
        }
        if (p->a == a && p->b == b && p->level == level) return GUI_DITHER_TAG | i;
    }
    return gui_dither_flat(a, b, t, max);
}

unsigned int gui_dither_pixel(int x, int y, unsigned int c){
    if (!GUI_DITHER_IS(c)) return c;
    if (!pairs) return 0;
    struct dither_pair *p = &pairs[c & (DITHER_SLOTS - 1)];
    if (!p->used) return 0;
    return p->level > bayer4[y & 3][x & 3] ? p->b : p->a;
}

/* Fifty-fifty of two colours: a level-8 dither, never a mid tone. */
unsigned int gui_blend(unsigned int a, unsigned int b){
    return gui_dither(a, b, 1, 2);
}

double gui_line_sqrt(double x){ double r; __asm__ volatile ("fsqrt" : "=t"(r) : "0"(x)); return r; }
