/* Small flat glyphs for the menu bar and Settings' sidebar: bold shapes, no text, one ink, drawn from a few circles, rounded
   boxes and capsules on a 256 unit square. Each pixel takes 4x4 samples, so the edges are anti-aliased at scale 1 and 2 and
   the glyph sits on any background. #included into main.c (it paints through fb, fb_pitch and fb_color). */
enum { GL_ABOUT, GL_DISPLAY, GL_SOUND, GL_DOCK, GL_CLOCK, GL_POWER };

static int g_circle(int x, int y, int cx, int cy, int r) { int dx = x - cx, dy = y - cy; return dx * dx + dy * dy <= r * r; }
static int g_box(int x, int y, int x0, int y0, int x1, int y1, int r) {   /* a rounded box: [x0,x1) by [y0,y1), corner radius r */
    int cx = x < x0 + r ? x0 + r : x > x1 - 1 - r ? x1 - 1 - r : x, cy = y < y0 + r ? y0 + r : y > y1 - 1 - r ? y1 - 1 - r : y;
    return x >= x0 && x < x1 && y >= y0 && y < y1 && g_circle(x, y, cx, cy, r);
}
static int g_capsule(int x, int y, int x0, int y0, int x1, int y1, int r) {   /* every point within r of the segment */
    long vx = x1 - x0, vy = y1 - y0, l2 = vx * vx + vy * vy, t = (long)(x - x0) * vx + (long)(y - y0) * vy;
    if (t < 0) t = 0;
    if (t > l2) t = l2;
    long dx = l2 ? (long)(x - x0) * l2 - vx * t : (long)(x - x0), dy = l2 ? (long)(y - y0) * l2 - vy * t : (long)(y - y0), d = l2 ? l2 : 1;
    return dx * dx + dy * dy <= (long)r * r * d * d;
}
/* Is the unit-square point (x, y) inside glyph `kind` (level: Sound's waves, 0 muted to 2)? */
static int glyph_in(int kind, int level, int x, int y) {
    switch (kind) {
    case GL_ABOUT:   /* a disc with an i cut out of it */
        return g_circle(x, y, 128, 128, 108) && !g_circle(x, y, 128, 74, 17) && !g_capsule(x, y, 128, 118, 128, 180, 15);
    case GL_DISPLAY: /* a screen on a stand */
        return g_box(x, y, 24, 40, 232, 168, 20) || g_capsule(x, y, 128, 168, 128, 206, 12) || g_capsule(x, y, 84, 214, 172, 214, 12);
    case GL_SOUND: {  /* a speaker, with up to two waves */
        int cone = x >= 84 && x <= 138 && y >= 100 - (x - 84) * 50 / 54 && y <= 156 + (x - 84) * 50 / 54;
        if (g_box(x, y, 28, 100, 92, 156, 10) || cone) return 1;
        for (int k = 1; k <= level; k++) {
            int dx = x - 138, dy = y - 128, d2 = dx * dx + dy * dy, ro = 40 + 38 * k, ri = ro - 22;
            if (dx > 0 && dy <= dx && -dy <= dx && d2 <= ro * ro && d2 >= ri * ri) return 1;
        }
        return 0; }
    case GL_DOCK:    /* a tray with three tiles cut out of it */
        return g_box(x, y, 16, 112, 240, 200, 30) && !g_box(x, y, 40, 134, 90, 178, 10) && !g_box(x, y, 103, 134, 153, 178, 10) && !g_box(x, y, 166, 134, 216, 178, 10);
    case GL_CLOCK:   /* a disc with two hands cut out of it */
        return g_circle(x, y, 128, 128, 108) && !g_capsule(x, y, 128, 128, 128, 62, 12) && !g_capsule(x, y, 128, 128, 172, 152, 12);
    case GL_POWER: { /* a ring open at the top, and a bar through the gap */
        int dx = x - 128, dy = y - 138, d2 = dx * dx + dy * dy;
        if (g_capsule(x, y, 128, 28, 128, 128, 16)) return 1;
        return d2 <= 98 * 98 && d2 >= 66 * 66 && !(dy < 0 && dx > -34 && dx < 34); }
    }
    return 0;
}
/* The glyph in a size x size physical box at (px, py), ink blended over what is there. */
static void glyph_paint(int kind, int level, int px, int py, int size, unsigned ink) {
    if (!fb || size <= 0) return;
    unsigned c = fb_color(ink & 0x00FFFFFFu);
    for (int y = 0; y < size; y++) for (int x = 0; x < size; x++) {
        int hit = 0;
        for (int j = 0; j < 4; j++) for (int i = 0; i < 4; i++) hit += glyph_in(kind, level, ((x * 4 + i) * 2 + 1) * 128 / (size * 4), ((y * 4 + j) * 2 + 1) * 128 / (size * 4));
        if (!hit) continue;
        unsigned *d = fb + (unsigned)(py + y) * fb_pitch + (unsigned)(px + x), o = 0;
        for (int b = 0; b <= 16; b += 8) o |= (((c >> b & 0xFF) * (unsigned)hit + (*d >> b & 0xFF) * (16u - (unsigned)hit)) / 16) << b;
        *d = o;
    }
}
