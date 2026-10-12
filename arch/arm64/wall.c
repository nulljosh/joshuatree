/* The ARM desktop's wallpaper: one flat colour, the calm clay #9c5a44 (docs/DESIGN.md; the terracotta #b5502c is the accent). The Satellite photo it replaced (3.0.0)
   decoded a PNG at boot; this writes the screen in one pass and needs no decoder, no heap. */
#include "wall.h"

/* Paints the whole w x h screen. `swap` says red and blue are the other way round in memory. Always succeeds. */
int wall_paint(unsigned *fb, unsigned pitch, int w, int h, int swap) {
    unsigned c = swap ? ((WALL_RGB & 0xFF) << 16 | (WALL_RGB & 0xFF00) | WALL_RGB >> 16) : WALL_RGB;
    for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) fb[(unsigned long)y * pitch + (unsigned)x] = c;
    return 1;
}
