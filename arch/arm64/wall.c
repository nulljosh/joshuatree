/* The ARM desktop's wallpaper: the baked-in Satellite photo (kernel/wall_sat.h, the same PNG the i386 desktop uses),
   decoded once with drivers/png.c and scaled straight into the framebuffer at the real screen size. Integer only.
   The decode is wrapped in heap_mark and heap_release because the bump heap never frees: only the scaled pixels
   survive, and those live in the framebuffer, not on the heap. */
#include "../../drivers/png.h"
#include "../../kernel/wall_sat.h"
unsigned long heap_mark(void);
void heap_release(unsigned long mark);

/* Paints the whole w x h screen with the photo, covering it (centre crop when the aspect differs) and blending
   bilinearly, so 1920x1080 is an exact 2x of the 960x540 photo. `swap` says red and blue are the other way round in
   memory. Returns 1 on success, 0 when the PNG would not decode (the caller keeps its flat fill). */
int wall_paint(unsigned *fb, unsigned pitch, int w, int h, int swap) {
    unsigned long mark = heap_mark();
    unsigned char *rgb = 0;
    unsigned sw = 0, sh = 0, ch = 0;
    if (png_decode(wall_sat_png, WALL_SAT_PNG_LEN, &rgb, &sw, &sh, &ch) != 0 || !rgb || ch != 3 || !sw || !sh) { heap_release(mark); return 0; }
    /* source pixels per screen pixel, 16.16: the smaller screen-to-photo ratio inverted, so the photo covers the screen */
    unsigned long long rx = ((unsigned long long)sw << 16) / (unsigned)w, ry = ((unsigned long long)sh << 16) / (unsigned)h;
    unsigned long long step = rx < ry ? rx : ry;
    long long ox = (((long long)sw << 16) - (long long)step * w) / 2, oy = (((long long)sh << 16) - (long long)step * h) / 2;
    for (int y = 0; y < h; y++) {
        long long fy = oy + (long long)step * y + (long long)step / 2 - 0x8000;   /* pixel centres line up */
        if (fy < 0) fy = 0;
        unsigned y0 = (unsigned)(fy >> 16), wy = (unsigned)(fy >> 8) & 0xFF;
        if (y0 >= sh) { y0 = sh - 1; wy = 0; }
        unsigned y1 = y0 + 1 < sh ? y0 + 1 : y0;
        const unsigned char *r0 = rgb + (unsigned long)y0 * sw * 3, *r1 = rgb + (unsigned long)y1 * sw * 3;
        for (int x = 0; x < w; x++) {
            long long fx = ox + (long long)step * x + (long long)step / 2 - 0x8000;
            if (fx < 0) fx = 0;
            unsigned x0 = (unsigned)(fx >> 16), wx = (unsigned)(fx >> 8) & 0xFF;
            if (x0 >= sw) { x0 = sw - 1; wx = 0; }
            unsigned x1 = x0 + 1 < sw ? x0 + 1 : x0;
            unsigned c[3];
            for (int k = 0; k < 3; k++) {
                unsigned top = r0[x0 * 3 + k] * (256 - wx) + r0[x1 * 3 + k] * wx;
                unsigned bot = r1[x0 * 3 + k] * (256 - wx) + r1[x1 * 3 + k] * wx;
                c[k] = (top * (256 - wy) + bot * wy) >> 16;
            }
            fb[(unsigned long)y * pitch + (unsigned)x] = swap ? (c[2] << 16 | c[1] << 8 | c[0]) : (c[0] << 16 | c[1] << 8 | c[2]);
        }
    }
    heap_release(mark);
    return 1;
}
