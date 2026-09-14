#include "window.h"
#include "vbe.h"
#include "paging.h"

typedef unsigned int u32;

static u32 *fb = 0;
static u32 win_w = 0, win_h = 0;

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

void window_push_target(u32 *buf, u32 w, u32 h) { target_fb = buf; target_w = w; target_h = h; }
void window_pop_target(void) { target_fb = 0; target_w = 0; target_h = 0; }

int window_open(u32 width, u32 height, u32 bpp) {
    u32 addr;
    if (!vbe_set_mode(width, height, bpp, &addr)) return 0;
    if (!paging_map_region(addr, width * height * (bpp / 8))) { vbe_disable(); return 0; }
    fb = (u32 *)addr;
    win_w = width;
    win_h = height;
    return 1;
}

void window_close(void) {
    vbe_disable();
    fb = 0;
    win_w = win_h = 0;
}

void window_clear(u32 color) {
    for (u32 i = 0; i < win_w * win_h; i++) fb[i] = color;
}

void window_pixel(int x, int y, u32 color) {
    if (target_fb) {
        if (x < 0 || y < 0 || (u32)x >= target_w || (u32)y >= target_h) return;
        target_fb[(u32)y * target_w + (u32)x] = color;
        return;
    }
    if (x < 0 || y < 0 || (u32)x >= win_w || (u32)y >= win_h) return;
    fb[(u32)y * win_w + (u32)x] = color;
}

void window_rect(int x, int y, int w, int h, u32 color) {
    for (int yy = y; yy < y + h; yy++)
        for (int xx = x; xx < x + w; xx++)
            window_pixel(xx, yy, color);
}

u32 window_width(void)  { return target_fb ? target_w : win_w; }
u32 window_height(void) { return target_fb ? target_h : win_h; }
