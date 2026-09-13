#include "window.h"
#include "vbe.h"
#include "paging.h"

typedef unsigned int u32;

static u32 *fb = 0;
static u32 win_w = 0, win_h = 0;

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
    if (x < 0 || y < 0 || (u32)x >= win_w || (u32)y >= win_h) return;
    fb[(u32)y * win_w + (u32)x] = color;
}

void window_rect(int x, int y, int w, int h, u32 color) {
    for (int yy = y; yy < y + h; yy++)
        for (int xx = x; xx < x + w; xx++)
            window_pixel(xx, yy, color);
}

u32 window_width(void)  { return win_w; }
u32 window_height(void) { return win_h; }
