/* Toroid: Conway's Game of Life on a torus, the same idea as
   toroid.heyitsmejosh.com, running natively instead of as a one-line card
   from the HTML renderer. Both edges wrap, so a glider that leaves one side
   comes back on the other. Included by kernel.c after wx_put_int, which the
   generation counter reuses.
   ponytail: fixed 10px cells and a static max grid, no zoom or pan. */
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
}

static void gui_launch_toroid(void){
    window_clear(GUI_BG);
    gui_draw_app_titlebar("Toroid");
    tr_w = ((int)window_width() - 2 * TR_PAD) / TR_CELL;
    tr_h = ((int)window_height() - TR_TOP - 44) / TR_CELL;
    if (tr_w > TR_MAXW) tr_w = TR_MAXW;
    if (tr_h > TR_MAXH) tr_h = TR_MAXH;
    if (tr_w < 8) tr_w = 8;
    if (tr_h < 8) tr_h = 8;
    tr_paused = 0;
    tr_reseed();
    tr_draw();
    window_present(); sleep_ticks(5);
    mouse_click_edge_sync();
    for (int frame = 0;; frame++) {
        gui_app_mouse_tick();
        int sc = kbd_pop();
        if (sc >= 0 && !(sc & 0x80)) {
            char c = SC[sc & 0x7F];
            if (c == 27) { gui_close_was_click = 0; return; }
            if (c == ' ') tr_paused = !tr_paused;
            else if (c == 'r') tr_reseed();
            else if (c == 'c') { tr_clear(); tr_paused = 1; }
            tr_draw();
        }
        if (mouse_click_edge()) {
            int cx = (app_cursor_x - app_view_x - TR_PAD) / TR_CELL, cy = (app_cursor_y - app_view_y - TR_TOP) / TR_CELL;
            int inside = app_cursor_x - app_view_x >= TR_PAD && app_cursor_y - app_view_y >= TR_TOP && cx < tr_w && cy < tr_h;
            if (!inside) { gui_close_was_click = 1; return; } /* the titlebar X, or anywhere off the grid, same as every other app */
            tr_a[cy][cx] = !tr_a[cy][cx];
            tr_draw();
        }
        if (!tr_paused && frame % 8 == 0) { tr_step(); tr_draw(); }
        window_present(); sleep_ticks(1);
    }
}
