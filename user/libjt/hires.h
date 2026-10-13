#ifndef LIBJT_HIRES_H
#define LIBJT_HIRES_H
/* libjt hi-res drawing (3.9.0). An app that opens its window with jt_window_open_hires() on the ARM64 kernel at scale 2
   gets a buffer twice as wide and tall as the window it lays out in. It keeps drawing in logical pixels; this header
   turns each call into device pixels, so a rectangle edge, a circle and a stem of text are sharp at 2x instead of a 1x
   picture smoothed up. Everything here is header-only static inline, so i386 and every app that never calls it pay
   nothing, and at scale 1 each call is the plain 1x loop it replaced. */
#include "jtsys.h"

/* The window's scale: 2 when the kernel mapped a device-size buffer (pitch in bytes is twice width * 4), else 1. A
   scratch struct whose pitch is not a byte pitch (calendar's small_date) comes out as 1. */
static inline int jt_scale(const struct jt_window_info *w) { return w->pitch == w->width * 8u ? 2 : 1; }

/* Fills the logical rectangle x, y, cw by ch with colour c, clipped to the window. */
static inline void jt_fill(struct jt_window_info *w, int x, int y, int cw, int ch, unsigned c) {
    int k = jt_scale(w), W = (int)w->width, H = (int)w->height;
    if (x < 0) { cw += x; x = 0; }
    if (y < 0) { ch += y; y = 0; }
    if (x + cw > W) cw = W - x;
    if (y + ch > H) ch = H - y;
    unsigned stride = w->width * (unsigned)k;
    for (int yy = 0; yy < ch * k; yy++) {
        unsigned *row = w->pixels + (unsigned)(y * k + yy) * stride + (unsigned)(x * k);
        for (int xx = 0; xx < cw * k; xx++) row[xx] = c;
    }
}

/* The same in device pixels, for a shape the app works out at the window's scale (a slanted edge, a picture): x, y, cw, ch
   count buffer pixels, so at scale 1 it is jt_fill. */
static inline void jt_fill_dev(struct jt_window_info *w, int x, int y, int cw, int ch, unsigned c) {
    int k = jt_scale(w), W = (int)w->width * k, H = (int)w->height * k;
    if (x < 0) { cw += x; x = 0; }
    if (y < 0) { ch += y; y = 0; }
    if (x + cw > W) cw = W - x;
    if (y + ch > H) ch = H - y;
    for (int yy = 0; yy < ch; yy++) {
        unsigned *row = w->pixels + (unsigned)(y + yy) * (unsigned)W + (unsigned)x;
        for (int xx = 0; xx < cw; xx++) row[xx] = c;
    }
}

/* One logical pixel. */
static inline void jt_pixel(struct jt_window_info *w, int x, int y, unsigned c) { jt_fill(w, x, y, 1, 1, c); }

/* A disc of logical radius r about the centre of logical pixel (cx, cy), the pixels with dx * dx + dy * dy <= r * r at
   scale 1. At scale 2 the same footprint (radius r + 0.5 logical, so layout does not move) is drawn at device size with
   4 by 4 samples a device pixel, so the rim is a one pixel ramp. */
static inline void jt_disc(struct jt_window_info *w, int cx, int cy, int r, unsigned c) {
    int k = jt_scale(w);
    if (k == 1) {
        for (int dy = -r; dy <= r; dy++)
            for (int dx = -r; dx <= r; dx++)
                if (dx * dx + dy * dy <= r * r) jt_fill(w, cx + dx, cy + dy, 1, 1, c);
        return;
    }
    int R8 = (2 * r + 1) * 8, mx = 2 * cx + 1, my = 2 * cy + 1;   /* device units of 1/8 pixel; the centre is the middle of the 2x2 */
    unsigned stride = w->width * 2u;
    int W = (int)stride, H = (int)w->height * 2;
    for (int py = 2 * cy - 2 * r - 1; py <= 2 * cy + 2 * r + 2; py++) {
        if (py < 0 || py >= H) continue;
        for (int px = 2 * cx - 2 * r - 1; px <= 2 * cx + 2 * r + 2; px++) {
            if (px < 0 || px >= W) continue;
            int n = 0;
            for (int j = 0; j < 4; j++) for (int i = 0; i < 4; i++) {
                int du = px * 8 + 2 * i + 1 - mx * 8, dv = py * 8 + 2 * j + 1 - my * 8;
                if (du * du + dv * dv <= R8 * R8) n++;
            }
            if (!n) continue;
            unsigned *d = &w->pixels[(unsigned)py * stride + (unsigned)px], o = *d, out = 0;
            for (int sh = 0; sh < 24; sh += 8) {
                int a = (int)(c >> sh & 255), b = (int)(o >> sh & 255);
                out |= (unsigned)(b + (a - b) * n / 16) << sh;
            }
            *d = out;
        }
    }
}

/* n bytes of text through JT_SYS_TEXT2 (the kernel rasterises Tree Sans into the device-size buffer), the pen starting at
   device pixel x with its baseline at device row y and moving by adv[i] device pixels after byte i, 255 bytes a request.
   face 0 body, 1 bold, 2 display, 3 mono; pct is the size as a percent of the face at the window's scale. Returns the
   pen's device x. */
static inline int jt_text2_run(int face, int pct, int x, int y, unsigned rgb, const unsigned char *s, const unsigned char *adv, int n) {
    while (n > 0) {
        int m = n > 255 ? 255 : n;
        struct jt_text2 t = { x, y, rgb, face, pct, m, (unsigned long)s, (unsigned long)adv };
        jt_syscall(JT_SYS_TEXT2, (jt_arg)&t, 0, 0);
        for (int i = 0; i < m; i++) x += adv[i];
        s += m; adv += m; n -= m;
    }
    return x;
}
#endif
