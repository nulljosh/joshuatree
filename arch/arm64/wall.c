/* The ARM desktop's wallpaper: one flat colour, the brand terracotta #b5502c (docs/DESIGN.md). The Satellite photo it replaced (3.0.0)
   decoded a PNG at boot; this writes the screen in one pass and needs no decoder, no heap. */
#define WALL_RGB 0x00B5502Cu

/* Paints the whole w x h screen. `swap` says red and blue are the other way round in memory. Always succeeds. */
int wall_paint(unsigned *fb, unsigned pitch, int w, int h, int swap) {
    unsigned c = swap ? ((WALL_RGB & 0xFF) << 16 | (WALL_RGB & 0xFF00) | WALL_RGB >> 16) : WALL_RGB;
    for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) fb[(unsigned long)y * pitch + (unsigned)x] = c;
    return 1;
}
