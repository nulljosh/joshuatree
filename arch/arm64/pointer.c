/* pointer.c: cut out of main.c (the god-file split). The pointer: the arrow drawn over the desktop, its save-under, and the wheel. Built as part of main.o (the Makefile's MAINSRC) with
   main.c's own flags; what it shares with main.c is declared in kmain.h. */
#include "kmain.h"
/* ---- Slice 4: the pointer. The i386 arrow (gui_paint.c's software cursor: a save of the pixels it covers, then the
   antialiased arrow blended on top) over the framebuffer. Anything else that draws where the arrow is takes it off
   first and puts it back after (cur_hold, cur_release), so the save never goes stale and nothing smears. Every change
   is cleaned out of the data cache, as the GPU reads RAM. The arrow shows from the first pointer event on, so a desktop
   nobody has touched looks exactly as before. mouse_x and mouse_y are physical pixels; the painters and the dock's hit
   test work on the logical grid, so both are divided by window_scale(). ---- */
unsigned mouse_x = 400, mouse_y = 300, mouse_moved;   /* fb_init moves it to the middle of the real screen */
int cur_wanted;                   /* a pointer has moved: show the arrow */
int win_lx, win_ly, win_lw, win_lh;   /* the Console's frame on the logical grid */
int hover_slot = -1;
static void cur_flush(int lx, int ly) { int s = (int)window_scale(); fb_flush(lx * s, ly * s, CURSOR_W * s, CURSOR_H * s); }
void cur_hide(void) {
    if (!cur_on) return;
    int x = cursor_saved_x, y = cursor_saved_y;
    gui_cursor_restore();
    cur_flush(x, y);
    cur_on = 0;
}
int cur_frozen;   /* 4.3.0: win_set is putting several windows back: the arrow waits for the end, or its saved pixels go stale */
void cur_show(void) {
    if (cur_on || !cur_wanted || !fb || cur_frozen) return;
    int s = (int)window_scale(), x = (int)mouse_x / s, y = (int)mouse_y / s;
    gui_cursor_save(x, y);
    gui_draw_cursor(x, y);
    cur_flush(x, y);
    cur_on = 1;
}
int cur_hold(void) {   /* the Console is about to draw: take the arrow off if it is over the Console's frame */
    if (!cur_on) return 0;
    int ax = cursor_saved_x, ay = cursor_saved_y;
    if (ax >= win_lx + win_lw || ax + CURSOR_W <= win_lx || ay >= win_ly + win_lh || ay + CURSOR_H <= win_ly) return 0;
    cur_hide();
    return 1;
}
void cur_release(int held) { if (held) cur_show(); }
void con_fit(void) {   /* a resized Console or Terminal: the grid for the rectangle as it is now, kept inside the frame's white well */
    int s = (int)window_scale();
    con_place(win_lx * s, win_ly * s, win_lw * s, win_lh * s);
    int wx = (win_lx + 8) * s, wy = (win_ly + 30) * s + 2 * s, wr = (win_lx + win_lw - 8) * s, wb = (win_ly + win_lh - 8) * s;
    if (con_x < wx) con_x = wx;
    if (con_y < wy) con_y = wy;
    if (con_x + con_cols * con_cw > wr) con_cols = (wr - con_x) / con_cw;
    if (con_y + (con_rows + 1) * con_ch > wb) con_rows = (wb - con_y) / con_ch - 1;
}
void console_frame(void) {   /* the i386 window frame, and a white well for the text */
    int s = (int)window_scale();
    gui_draw_window_frame_st(win_lx, win_ly, win_lw, win_lh, cp == &term_p ? (term_index ? "Terminal 2" : "Terminal 1") : "Console", tl_state(0));
    fb_rect((win_lx + 8) * s, (win_ly + 30) * s, (win_lw - 16) * s, (win_lh - 38) * s, CON_BG);
}
int term_front(void) { return cp == &term_p && !calc_front && !app_front && (con_live || !fb); }   /* with no screen at all, the UART is the Terminal */
/* The red close button puts the wallpaper back where the window was. The log keeps every line (and the UART still
   prints them); a click on any dock tile opens it again with the newest lines, so the one debug view on a Pi can
   never be lost for good. The Terminal (its tile, or F1) takes the same rectangle: one of the two is in front. With
   the Calculator open behind it, the Calculator comes to the front. */
void calc_raise(void);
void app_raise(void);
int calc_lx, calc_ly, calc_lw, calc_lh;   /* its frame on the logical grid, set in fb_init */
void wall_fill(int x, int y, int w, int h);
void tribute_paint(void) {   /* the line over the dock, drawn on the flat wallpaper at boot and again after a window leaves near it */
    int W = (int)fb_w, H = (int)fb_h, band_y = gui_dock_band_top() * (int)window_scale();
    /* Steve Jobs died on 5 October 2011. Fifteen years on, one quiet line above the dock, ending on the title
       of the Steve Jobs Archive's book of his own words, which Joshua was reading that week. */
    const char *thanks = "Seduce. Vilify. Ignore.";
    int ar = sc(6), gapx = sc(7), tw = text_width(2, thanks, sc(110)), tx = (W - tw - 7 * ar / 2 - gapx) / 2, ty = band_y - sc(4);
    {   int bx = tx - 4, by = ty - sc(4) + ar - 7 * ar / 2 - 2;   /* the line's whole box back to flat wallpaper first: a repaint over half of it would blend the edges twice */
        wall_fill(bx, by, tx + tw + gapx + 7 * ar / 2 + 4 - bx, band_y + sc(6) - by);   /* down past the descenders: the line's own pixels, nothing of the hover band's label capsule above the tiles */
    }
    text_draw(2, thanks, tx, ty, sc(110), fb_color(0x00f0f4f8), fb, fb_pitch, W, H);
    {   /* a small bitten apple after the line: apple_mask scaled to 3.5 ar square, 4x4 samples a pixel, onto the flat fill */
        int S = 7 * ar / 2, x0 = tx + tw + gapx, y0 = ty - sc(4) + ar - S; unsigned ink = fb_color(0x00f0f4f8);
        for (int y = 0; y < S; y++) for (int x = 0; x < S; x++) {
            int sum = 0;
            for (int j = 0; j < 4; j++) for (int i = 0; i < 4; i++) sum += apple_mask[((y * 4 + j) * APPLE_N / (S * 4)) * APPLE_N + (x * 4 + i) * APPLE_N / (S * 4)];
            unsigned al = (unsigned)sum / 16, d = fb[(unsigned)(y0 + y) * fb_pitch + (unsigned)(x0 + x)], out = 0;
            if (!al) continue;
            for (int b = 0; b <= 16; b += 8) out |= ((((ink >> b) & 0xFF) * al + ((d >> b) & 0xFF) * (255 - al)) / 255) << b;
            fb[(unsigned)(y0 + y) * fb_pitch + (unsigned)(x0 + x)] = out;
        }
    }
}
void wall_fill(int x, int y, int w, int h) {   /* physical px: the flat wallpaper colour (wall.c); there is no picture to save, so a window can leave from anywhere */
    unsigned c = fb_color(wall_get());
    for (int j = 0; j < h; j++) for (int i = 0; i < w; i++) fb[(unsigned)(y + j) * fb_pitch + (unsigned)(x + i)] = c;
}
void wall_minus(int ox, int oy, int ow, int oh, int nx, int ny, int nw, int nh) {   /* physical px: wallpaper over the old rectangle except where the new one is */
    for (int j = 0; j < oh; j++) {
        int y = oy + j, inside = y >= ny && y < ny + nh, a = ox, b = ox + ow;
        if (inside) {   /* the row meets the new rectangle: keep its span, fill the left and right remainders */
            int l = nx < b ? (nx > a ? nx : a) : b, r = nx + nw > a ? (nx + nw < b ? nx + nw : b) : a;
            wall_fill(a, y, l - a, 1); wall_fill(r, y, b - r, 1);
        } else wall_fill(a, y, b - a, 1);
    }
}
/* A window leaves by wall_vacate: the wallpaper over its frame AND a ring of VACATE_PAD logical px around it. Content may
   overhang its frame (Brick's playfield is 6 px wider than the frame at 1080p and its hint line runs 5 px below it), and
   the old closes cleared the frame alone, leaving the overhang as a thin white bar and glyph bits (board photo, 3.6.3).
   The ring stops under the menu bar and above the tribute line, where nothing but the wallpaper is ever under a window. */
#define VACATE_PAD 6
void wall_vacate(int lx, int ly, int lw, int lh, int ring_only) {
    int s = (int)window_scale(), x0 = (lx - VACATE_PAD) * s, y0 = (ly - VACATE_PAD) * s, x1 = (lx + lw + VACATE_PAD) * s, y1 = (ly + lh + VACATE_PAD) * s;
    int top = sg(MENUBAR_H), bottom = (gui_dock_band_top() - 12) * s;
    if (x0 < 0) x0 = 0;
    if (x1 > (int)fb_w) x1 = (int)fb_w;
    if (y0 < top) y0 = top;
    if (y1 > bottom) y1 = bottom > ly * s + lh * s ? bottom : ly * s + lh * s;   /* never less than the frame itself */
    int fx = lx * s, fy = ly * s, fw = lw * s, fh = lh * s;
    if (!ring_only) wall_fill(x0, y0, x1 - x0, y1 - y0);
    else {   /* a window being dragged keeps its frame: only the ring goes */
        if (fy > y0) wall_fill(x0, y0, x1 - x0, fy - y0);
        if (y1 > fy + fh) wall_fill(x0, fy + fh, x1 - x0, y1 - fy - fh);
        if (fx > x0) wall_fill(x0, fy, fx - x0, fh);
        if (x1 > fx + fw) wall_fill(fx + fw, fy, x1 - fx - fw, fh);
    }
    fb_flush(x0, y0, x1 - x0, y1 - y0);
    if (text_ok && y1 > (gui_dock_band_top() - 40) * s) { tribute_paint(); fb_flush(0, (gui_dock_band_top() - 40) * s, (int)fb_w, 40 * s); }   /* the ring may have cut into the line over the dock */
}
void console_close(void) {
    if (!con_live || !fb) return;
    cur_hide();
    wall_vacate(win_lx, win_ly, win_lw, win_lh, 0);
    con_live = 0;
    uart_puts(cp == &term_p ? "terminal closed\n" : "console closed\n");
    cur_show();
    if (calc_live) { wall_vacate(calc_lx, calc_ly, calc_lw, calc_lh, 0); calc_raise(); }   /* the wallpaper just went back over the part of it the window covered; the window behind is wiped whole and drawn again, so its anti-aliased edge is not blended over its own old one */
    if (app_wbytes) { wall_vacate(app_lx, app_ly, app_lw, app_lh, 0); app_raise(); }   /* the same for an app's window */
}
void ask_redraw(void);   /* ask.c: the line being typed, back on the ask> row */
void pane_draw(void) {   /* the frame, the well and the newest lines of cp, in the rectangle as it is now; the caller hides the arrow */
    int s = (int)window_scale();
    console_frame();
    con_live = 1; cp->scrolled = 0;
    unsigned nl = con_nlines(), keep = con_rows > 1 ? (unsigned)con_rows - 1 : 1;
    cp->anchor = con_line_off(nl > keep ? nl - keep : 0);
    con_wipe();
    for (unsigned k = cp->anchor; k < cp->len; k++) con_draw(k);
    con_summary(); con_hint(); ask_redraw();
    fb_flush(win_lx * s, win_ly * s, win_lw * s, win_lh * s);
}
void pane_open(struct pane *p) {   /* the Console or the Terminal in front, its newest lines in view */
    int raise = con_live && cp == p;   /* already open, behind the Calculator: bring it up and give it the keys */
    if (raise && !calc_front && !app_front) return;
    if (!fb) { cp = p; uart_puts(p == &term_p ? "terminal open\n" : "console open\n"); return; }   /* no screen: the keys still follow */
    cur_hide();
    calc_front = app_front = 0;   /* before any drawing: the pane's own draws are off while the Calculator or an app is on top */
    cp = p;
    pane_draw();
    if (raise) { con_quiet = 1; uart_puts(p == &term_p ? "focus terminal\n" : "focus console\n"); }
    else uart_puts(p == &term_p ? "terminal open\n" : "console open\n");
    cur_show();
}
void console_open(void) { pane_open(&con_p); }
void con_tui_end(void) {   /* back to the shell: the Terminal's own text and prompt */
    if (!con_rows_total()) return;
    tui_face_rows = 0; face_mouth = -1;
    int held = cur_hold(), sc_ = (int)window_scale();
    cp->scrolled = 0;
    unsigned nl = con_nlines(), keep = con_rows > 1 ? (unsigned)con_rows - 1 : 1;
    cp->anchor = con_line_off(nl > keep ? nl - keep : 0);
    con_wipe();
    for (unsigned k = cp->anchor; k < cp->len; k++) con_draw(k);
    con_summary(); con_hint(); ask_redraw();
    fb_flush(win_lx * sc_, win_ly * sc_, win_lw * sc_, win_lh * sc_);
    cur_release(held);
}

/* The Calculator (calc.c holds the sums and the keypad; this draws it). Slice 5: it is a second window, with its own
   rectangle overlapping the right of the Console's, so the Console or the Terminal stays on screen beside it. The one
   in front has the keys, as on the i386 desktop: a click on the window behind brings it up and gives it the keys
   (calc_front says which). The window behind keeps its text but draws nothing until it is in front again. The Apps
   tile opens it; a click on a key or typing works it; Esc or the red dot closes it and puts the wallpaper back. */
int calc_keys(int *cols);
const char *calc_key(int i);
int calc_press(const char *k);
int calc_type(int ch);
const char *calc_input(void);
const char *calc_output(void);
int calc_flags(void);   /* bit 0 scientific, bit 1 degrees, bit 2 memory held */
int clock_live, brick_live, spot_live, browser_live, settings_live;
#include "accounts.h"
#include "login.h"
#include "sudo.h"
int login_live;   /* login_ui.h: the login screen covers everything; the bar, the notices and the clock stand back */
void set_paint(void);
void settings_open(int sec);
void settings_close(void);
int settings_key(unsigned code);
void settings_click(int lx, int ly);
int menubar_click(int x, int y);   /* 1 when the click landed on one of the status icons, which opened Settings */
/* The tree menu (the mark and "Joshua Tree" at the left of the menu bar) is a second card on Spotlight's machinery: the same
   spot_live, declared above (so every window, drag, app frame and click that stands back for the bar stands back for it), the same saved
   pixels, the same rows of SPOT_ROW and terracotta selection. spot_tree says which card is up. */
#define TREE_W 220
#define TREE_H 112
int spot_tree;
void tree_open(void);
void tree_key(unsigned code);
void tree_click(int lx, int ly);
void tree_hover(int lx, int ly);
int tree_bar_hit(void);
void browser_open(void);
void browser_close(void);
int browser_key(unsigned code);
void browser_click(int lx, int ly);
void browser_wheel(int v);
int wheel_acc;   /* wheel notches since the last report ended */
void brick_open(void);
void brick_close(void);
int brick_key(unsigned code, unsigned value);
void brick_mouse(int lx, int ly);
void brick_tick(void);
void menubar_notify(const char *message);
void clock_open(void);
void clock_close(void);
#define CALC_TOP 36      /* the display's top, under the title band */
#define CALC_DISP 46     /* the display's height */
void calc_cell(int i, int *x, int *y, int *w, int *h) {   /* key i's box on the logical grid */
    int cols, n = calc_keys(&cols), rows = (n + cols - 1) / cols, gap = 4;
    int ax = calc_lx + 12, ay = calc_ly + CALC_TOP + CALC_DISP + 8, aw = calc_lw - 24, ah = calc_ly + calc_lh - 10 - ay;
    *w = (aw - gap * (cols - 1)) / cols; *h = (ah - gap * (rows - 1)) / rows;
    *x = ax + (i % cols) * (*w + gap); *y = ay + (i / cols) * (*h + gap);
}
void calc_paint(void) {
    int s = (int)window_scale(), x0 = calc_lx + 12, w0 = calc_lw - 24, y0 = calc_ly + CALC_TOP, f = calc_flags();
    cur_hide();
    gui_draw_window_frame_st(calc_lx, calc_ly, calc_lw, calc_lh, "Calculator", tl_state(1));
    window_rect(x0, y0, w0, CALC_DISP, 0x00DDD8CE);
    window_rect(x0 + 1, y0 + 1, w0 - 2, CALC_DISP - 2, 0x00FFFFFF);
    const char *in = calc_input(), *out = calc_output();
    while (*in && gui_text_width(in) > w0 - 70) in++;   /* keep the end of a long sum in view */
    gui_text(*in ? in : "Type or click a sum", x0 + 6, y0 + 3, *in ? 0x001C1C1E : 0x0075726E);
    gui_text(f & 1 ? (f & 2 ? "DEG" : "RAD") : "", x0 + w0 - 34, y0 + 3, 0x0075726E);
    if (f & 4) gui_text("M", x0 + w0 - 50, y0 + 3, 0x00b5502c);
    if (*out) gui_icon_text(out, x0 + w0 - 8 - gui_icon_text_w(out, 1, s), y0 + 20, 1, s, *out == 'E' ? 0x00b5502c : 0x001C1C1E);
    int cols, n = calc_keys(&cols);
    for (int i = 0; i < n; i++) {
        const char *k = calc_key(i);
        int x, y, w, h, eq = k[0] == '=', op = !k[1] && (k[0] == '+' || k[0] == '-' || k[0] == '*' || k[0] == '/');
        calc_cell(i, &x, &y, &w, &h);
        window_rect(x, y, w, h, eq ? 0x00b5502c : 0x00DDD8CE);
        if (!eq) window_rect(x + 1, y + 1, w - 2, h - 2, op ? 0x00EFEBE4 : 0x00FFFFFF);
        gui_text(k, x + (w - gui_text_width(k)) / 2, y + (h - 16) / 2, eq ? 0x00FFFFFF : 0x001C1C1E);
    }
    fb_flush(calc_lx * s, calc_ly * s, calc_lw * s, calc_lh * s);
    cur_show();
}
void calc_raise(void) {   /* in front of the Console or the Terminal, with the keys */
    if (!calc_live) return;
    int was = calc_front;
    calc_front = 1;
    calc_paint();
    if (!was) { con_quiet = 1; uart_puts("focus calc\n"); }
}
void calc_open(void) {
    settings_close();
    brick_close();
    clock_close();
    browser_close();
    app_kill("closed for the Calculator");   /* the two never share the screen, so there is no third window to stack */
    if (calc_live) { calc_raise(); return; }
    calc_live = calc_front = 1;
    calc_paint();
    uart_puts("calc open\n");
}
void calc_close(void) {
    if (!calc_live) return;
    cur_hide();
    wall_vacate(calc_lx, calc_ly, calc_lw, calc_lh, 0);
    calc_live = calc_front = 0;
    uart_puts("calc closed\n");
    if (con_live) { wall_vacate(win_lx, win_ly, win_lw, win_lh, 0); con_live = 0; pane_open(cp); }   /* the wallpaper went over its corner too: draw it whole, in front, on fresh wallpaper */
    cur_show();
}
void calc_did(int ran) {   /* after a key: redraw, and log a finished sum on the UART (what arm64-calc-check.py reads) */
    calc_paint();
    if (ran) { con_quiet = 1; uart_puts("calc: "); uart_puts(calc_input()); uart_puts(" = "); uart_puts(calc_output()); uart_putc('\n'); }
}
