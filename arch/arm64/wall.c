/* The ARM desktop's wallpaper. 8.9.0: on the Pi the default is a picture, Joshua Tree at dusk (park_wall.h, made by
   tools/gen/gen_park_wall.py). 13.27.0: a soft painting (art/wallpaper/park-dusk.jpg) at 960 x 540 in 256 colours, a byte a
   pixel, scaled to the screen; the 16-colour pixel scene and its scanlines are gone. Settings can pick a flat colour instead (settings.c); the QEMU builds start on the flat blue #3B6EA5, so the
   checks that read bare wallpaper see one colour. No decoder, no heap: wall_px works out any pixel from the packed picture. */
#include "park_wall.h"
#define WALL_PARK 0x01000000u   /* settings.c's walls[]: the picture, not a colour */
#if defined(PI_BUILD) || defined(PARK_WALL)   /* 13.22.0: PARK_WALL (KFLAGS=-DPARK_WALL) films the landing clips on the Pi's own picture */
static unsigned wall_rgb = WALL_PARK;
#else
static unsigned wall_rgb = 0x003B6EA5u;   /* a calm blue (Joshua: "a nice blue, not grey") */
#endif

static unsigned rb_swap(unsigned c) { return (c & 0xFF) << 16 | (c & 0xFF00) | c >> 16; }   /* red and blue the other way round */
/* n pixels of row y from column x into dst, in the screen's byte order: the picture's row and scanline worked out once,
   each pixel a fixed-point step (the drag path repaints wallpaper every frame, so no division per pixel). */
void wall_span(unsigned *dst, int x, int y, int n, int w, int h, int swap) {
    if (!(wall_rgb & WALL_PARK)) {
        unsigned c = swap ? rb_swap(wall_rgb) : wall_rgb;
        for (int i = 0; i < n; i++) dst[i] = c;
        return;
    }
    static unsigned lut[2][256], made;   /* 13.27.0: the painting's 256 colours in both byte orders, worked out once (the drag
                                            path repaints wallpaper every frame); no scanline now, the picture is smooth */
    if (!made) { for (int k = 0; k < 256; k++) { lut[0][k] = park_pal[k]; lut[1][k] = rb_swap(park_pal[k]); } made = 1; }
    const unsigned char *row = park_px + (unsigned)y * PARK_H / (unsigned)h * PARK_W;
    const unsigned *pal = lut[swap ? 1 : 0];
    unsigned step = (PARK_W << 16) / (unsigned)w, at = (unsigned)x * step;
    for (int i = 0; i < n; i++, at += step) dst[i] = pal[row[at >> 16]];
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
