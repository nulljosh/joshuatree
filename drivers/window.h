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
/* Reads back whatever's currently at (x,y), 0 if out of bounds. Lets a
   caller correctly alpha-blend a partial-coverage pixel (real anti-
   aliasing) against whatever's actually on screen there instead of
   guessing a background color, needed for font_draw_char's "transparent"
   mode (bg < 0) where there's no single known background to blend into. */
unsigned int window_get_pixel(int x, int y);
/* v41: see window.c. Logical size stays what window_width/height report;
   the framebuffer is `scale` times bigger in each axis. */
int window_open_scaled(unsigned int width, unsigned int height, unsigned int bpp, unsigned int scale);
unsigned int window_scale(void);
void window_pixel_phys(int px, int py, unsigned int color);
unsigned int window_get_pixel_phys(int px, int py);
unsigned int *window_phys_row(int py);
int window_has_target(void);
void window_rect(int x, int y, int w, int h, unsigned int color);
unsigned int window_width(void);
unsigned int window_height(void);

/* Draw an app into a bounded rectangle of the existing framebuffer. The
   desktop outside it stays visible; coordinates inside are app-local. */
void window_set_viewport(int x, int y, unsigned int w, unsigned int h);
void window_clear_viewport(void);

/* Compose a physical row band offscreen, then present it in one copy. */
void window_push_screen_band(unsigned int *buf, int top, unsigned int h);
void window_pop_screen_band(void);

/* Redirects window_pixel/window_rect/window_width/window_height to an
   offscreen buffer instead of the real screen, real supersampling for
   anything that renders into it (draw bigger, downsample smaller, real
   anti-aliasing from oversampling rather than a fixed-width color-step
   band). One level, not a stack: pop always returns to the real screen,
   nothing in this kernel needs to nest two offscreen targets. */
void window_push_target(unsigned int *buf, unsigned int w, unsigned int h);
void window_pop_target(void);
unsigned int window_get_pixel(int x, int y);
/* v41: see window.c. Logical size stays what window_width/height report;
   the framebuffer is `scale` times bigger in each axis. */
int window_open_scaled(unsigned int width, unsigned int height, unsigned int bpp, unsigned int scale);
unsigned int window_scale(void);
void window_pixel_phys(int px, int py, unsigned int color);
unsigned int window_get_pixel_phys(int px, int py);
int window_has_target(void);
#endif
