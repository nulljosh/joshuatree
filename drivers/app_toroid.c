/* Toroid, Conway's Game of Life on a torus, in its own translation unit:
   the second app moved out of kernel.c, same pattern as app_keyrate.c.
   Both edges wrap, so a glider that leaves one side comes back on the
   other. Needs the window and font drivers, ticks() for the seed, and
   what app.h declares (get_key_or_click_until stands in for the raw
   kbd_pop/mouse-tick loop kernel.c used to run by hand here).
   ponytail: fixed 10px cells and a static max grid, no zoom or pan. */
#include "app.h"
#include "toroid.h"
#include "window.h"
#include "font.h"
#include "mouse.h"
#include "irq.h"

#define TR_MAXW 160
#define TR_MAXH 80
#define TR_CELL 10
#define TR_TOP  48
#define TR_PAD  20

static unsigned char tr_a[TR_MAXH][TR_MAXW], tr_b[TR_MAXH][TR_MAXW];
static int tr_w, tr_h, tr_gen, tr_paused;
static unsigned int tr_seed = 2463534242u;
static unsigned int tr_rand(void){ tr_seed ^= tr_seed << 13; tr_seed ^= tr_seed >> 17; tr_seed ^= tr_seed << 5; return tr_seed; }

static void tr_reseed(void){
    for (int y = 0; y < tr_h; y++) for (int x = 0; x < tr_w; x++) tr_a[y][x] = (tr_rand() % 4) == 0;
    tr_gen = 0;
}
static void tr_clear(void){
    for (int y = 0; y < tr_h; y++) for (int x = 0; x < tr_w; x++) tr_a[y][x] = 0;
    tr_gen = 0;
}
static void tr_step(void){
    for (int y = 0; y < tr_h; y++) {
        int yu = (y + tr_h - 1) % tr_h, yd = (y + 1) % tr_h;
        for (int x = 0; x < tr_w; x++) {
            int xl = (x + tr_w - 1) % tr_w, xr = (x + 1) % tr_w;
            int n = tr_a[yu][xl] + tr_a[yu][x] + tr_a[yu][xr] + tr_a[y][xl] + tr_a[y][xr] + tr_a[yd][xl] + tr_a[yd][x] + tr_a[yd][xr];
            tr_b[y][x] = (unsigned char)(n == 3 || (n == 2 && tr_a[y][x]));
        }
    }
    for (int y = 0; y < tr_h; y++) for (int x = 0; x < tr_w; x++) tr_a[y][x] = tr_b[y][x];
    tr_gen++;
}
static void tr_draw(void){
    window_rect(TR_PAD, TR_TOP, tr_w * TR_CELL, tr_h * TR_CELL, 0x00F1EDE7);
    for (int y = 0; y < tr_h; y++) for (int x = 0; x < tr_w; x++)
        if (tr_a[y][x]) window_rect(TR_PAD + x * TR_CELL + 1, TR_TOP + y * TR_CELL + 1, TR_CELL - 2, TR_CELL - 2, 0x001C1C1E);
    char line[96]; char *o = line; const char *s;
    for (s = "generation "; *s; s++) *o++ = *s;
    o = wx_put_int(o, tr_gen);
    for (s = tr_paused ? "   paused" : "         "; *s; s++) *o++ = *s;
    for (s = "   space pause  r reseed  c clear  click a cell  esc closes"; *s; s++) *o++ = *s;
    *o = 0;
    int ly = (int)window_height() - 30;
    window_rect(TR_PAD, ly - 2, (int)window_width() - 2 * TR_PAD, 20, GUI_BG);
    font_draw_string(line, TR_PAD, ly, 0x0075726E, -1);
    window_present();
}

void toroid_open(void){
    app_begin("Toroid", GUI_BG);
    tr_w = ((int)window_width() - 2 * TR_PAD) / TR_CELL;
    tr_h = ((int)window_height() - TR_TOP - 44) / TR_CELL;
    if (tr_w > TR_MAXW) tr_w = TR_MAXW;
    if (tr_h > TR_MAXH) tr_h = TR_MAXH;
    if (tr_w < 8) tr_w = 8;
    if (tr_h < 8) tr_h = 8;
    tr_paused = 0;
    tr_reseed();
    tr_draw();
    mouse_click_edge_sync();
    for (;;) {
        /* Paused: block for real input, same as any other app. Running:
           time out every 8 ticks and step, same cadence the old frame-
           counted loop kept, but a real keypress or click still answers
           immediately instead of waiting out the window. */
        int k = get_key_or_click_until(tr_paused ? 0 : ticks() + 8);
        if (k == KEY_ESC) return;
        if (k == KEY_CLICK) {
            int cx = (app_cursor_x - app_view_x - TR_PAD) / TR_CELL, cy = (app_cursor_y - app_view_y - TR_TOP) / TR_CELL;
            if (cx < 0 || cy < 0 || cx >= tr_w || cy >= tr_h) return; /* titlebar X, or off the grid, same as every other app */
            tr_a[cy][cx] = !tr_a[cy][cx];
        } else if (k == ' ') tr_paused = !tr_paused;
        else if (k == 'r') tr_reseed();
        else if (k == 'c') { tr_clear(); tr_paused = 1; }
        else if (k == 0 && !tr_paused) tr_step();
        tr_draw();
    }
}
