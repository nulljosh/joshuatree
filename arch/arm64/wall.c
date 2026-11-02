/* The ARM desktop's wallpaper. 8.9.0: on the Pi the default is a picture, Joshua Tree at dusk (park_wall.h, drawn by
   tools/gen/gen_park_wall.py): 480 x 270 in 16 colours, dithered, scaled to the screen with every fourth row dimmed, a
   scanline look. Settings can pick a flat colour instead (settings.c); the QEMU builds start on the flat blue #3B6EA5, so the
   checks that read bare wallpaper see one colour. No decoder, no heap: wall_px works out any pixel from the packed picture. */
#include "park_wall.h"
#define WALL_PARK 0x01000000u   /* settings.c's walls[]: the picture, not a colour */
#ifdef PI_BUILD
static unsigned wall_rgb = WALL_PARK;
#else
static unsigned wall_rgb = 0x003B6EA5u;   /* a calm blue (Joshua: "a nice blue, not grey") */
#endif

static unsigned rb_swap(unsigned c) { return (c & 0xFF) << 16 | (c & 0xFF00) | c >> 16; }   /* red and blue the other way round */
static unsigned dimmed(unsigned c) { return ((c >> 16 & 0xFF) * 4 / 5) << 16 | ((c >> 8 & 0xFF) * 4 / 5) << 8 | (c & 0xFF) * 4 / 5; }   /* the scanline */
/* n pixels of row y from column x into dst, in the screen's byte order: the picture's row and scanline worked out once,
   each pixel a fixed-point step (the drag path repaints wallpaper every frame, so no division per pixel). */
void wall_span(unsigned *dst, int x, int y, int n, int w, int h, int swap) {
    if (!(wall_rgb & WALL_PARK)) {
        unsigned c = swap ? rb_swap(wall_rgb) : wall_rgb;
        for (int i = 0; i < n; i++) dst[i] = c;
        return;
    }
    const unsigned char *row = park_px + (unsigned)y * PARK_H / (unsigned)h * (PARK_W / 2);
    unsigned step = (PARK_W << 16) / (unsigned)w, at = (unsigned)x * step, dim = (y & 3) == 3, last = ~0u, out = 0;
    for (int i = 0; i < n; i++, at += step) {
        unsigned px = at >> 16, k = px & 1 ? row[px / 2] & 15 : row[px / 2] >> 4;
        if (k != last) {   /* runs of one colour are long: convert only on a change */
            unsigned v = dim ? dimmed(park_pal[k]) : park_pal[k];
            out = swap ? rb_swap(v) : v;
            last = k;
        }
        dst[i] = out;
    }
}
unsigned wall_px(int x, int y, int w, int h) {   /* the wallpaper's colour at screen pixel x, y of a w x h screen, 0x00RRGGBB */
    unsigned c;
    wall_span(&c, x, y, 1, w, h, 0);   /* one pixel of wall_span, so the two always agree */
    return c;
}
void wall_set(unsigned rgb) { wall_rgb = rgb & (0x00FFFFFFu | WALL_PARK); }
unsigned wall_get(void) { return wall_rgb; }

/* Paints the whole w x h screen. `swap` says red and blue are the other way round in memory. Always succeeds. */
int wall_paint(unsigned *fb, unsigned pitch, int w, int h, int swap) {
    for (int y = 0; y < h; y++) wall_span(fb + (unsigned long)y * pitch, 0, y, w, w, h, swap);
    return 1;
}
