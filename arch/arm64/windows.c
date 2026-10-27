/* windows.c: cut out of main.c (the god-file split). Window drag and resize for every window the desktop draws, and pointer clicks routed to them. Built as part of main.o (the Makefile's MAINSRC) with
   main.c's own flags; what it shares with main.c is declared in kmain.h. */
#include "kmain.h"
/* ---- Window drag and resize. Every window the desktop draws (the Console, the Terminal, Clock, Brick and the Browser in one
   rectangle win_lx..win_lh; the Calculator; an app's frame) is one target of the same path: a press picks the front window and
   what the pointer took hold of, pointer_moved turns the pointer into a new rectangle, and win_set applies it. A press on the
   title band (not on a dot) moves the window, the grab offset kept. A press within RS_ZONE = 12 px of the left, right or
   bottom edge (the bottom-right corner is both) below the title band resizes it, the grabbed edge following the pointer and the
   opposite one staying. The size is held between a minimum (win_limits) and the strip between the menu bar and the dock's
   band, the position inside the screen and that strip, so no band is ever under a window. A move of the Console family copies
   its pixels (the contents come along as they are, the uncovered part becomes wallpaper, the Console's text origin shifts);
   anything else, a resize or a move of the Calculator or an app, puts wallpaper where the window was, draws the Console behind
   whole if the old place met it, then draws the window again at the new rectangle, so it lays its content out to the new
   size: the Console and Terminal re-fit their columns and rows, the Browser re-wraps, the Clock and Brick re-centre, the
   Calculator re-spaces its keys, and an app gets JT_EV_RESIZE (the same event i386's compositor sends), answers it with a
   second JT_SYS_WINDOW_OPEN and redraws. The left button going up ends it. The Console is not moved while the Calculator or
   an app is open (it is behind: there is no stacking to repaint). ---- */
#define WT_MAIN 0
#define WT_CALC 1
#define WT_APP  2
#define RS_L 1
#define RS_R 2
#define RS_B 4
#define RS_ZONE 12   /* logical px inside an edge that take hold of it */
#define MIN_WIN_W 320   /* the smallest the Console, Terminal, Browser and Clock shrink to (Brick keeps its first size) */
#define MIN_WIN_H 220
#define MIN_CALC_W 220
#define MIN_CALC_H 260
#define MIN_APP_W 400
#define MIN_APP_H 300
#define GRAB_TITLE (-1)
int in_box(int x, int y, int bx, int by, int bw, int bh);
void clock_paint(void);
void brick_paint(void);
void browser_paint(int report);
int win_def[3][2];   /* each window's size when the desktop drew it first: the Console family's, the Calculator's, an app's */
static int *win_rc(int t, int i) {   /* window t's x, y, w, h (i = 0..3); the app's are the front app's, so no table can hold their addresses */
    static int *const a[2][4] = { { &win_lx, &win_ly, &win_lw, &win_lh }, { &calc_lx, &calc_ly, &calc_lw, &calc_lh } };
    int *b[4] = { &app_lx, &app_ly, &app_lw, &app_lh };
    return t == WT_APP ? b[i] : a[t][i];
}
int drag_on, drag_dx, drag_dy;   /* a title-band press is moving the window; where inside it the pointer took hold, logical px */
static int rs_on, rs_dx, rs_dy;         /* an edge press is resizing it: the edges taken (RS_*), and how far the pointer was past the right and bottom edges */
static int drag_t;                      /* which window (WT_*) either is acting on */
int win_sizing;                  /* win_set is redrawing a window at a new size: the Clock keeps its log line for the minute */
static int win_target(void) {   /* the window that takes a drag: the one in front, -1 for none */
    if (spot_live) return -1;
    if (brick_live || clock_live || browser_live || settings_live) return WT_MAIN;
    if (app_wbytes && app_front) return WT_APP;
    if (calc_live && calc_front) return WT_CALC;
    if (con_live && !calc_live && !app_wbytes) return WT_MAIN;
    return -1;
}
static void win_limits(int t, int *minw, int *minh) {   /* a window opens at its default size and never has to be larger than that to fit its content: the Brick field has a fixed size, the rest shrink to a floor */
    int dw = win_def[t][0], dh = win_def[t][1], fw = t == WT_APP ? MIN_APP_W : t == WT_CALC ? MIN_CALC_W : MIN_WIN_W, fh = t == WT_APP ? MIN_APP_H : t == WT_CALC ? MIN_CALC_H : MIN_WIN_H;
    if (t == WT_MAIN && brick_live) { fw = dw; fh = dh; }
    *minw = fw < dw ? fw : dw; *minh = fh < dh ? fh : dh;
}
static int win_grab(int t, int x, int y) {   /* what a press at (x, y) takes hold of: GRAB_TITLE, a mask of RS_* edges, or 0 for the window's inside */
    int wx = *win_rc(t, 0), wy = *win_rc(t, 1), ww = *win_rc(t, 2), wh = *win_rc(t, 3);
    if (!in_box(x, y, wx, wy, ww, wh)) return 0;
    if (y < wy + 30) {   /* the title band, but not the three dots (radius 7, a pixel spare) */
        for (int d = 0; d < 3; d++) { int dx = x - (wx + 24 + 22 * d), dy = y - (wy + 16); if (dx * dx + dy * dy <= 8 * 8) return 0; }
        return GRAB_TITLE;
    }
    return (x < wx + RS_ZONE ? RS_L : 0) | (x >= wx + ww - RS_ZONE ? RS_R : 0) | (y >= wy + wh - RS_ZONE ? RS_B : 0);
}
static void app_note_resize(void) {   /* JT_EV_RESIZE for the app; one waiting event is updated rather than a second queued, so a long drag asks once for the size it ends on */
    if (!app_cur) return;
    for (unsigned i = app_qh; i != app_qt; i++) if (app_q[i % APP_EVQ].kind == 4) { app_q[i % APP_EVQ].a = (int)app_cw; app_q[i % APP_EVQ].b = (int)app_ch; app_kick = 1; return; }
    app_push(4, (int)app_cw, (int)app_ch);
}
/* 4.3.0: the damage of a moved or resized Calculator or app window, physical px x0..x1 by y0..y1 (the old and new rectangles'
   union), put together from the bottom up: the wallpaper (and the line over the dock when it is that low), the Console if it
   is open behind, then the Calculator or every app window that meets the union, back to front. With the back buffer it is
   drawn on the scratch screen, where each window may be drawn whole (its corners blend over whatever is there, and outside the
   union that is never looked at), and only the union is copied into the back buffer. So nothing outside it changes, a window
   behind comes back where the moved one uncovered it, and no corner is blended over itself twice. */
static int box_meets(int x0, int y0, int x1, int y1, int lx, int ly, int lw, int lh) {
    int s = (int)window_scale();
    return x0 < (lx + lw) * s && lx * s < x1 && y0 < (ly + lh) * s && ly * s < y1;
}
static void desk_compose(int x0, int y0, int x1, int y1) {
    int s = (int)window_scale();
    unsigned *keep = fb;
    if (fb_scan) fb = (unsigned *)(FB_RAM_BASE + FB_RAM_MAX);
    fb_mute++;
    wall_fill(x0, y0, x1 - x0, y1 - y0);
    if (text_ok && y1 > (gui_dock_band_top() - 40) * s) tribute_paint();
    if (con_live && box_meets(x0, y0, x1, y1, win_lx, win_ly, win_lw, win_lh)) { int cf = calc_front, af = app_front; calc_front = app_front = 0; pane_draw(); calc_front = cf; app_front = af; }
    if (calc_live && box_meets(x0, y0, x1, y1, calc_lx, calc_ly, calc_lw, calc_lh)) calc_paint();
    struct app *k = ap;
    for (int z = (int)app_zn - 1; z >= 0; z--) { ap = &apps[app_z[z]]; if (app_wbytes && box_meets(x0, y0, x1, y1, app_lx, app_ly, app_lw, app_lh)) app_paint(); }
    ap = k;
    fb_mute--;
    if (fb != keep) {
        for (int j = y0; j < y1; j++) for (int i = x0; i < x1; i++) keep[(unsigned)j * fb_pitch + (unsigned)i] = fb[(unsigned)j * fb_pitch + (unsigned)i];
        fb = keep;
    }
}
static void win_set(int t, int nx, int ny, int nw, int nh) {   /* the one place a window changes rectangle */
    if (!fb) return;
    int *px_ = win_rc(t, 0), *py_ = win_rc(t, 1), *pw_ = win_rc(t, 2), *ph_ = win_rc(t, 3);
    if (nx == *px_ && ny == *py_ && nw == *pw_ && nh == *ph_) return;
    int s = (int)window_scale(), ox = *px_ * s, oy = *py_ * s, w = *pw_ * s, h = *ph_ * s, px = nx * s, py = ny * s, dx = px - ox, dy = py - oy;
    int resized = nw != *pw_ || nh != *ph_;
    int ux = ox < px ? ox : px, uy = oy < py ? oy : py, ex = ox + w > px + nw * s ? ox + w : px + nw * s, ey = oy + h > py + nh * s ? oy + h : py + nh * s;
    fb_hold(); fb_tag = 1;   /* 4.3.0: everything below reaches the screen as one copy, the old and new rectangles' union */
    cur_hide(); cur_frozen++;
    if (t == WT_MAIN && !resized) {
        wall_vacate(*px_, *py_, *pw_, *ph_, 1);   /* what overhangs the frame stays behind otherwise */
        for (int j = 0; j < h; j++) {   /* a memmove by rows: the far rows first when moving down, the far columns first when moving right */
            int r = dy > 0 ? h - 1 - j : j;
            unsigned *src = fb + (unsigned)(oy + r) * fb_pitch + (unsigned)ox, *dst = fb + (unsigned)(py + r) * fb_pitch + (unsigned)px;
            if (dx > 0) for (int i = w - 1; i >= 0; i--) dst[i] = src[i];
            else for (int i = 0; i < w; i++) dst[i] = src[i];
        }
        wall_minus(ox, oy, w, h, px, py, w, h);
        *px_ = nx; *py_ = ny;
        con_x += dx; con_y += dy; con_wx += dx; con_wy += dy;   /* con_layout's origin and the title bar's "lines N-M" follow; the size, and so the rows and columns, are unchanged */
    } else if (t == WT_MAIN) {
        wall_minus(ox, oy, w, h, px, py, nw * s, nh * s);
        *px_ = nx; *py_ = ny; *pw_ = nw; *ph_ = nh;
        win_sizing = resized;
        if (brick_live) brick_paint();
        else if (clock_live) clock_paint();
        else if (browser_live) { br_frame = 1; browser_paint(0); }
        else if (settings_live) set_paint();
        else { con_fit(); pane_draw(); }
        win_sizing = 0;
    } else {
        *px_ = nx; *py_ = ny; *pw_ = nw; *ph_ = nh;
        if (t == WT_APP) { app_cw = (unsigned)(nw - 16); app_ch = (unsigned)(nh - 40); if (resized) app_note_resize(); }
        win_sizing = resized;
        desk_compose(ux, uy, ex, ey);   /* the union again from the wallpaper up, every window that meets it in stacking order */
        win_sizing = 0;
    }
    fb_flush(ux, uy, ex - ux, ey - uy);
    cur_frozen--;
    fb_release();
}
static void sam_fill(void) {   /* 4.20.0: the Samantha tile opens her chat across the screen, menu bar to dock, so her face is the big one */
    if (!fb || brick_live || clock_live || browser_live || settings_live || calc_front || app_front || cp != &term_p) return;
    int s = (int)window_scale(), top = sg(MENUBAR_H) / s + 8, bottom = gui_dock_band_top() - 22;
    win_set(WT_MAIN, 16, top, (int)fb_w / s - 32, bottom - top);
    uart_puts("dock Samantha: full\n");
}
void win_move_to(int nx, int ny) {   /* the front window (or the Console family's, when none is up) to this place, held whole between the menu bar and the dock's band */
    int t = drag_on || rs_on ? drag_t : WT_MAIN;
    int s = (int)window_scale(), top = sg(MENUBAR_H) / s + 8, bottom = gui_dock_band_top() - 22, w = *win_rc(t, 2), h = *win_rc(t, 3), right = (int)fb_w / s - w;
    if (nx > right) nx = right;
    if (nx < 0) nx = 0;
    if (ny > bottom - h) ny = bottom - h;
    if (ny < top) ny = top;
    win_set(t, nx, ny, w, h);
}
static void win_resize_to(int lx, int ly) {   /* the pointer is at (lx, ly): the grabbed edges follow it, the others stay */
    int t = drag_t, s = (int)window_scale(), bottom = gui_dock_band_top() - 22, sw = (int)fb_w / s, mnw, mnh;
    int x = *win_rc(t, 0), y = *win_rc(t, 1), w = *win_rc(t, 2), h = *win_rc(t, 3), r = x + w, nx = x, nw = w, nh = h;
    win_limits(t, &mnw, &mnh);
    if (rs_on & RS_R) nw = lx - rs_dx - x;
    if (rs_on & RS_L) { nx = lx - rs_dx; nw = r - nx; }
    if (rs_on & RS_B) nh = ly - rs_dy - y;
    if (rs_on & RS_R) { if (nw > sw - x) nw = sw - x; }
    if (rs_on & RS_L) { if (nw > r) nw = r; }
    if (nw < mnw) nw = mnw;
    if (nh > bottom - y) nh = bottom - y;
    if (nh < mnh) nh = mnh;
    if (rs_on & RS_L) nx = r - nw;
    if (t == WT_APP && (unsigned long)(nw - 16) * (unsigned)(nh - 40) * 4 > UWIN_MAX) return;   /* the app's buffer must fit its mapped window: the size stays where it last fit */
    win_set(t, nx, y, nw, nh);
}
void drag_end(void) {
    if (!drag_on && !rs_on) return;
    int t = drag_t, was_sizing = rs_on;
    drag_on = rs_on = 0;
    con_quiet = 1; uart_puts("window at "); uart_dec((unsigned)*win_rc(t, 0)); uart_putc(','); uart_dec((unsigned)*win_rc(t, 1)); uart_putc('\n');
    if (was_sizing) {
        con_quiet = 1; uart_puts("window size "); uart_dec((unsigned)*win_rc(t, 2)); uart_putc('x'); uart_dec((unsigned)*win_rc(t, 3)); uart_putc('\n');
        if (t == WT_APP && app_wbytes && (app_cw != app_bw || app_ch != app_bh)) app_note_resize();   /* an event the full ring dropped is asked again */
    }
}
void pointer_moved(void) {   /* one report done: the arrow to its new place, the dock's label to the slot under it */
    if (!fb) return;
    int s = (int)window_scale(), slot = gui_dock_hit_test((int)mouse_x / s, (int)mouse_y / s);
    fb_hold();   /* 4.3.0: a moved window and the arrow on top of it go to the screen as one copy, at the end */
    if (drag_on || rs_on) {
        if (win_target() != drag_t) drag_on = rs_on = 0;
        else if (drag_on) win_move_to((int)mouse_x / s - drag_dx, (int)mouse_y / s - drag_dy);
        else win_resize_to((int)mouse_x / s, (int)mouse_y / s);
    }
    if (spot_live && spot_tree) tree_hover((int)mouse_x / s, (int)mouse_y / s);
    if (brick_live) brick_mouse((int)mouse_x / s, (int)mouse_y / s);
    cur_wanted = 1;
    cur_hide();
    if (dock_mag_zone((int)mouse_x / s, (int)mouse_y / s)) { hover_slot = slot; dock_magnify((int)mouse_x / s, slot); }   /* 6.3.0 */
    else {
        if (dock_mag_on()) { dock_unmagnify(); hover_slot = -1; }
        if (slot != hover_slot) { hover_slot = slot; dock_hover(slot); }
    }
    cur_show();
    fb_release();
}
int in_box(int x, int y, int bx, int by, int bw, int bh) { return x >= bx && x < bx + bw && y >= by && y < by + bh; }
static int on_min_dot(int x, int y, int wx, int wy) { int dx = x - (wx + 46), dy = y - (wy + 16); return dx * dx + dy * dy <= 8 * 8; }   /* 6.5.0: the yellow dot */
static int on_close_dot(int x, int y, int wx, int wy) { int dx = x - (wx + 24), dy = y - (wy + 16); return dx * dx + dy * dy <= 8 * 8; }   /* gui_draw_window_frame's red dot, radius 7 */
void dock_activate(int slot);
void ask_prefill(const char *s);   /* ask.c: type s on the Terminal's empty line */
const char *const game_names[11] = {"Mines", "Toroid", "Hamurabi", "Keyrate", "Solitaire", "Paddle", "Bricks", "Depths", "Snake", "Chess", "Movies"};   /* 6.1.0: Movies is last, not a game */
int lp_mode;                       /* 4.1.2: spot_live holds the Launchpad's card, not Spotlight's bar */
void lp_open(void);
void lp_click(int lx, int ly);
void pointer_click(void) {   /* the left button went down */
    if (!fb) return;
    int s = (int)window_scale(), lx = (int)mouse_x / s, ly = (int)mouse_y / s, slot = gui_dock_hit_test(lx, ly);
    if (spot_live && spot_tree) { tree_click(lx, ly); return; }   /* the tree menu is up: a click is a row, or it dismisses the card */
    if (tree_bar_hit()) { spot_close(); tree_open(); return; }
    if (spot_live && lp_mode) { lp_click(lx, ly); return; }   /* the Launchpad takes the click: a cell opens, anywhere else closes it */
    if (spot_live && slot < 0) return;   /* the bar is over the windows: a raise under it would leave its saved pixels stale */
    if (slot < 0 && menubar_click((int)mouse_x, (int)mouse_y)) return;   /* a status icon in the menu bar: Settings at its section */
    int on_win = in_box(lx, ly, win_lx, win_ly, win_lw, win_lh), on_calc = calc_live && in_box(lx, ly, calc_lx, calc_ly, calc_lw, calc_lh);
    if (slot < 0 && app_front) {   /* a press on an app window that is not the front one raises it first */
        for (unsigned z = 0; z < app_zn; z++) {
            struct app *w = &apps[app_z[z]];
            if (w->wbytes && in_box(lx, ly, w->lx, w->ly, w->lw, w->lh)) { if ((int)w->slot != app_top) { ap = w; app_raise(); return; } break; }
        }
    }
    int on_app = app_wbytes && !ap->min && in_box(lx, ly, app_lx, app_ly, app_lw, app_lh);
    int wt = slot < 0 ? win_target() : -1, grab = wt >= 0 ? win_grab(wt, lx, ly) : 0;   /* the front window's title band or an edge takes hold of it */
    if (grab == GRAB_TITLE) { drag_on = 1; drag_t = wt; drag_dx = lx - *win_rc(wt, 0); drag_dy = ly - *win_rc(wt, 1); return; }
    if (grab) { rs_on = grab; drag_t = wt; rs_dx = lx - (*win_rc(wt, 0) + *win_rc(wt, 2)); rs_dy = ly - (*win_rc(wt, 1) + *win_rc(wt, 3)); if (grab & RS_L) rs_dx = lx - *win_rc(wt, 0); return; }
    if (brick_live && slot < 0) {
        if (spot_live) return;
        if (on_close_dot(lx, ly, win_lx, win_ly)) brick_close();
        else if (on_win && ly >= win_ly + 32) brick_key(57, 1);
    } else if (clock_live && slot < 0) {
        if (on_close_dot(lx, ly, win_lx, win_ly)) clock_close();
    } else if (settings_live && slot < 0) {
        if (on_close_dot(lx, ly, win_lx, win_ly)) settings_close();
        else if (on_win) settings_click(lx, ly);
    } else if (browser_live && slot < 0) {
        if (spot_live) return;
        if (on_close_dot(lx, ly, win_lx, win_ly)) browser_close();
        else if (on_win) browser_click(lx, ly);
    } else if (slot >= 0) {
        dock_activate(slot);   /* it unmagnifies first, so what opens saves the plain dock */
        if (dock_mag_zone(lx, ly)) dock_magnify_now(lx, slot);   /* the pointer is still on the tile: magnified again at once, as on a Mac */
    }
    else if (on_app && app_front) {   /* the front window takes the click: an app's red dot ends it, its content gets the click */
        if (on_close_dot(lx, ly, app_lx, app_ly)) {   /* i386's close: a click above the content (b < 0), which calendar.c answers by exiting */
            app_push(2, lx - (app_lx + 8), ly - (app_ly + 32));
            if (!app_close_at) app_close_at = cnt_now() + cnt_hz() / 4;   /* and an app that ignores it is ended in a quarter second */
        }
        else if (on_min_dot(lx, ly, app_lx, app_ly)) app_minimize();   /* 6.5.0: into the dock with the genie */
        else if (in_box(lx, ly, app_lx + 8, app_ly + 32, (int)app_cw, (int)app_ch)) app_push(2, lx - (app_lx + 8), ly - (app_ly + 32));
    } else if (on_calc && calc_front) {
        if (on_close_dot(lx, ly, calc_lx, calc_ly)) { calc_close(); return; }
        int cols, n = calc_keys(&cols);
        for (int i = 0; i < n; i++) {
            int x, y, w, h; calc_cell(i, &x, &y, &w, &h);
            if (in_box(lx, ly, x, y, w, h)) { calc_did(calc_press(calc_key(i))); return; }
        }
    } else if (on_win && con_live && !calc_front && !app_front) { if (on_close_dot(lx, ly, win_lx, win_ly)) console_close(); }
    else if (on_calc) calc_raise();                    /* ...one behind it comes to the front with the keys */
    else if (on_app) app_raise();
    else if (on_win && con_live) pane_open(cp);
}
const char *dock_app_name(int slot) {   /* the EL0 app a slot starts, by its user_apps name; 0 for the kernel-drawn ones */
    return slot == SPOT_MUSIC ? "Music" : slot == SPOT_BOOKS ? "Books" : slot == SPOT_FINDER || slot == 1 ? "Finder" : slot >= SPOT_GAME0 ? game_names[slot - SPOT_GAME0] : dock_names[slot];
}
void dock_activate(int slot) {   /* a dock tile, from a click, a dock key, Spotlight or the Launchpad; slot 0, Apps, is the Launchpad (it was the Calculator until 4.1.2: the first desktop slice had no Launchpad, so the Apps tile opened the one app there was) */
    dock_unmagnify();   /* 6.3.0: whatever opens saves the plain dock under it, never the grown one */
    if (slot == 0) { lp_open(); return; }
    if (slot == SPOT_CALC) { calc_open(); return; }
    if (slot == GUI_ICON_COUNT) { clock_open(); return; }
    if (slot == GUI_ICON_COUNT + 1) { brick_open(); return; }
    if (slot == SPOT_SETTINGS) { settings_open(0); return; }
    if (slot == SPOT_BROWSER) { browser_open(); return; }
    if (slot == 2 && mail_signed_in()) { browser_go("mail:"); return; }   /* 6.32.0: Mail, signed in, is the real inbox in Blackcomb; before that the card's MAIL.TXT app */
    settings_close();
    brick_close();
    clock_close();
    browser_close();
    if (slot == 3) ics_autosync();   /* 6.43.0: Calendar syncs CalDAV first (ics.c) */
    if (app_dock(dock_app_name(slot))) return;   /* slot 1 is the Burrow tile: the file window, called Finder on the Pi */   /* Calendar, Notes, Music and Books: user/NAME.c at EL0, in its own window */
    if (slot == 7) { ask_prefill("sam "); pane_open(&term_p); sam_fill(); ask_redraw(); con_quiet = 1; uart_puts("dock Samantha: sam ready\n"); return; }   /* 3.9.8: the Samantha tile is the Terminal with `sam ` already typed */
    if (slot == 6) { pane_open(&term_p); return; }   /* dock_names[6], Terminal: the command line, in front of an open Calculator */
    console_open();
    uart_puts("dock "); uart_puts(dock_names[slot]); uart_puts(": not on ARM yet\n");
}


/* 6.6.0: the traffic lights as on a Mac. The window in front shows its three dots plain and the close X and minimize bar
   only while the pointer is over them; every window behind it shows three grey dots. The frames are drawn in the right
   state (tl_state), and this tick, on every pass of the main loop, redraws only the dots when the front window or the
   hover changes. Where a window stacked higher overlaps a back window's dots, its pixels are put back after the redraw:
   the front one is on top, the apps follow their z order, and the Console and the Calculator are below the apps (an app
   and the Calculator are never open together). */
static int tl_hover, tl_sig = -1;
static int tl_front(void) { return calc_front ? 1 : app_front && app_top >= 0 ? 2 + app_top : 0; }   /* nothing else in front: the Console (it paints its frame before con_live is set) */
static int tl_rect(int id, int *r) {   /* the window's frame, logical; 0 when it is not on the screen */
    if (id == 0) { r[0] = win_lx; r[1] = win_ly; r[2] = win_lw; r[3] = win_lh; return con_live; }
    if (id == 1) { r[0] = calc_lx; r[1] = calc_ly; r[2] = calc_lw; r[3] = calc_lh; return calc_live; }
    struct app *a = &apps[id - 2];
    r[0] = a->lx; r[1] = a->ly; r[2] = a->lw; r[3] = a->lh;
    return a->cur && a->wbytes && !a->min;
}
static int tl_rank(int id, int f) {   /* 0 on top */
    if (id == f) return 0;
    if (id >= 2) { for (unsigned z = 0; z < app_zn; z++) if (app_z[z] == id - 2) return 1 + (int)z; return 90; }
    return id == 0 ? 50 : 60;
}
int tl_state(int id) { return id != tl_front() ? GUI_TL_BACK : tl_hover ? GUI_TL_GLYPHS : GUI_TL_PLAIN; }
static unsigned tl_under[64 * 4 * 21 * 4];   /* one dot band at up to scale 4, what was there before the redraw */
void traffic_tick(void) {
    if (!fb || login_live || spot_live || brick_live || clock_live || settings_live || browser_live) { tl_sig = -1; return; }
    int s = (int)window_scale(), lx = (int)mouse_x / s, ly = (int)mouse_y / s, f = tl_front(), r[4], o[4];
    if (s > 4) return;
    tl_hover = f >= 0 && tl_rect(f, r) && in_box(lx, ly, r[0] + 14, r[1] + 6, 64, 21);
    int vis = 0;
    for (int id = 0; id < 2 + APP_SLOTS; id++) if (tl_rect(id, r)) vis |= 1 << id;
    int sig = (f + 1) * 2 + tl_hover + (vis << 8);
    if (sig == tl_sig) return;
    tl_sig = sig;
    cur_hide();
    for (int id = 0; id < 2 + APP_SLOTS; id++) {
        if (!(vis & (1 << id))) continue;
        tl_rect(id, r);
        int bx = (r[0] + 14) * s, by = (r[1] + 6) * s, bw = 64 * s, bh = 21 * s, rk = tl_rank(id, f);
        if (bx < 0 || by < 0 || bx + bw > (int)fb_w || by + bh > (int)fb_h) continue;
        for (int y = 0; y < bh; y++) for (int x = 0; x < bw; x++) tl_under[y * bw + x] = fb[(unsigned)(by + y) * fb_pitch + (unsigned)(bx + x)];
        gui_draw_traffic(r[0], r[1], tl_state(id));
        for (int k = 0; k < 2 + APP_SLOTS; k++) {   /* a window stacked higher keeps its own pixels */
            if (k == id || !(vis & (1 << k)) || tl_rank(k, f) >= rk || !tl_rect(k, o)) continue;
            for (int y = 0; y < bh; y++) for (int x = 0; x < bw; x++)
                if (in_box((bx + x) / s, (by + y) / s, o[0], o[1], o[2], o[3])) fb[(unsigned)(by + y) * fb_pitch + (unsigned)(bx + x)] = tl_under[y * bw + x];
        }
        fb_flush(bx, by, bw, bh);
    }
    con_quiet = 1; uart_puts("traffic front "); uart_dec((unsigned)(f + 1)); uart_puts(tl_hover ? " hover\n" : "\n");
    cur_show();
}
