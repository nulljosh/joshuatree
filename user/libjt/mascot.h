/* The mascot in a ring-3 app window, ARM64 build only: Music and Books draw it above their "no songs yet" and "no books
 * yet" words. The art is arch/arm64/mascot_art.h (a 192 px RGBA PNG); the first call decodes it with libjt's png_decode,
 * later calls reuse the pixels, and a PNG that will not decode draws nothing. Each pixel is sampled 2x2 and blended over
 * what the window already shows, so draw it after the background. */
#ifndef JT_MASCOT_H
#define JT_MASCOT_H
#include "../jtsys.h"
#include "hires.h"
#include "../../arch/arm64/mascot_art.h"

int png_decode(const unsigned char *data, unsigned int len, unsigned char **out, unsigned int *w, unsigned int *h, unsigned int *channels);

static unsigned char *jt_mascot_px JT_DATA = 0;
static int jt_mascot_tried JT_DATA = 0;

/* d px square, centred on cx, top edge at top. In a hi-res window (hires.h) it is d * 2 device pixels square, so the 192 px art
   is drawn 1 to 1 at d = 96 instead of being sampled down to d. */
static void jt_mascot_draw(const struct jt_window_info *win, int cx, int top, int d) {
    if (!jt_mascot_tried) {
        unsigned w = 0, h = 0, ch = 0;
        jt_mascot_tried = 1;
        if (png_decode(mascot_png, MASCOT_PNG_LEN, &jt_mascot_px, &w, &h, &ch) || w != MASCOT_PX || h != MASCOT_PX || ch != 4) jt_mascot_px = 0;
    }
    if (!jt_mascot_px) return;
    int k = jt_scale(win), D = d * k, x0 = (cx - d / 2) * k, y0 = top * k, W = (int)win->width * k, H = (int)win->height * k;
    for (int y = 0; y < D; y++) for (int x = 0; x < D; x++) {
        int wx = x0 + x, wy = y0 + y;
        if (wx < 0 || wy < 0 || wx >= W || wy >= H) continue;
        unsigned r = 0, g = 0, b = 0, a = 0;
        for (int j = 0; j < 2; j++) for (int i = 0; i < 2; i++) {
            const unsigned char *q = jt_mascot_px + (((y * 2 + j) * MASCOT_PX / (D * 2)) * MASCOT_PX + (x * 2 + i) * MASCOT_PX / (D * 2)) * 4;
            r += q[0] * q[3]; g += q[1] * q[3]; b += q[2] * q[3]; a += q[3];
        }
        if (!a) continue;
        unsigned ink = ((r / a) << 16) | ((g / a) << 8) | (b / a), al = a / 4;
        unsigned *px = win->pixels + (unsigned)wy * (unsigned)W + (unsigned)wx, bgc = *px, out = 0;
        for (int sh = 0; sh <= 16; sh += 8) out |= ((((ink >> sh) & 0xFF) * al + ((bgc >> sh) & 0xFF) * (255 - al)) / 255) << sh;
        *px = out;
    }
}
#endif
