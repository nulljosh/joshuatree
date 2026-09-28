#ifndef GUI_PRIMS_H
#define GUI_PRIMS_H

/* Small, pure GUI math primitives with no state of their own beyond the
   dither pair table, split out of kernel.c so the color/geometry helpers
   every icon and line drawer leans on aren't buried in the one file. */

/* 1-bit blending. A blend result is a tagged handle (top nibble 0xD), not
   a colour; drivers/window.c resolves it per pixel with a 4x4 Bayer
   ordered dither so the framebuffer only ever holds one of the two source
   colours. Plain 0x00RRGGBB colours never carry the tag. */
#define GUI_DITHER_TAG 0xD0000000u
#define GUI_DITHER_IS(c) (((c) & 0xF0000000u) == GUI_DITHER_TAG)

/* `t/max` of the way from a to b, as a dither handle (or a/b themselves at
   the ends). Every UI blend routes through this one primitive. */
unsigned int gui_dither(unsigned int a, unsigned int b, int t, int max);

/* Resolve a handle at physical pixel (x, y): a or b. Passthrough for a
   plain colour. */
unsigned int gui_dither_pixel(int x, int y, unsigned int c);

/* Same decision with no table entry, for per-pixel loops that already
   know their (x, y) and would otherwise mint one pair per wallpaper sample. */
unsigned int gui_dither_pick(int x, int y, unsigned int a, unsigned int b, int t, int max);

/* The old flat channel lerp, kept ONLY for image resampling (the wallpaper
   scaler, the day/night colour grade): those are photo pixels, not a UI
   grey, and would flood the pair table. Never use it for chrome. */
unsigned int gui_dither_flat(unsigned int a, unsigned int b, int t, int max);

/* Fifty-fifty of two colours (a level-8 dither handle). */
unsigned int gui_blend(unsigned int a, unsigned int b);

/* sqrt() via the x87 fsqrt instruction, used by the line/circle primitives. */
double gui_line_sqrt(double x);

#endif
