#include "window.h"
#include "vbe.h"
#include "paging.h"

typedef unsigned int u32;

static u32 *fb = 0;
static u32 win_w = 0, win_h = 0;   /* logical size, what every caller lays out against */
static u32 phys_w = 0, scale = 1;  /* v41: physical framebuffer may be an integer multiple */
static int view_x = 0, view_y = 0;
static u32 view_w = 0, view_h = 0;
static u32 *screen_band = 0;
static int screen_band_top = 0;
static u32 screen_band_h = 0;

void window_set_viewport(int x, int y, u32 w, u32 h) { view_x = x; view_y = y; view_w = w; view_h = h; }
void window_clear_viewport(void) { view_w = view_h = 0; view_x = view_y = 0; }
void window_push_screen_band(u32 *buf, int top, u32 h) { screen_band = buf; screen_band_top = top; screen_band_h = h; }
void window_pop_screen_band(void) { screen_band = 0; screen_band_top = 0; screen_band_h = 0; }

static u32 *screen_pixel(int px, int py) {
    if (view_w) {
        if (px < 0 || py < 0 || (u32)px >= view_w * scale || (u32)py >= view_h * scale) return 0;
        px += view_x * (int)scale; py += view_y * (int)scale;
    }
    if (px < 0 || py < 0 || (u32)px >= phys_w || (u32)py >= win_h * scale) return 0;
    if (screen_band) {
        if (py < screen_band_top || (u32)(py - screen_band_top) >= screen_band_h) return 0;
        return &screen_band[(u32)(py - screen_band_top) * phys_w + (u32)px];
    }
    return &fb[(u32)py * phys_w + (u32)px];
}

/* A single redirectable render target, real supersampling for anything
   that wants it (icons, first user: gui_draw_one_icon renders each one
   at a few times its real size into a heap buffer, then box-downsamples
   it back to screen size, real anti-aliasing from oversampling instead
   of AA_BAND's discrete color-stepping, which visibly bands under a
   close zoom no matter how wide the band gets). Not a stack: nothing in
   this kernel needs to nest offscreen targets, one level covers every
   real caller, and a stack would be complexity nothing asks for. */
static u32 *target_fb = 0;
static u32 target_w = 0, target_h = 0;

int window_has_target(void) { return target_fb != 0; }
void window_push_target(u32 *buf, u32 w, u32 h) { target_fb = buf; target_w = w; target_h = h; }
void window_pop_target(void) { target_fb = 0; target_w = 0; target_h = 0; }

/* v41: open a physical mode `s` times larger than the logical one. Every
   existing caller keeps drawing in logical coordinates and window_pixel
   fills an s x s block, so nothing above this file has to know; the only
   thing that changes is that code which *wants* real physical pixels
   (icons) can write them through window_pixel_phys and come out s times
   sharper. This is the honest fix for "big pixels": vector polish had
   hit the 800x600 ceiling, the pixels themselves had to get smaller. */
int window_open_scaled(u32 width, u32 height, u32 bpp, u32 s) {
    u32 addr;
    if (s < 1) s = 1;
    if (!vbe_set_mode(width * s, height * s, bpp, &addr)) return 0;
    if (!paging_map_region(addr, width * s * height * s * (bpp / 8))) { vbe_disable(); return 0; }
    fb = (u32 *)addr;
    win_w = width;
    win_h = height;
    phys_w = width * s;
    scale = s;
    return 1;
}

int window_open(u32 width, u32 height, u32 bpp) {
    return window_open_scaled(width, height, bpp, 1);
}

u32 window_scale(void) { return scale; }

u32 window_get_pixel_phys(int px, int py) {
    u32 *p = screen_pixel(px, py);
    return p ? *p : 0;
}

u32 *window_phys_row(int py) {
    if (view_w || screen_band || py < 0 || (u32)py >= win_h * scale) return 0;
    return fb + (u32)py * phys_w;
}

void window_pixel_phys(int px, int py, u32 color) {
    u32 *p = screen_pixel(px, py);
    if (p) *p = color;
}

void window_close(void) {
    /* v77: unmap the framebuffer region, freeing its page tables for reuse.
       This is critical on the browser demo where the GUI runs and exits
       multiple times per session, or on systems where the heap needs to grow
       for other work after the GUI closes. Without this, every window_open
       consumes MAX_EXTRA_TABLES until all 16 are exhausted and nothing else
       can map new regions. fb is a physical address (valid as virtual due to
       identity mapping), so pass it directly to paging_unmap_region. */
    if (fb) {
        u32 size = phys_w * win_h * scale * 4; /* bytes per pixel = 4 */
        paging_unmap_region((u32)fb, size);
    }
    vbe_disable();
    fb = 0;
    win_w = win_h = 0;
    phys_w = 0; scale = 1;
    window_clear_viewport(); window_pop_screen_band();
}

void window_clear(u32 color) {
    if (view_w) {
        for (u32 y = 0; y < view_h * scale; y++)
            for (u32 x = 0; x < view_w * scale; x++) window_pixel_phys((int)x, (int)y, color);
    } else {
        for (u32 i = 0; i < phys_w * win_h * scale; i++) fb[i] = color;
    }
}

void window_pixel(int x, int y, u32 color) {
    if (target_fb) {
        if (x < 0 || y < 0 || (u32)x >= target_w || (u32)y >= target_h) return;
        target_fb[(u32)y * target_w + (u32)x] = color;
        return;
    }
    if (x < 0 || y < 0 || (u32)x >= (view_w ? view_w : win_w) || (u32)y >= (view_h ? view_h : win_h)) return;
    for (u32 j = 0; j < scale; j++)
        for (u32 i = 0; i < scale; i++) window_pixel_phys(x * (int)scale + (int)i, y * (int)scale + (int)j, color);
}

void window_rect(int x, int y, int w, int h, u32 color) {
    for (int yy = y; yy < y + h; yy++)
        for (int xx = x; xx < x + w; xx++)
            window_pixel(xx, yy, color);
}

/* v40: read-back, so a software cursor can save what it's about to cover
   and put it back exactly, whatever it was (wallpaper, dock, an icon). */
u32 window_get_pixel(int x, int y) {
    if (target_fb) {
        if (x < 0 || y < 0 || (u32)x >= target_w || (u32)y >= target_h) return 0;
        return target_fb[(u32)y * target_w + (u32)x];
    }
    return window_get_pixel_phys(x * (int)scale, y * (int)scale);
}

u32 window_width(void)  { return target_fb ? target_w : (view_w ? view_w : win_w); }
u32 window_height(void) { return target_fb ? target_h : (view_h ? view_h : win_h); }
