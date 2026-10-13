/* The ARM desktop's wallpaper: one flat colour. The default is the brand terracotta #b5502c (docs/DESIGN.md); Settings can pick
   another flat colour (settings.c). The Satellite photo this replaced (3.0.0) decoded a PNG at boot; this writes the screen in
   one pass and needs no decoder, no heap. */
static unsigned wall_rgb = 0x00B5502Cu;

void wall_set(unsigned rgb) { wall_rgb = rgb & 0x00FFFFFFu; }
unsigned wall_get(void) { return wall_rgb; }

/* Paints the whole w x h screen. `swap` says red and blue are the other way round in memory. Always succeeds. */
int wall_paint(unsigned *fb, unsigned pitch, int w, int h, int swap) {
    unsigned c = swap ? ((wall_rgb & 0xFF) << 16 | (wall_rgb & 0xFF00) | wall_rgb >> 16) : wall_rgb;
    for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) fb[(unsigned long)y * pitch + (unsigned)x] = c;
    return 1;
}
