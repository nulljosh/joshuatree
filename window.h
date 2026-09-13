#ifndef WINDOW_H
#define WINDOW_H
/* A minimal windowing surface: right now that means exactly one full-screen
   buffer, which the roadmap's own v6 note says counts. This wraps what
   gfxtest/mousetest were already doing with raw pixel writes into named,
   reusable calls instead of each caller reimplementing the same loop. Real
   multiple/overlapping windows are a later problem for whenever something
   actually needs more than one surface on screen at once. */

/* Opens the one full-screen window at the given mode. Returns 1 on success
   (also mapping the framebuffer via paging_map_region), 0 if there's no
   VGA device or no spare page tables. */
int window_open(unsigned int width, unsigned int height, unsigned int bpp);

/* Closes the window, returning to the text-mode shell. */
void window_close(void);

void window_clear(unsigned int color);
void window_pixel(int x, int y, unsigned int color);
void window_rect(int x, int y, int w, int h, unsigned int color);
unsigned int window_width(void);
unsigned int window_height(void);
#endif
