/* keys.c: cut out of main.c (the god-file split). Keyboard only: Spotlight and the Launchpad by key, the dock by key, and the desktop's chords. Built as part of main.o (the Makefile's MAINSRC) with
   main.c's own flags; what it shares with main.c is declared in kmain.h. */
#include "kmain.h"
/* ---- Keyboard only: the Pi has no mouse yet, and the Terminal (the agent's prompt) could only be opened by a click.
   F1 or Cmd+Space opens Spotlight, a small bar over the desktop: type to filter the dock's names, Up and Down to
   choose, Enter opens, Esc closes. With no pane holding the keys (the Console takes none), Left and Right move the
   dock's hover label, Enter opens that tile, Esc clears it. F2 or Ctrl+T opens the Terminal at once; Esc with the
   Terminal in front and no question running hands the keys back to the desktop (the Console). ---- */
int ask_pending(void);
int ask_select(unsigned pane);
int ask_char(unsigned code);                  /* ask.c: a key code as the character it types, Shift included */
#define SPOT_MAX 20                   /* rows the list holds (34 + 20 * 20 + 6 fits under the Pi's menu bar); 4.24.0: an empty query shows the first 20, typing finds the rest */
static char spot_text[24]; static unsigned spot_len;
int spot_sel, spot_hits[SPOT_MAX], spot_n, dock_sel = -1, ctrl_held, gui_held, alt_held;   /* Ctrl, Cmd (the GUI key), Alt or Option: either side */
unsigned *spot_under;          /* the pixels under the bar's largest size, or under the Launchpad's card, saved on open */
int spot_lx, spot_ly, spot_lh, spot_bw, spot_bh;   /* the bar on the logical grid; its height is what is drawn now; spot_bw x spot_bh is the box saved under (the bar at its tallest, or the Launchpad card) */
static int spot_match(const char *name) {   /* the typed text, case folded, anywhere in the name */
    if (!spot_len) return 1;
    for (unsigned i = 0; name[i]; i++) {
        unsigned k = 0;
        while (k < spot_len && name[i + k] && (name[i + k] | 32) == (spot_text[k] | 32)) k++;
        if (k == spot_len) return 1;
    }
    return 0;
}
const char *spot_name(int slot) { return slot == GUI_ICON_COUNT ? "Clock" : slot == GUI_ICON_COUNT + 1 ? "Brick" : slot == SPOT_MUSIC ? "Music" : slot == SPOT_BROWSER ? "Blackcomb" : slot == SPOT_BOOKS ? "Books" : slot == SPOT_CALC ? "Calculator" : slot == SPOT_FINDER ? "Finder" : slot == SPOT_SETTINGS ? "Settings" : slot == SPOT_GAME0 + 2 ? "Hamurapi" : slot >= SPOT_GAME0 ? game_names[slot - SPOT_GAME0] : dock_names[slot]; }
int spot_hmax(void) { return 34 + SPOT_ROW * SPOT_MAX + 6; }
static void spot_paint(void) {
    int s = (int)window_scale();
    spot_n = 0;
    for (int i = 1; i <= SPOT_LAST && spot_n < SPOT_MAX; i++) if (spot_match(spot_name(i)) || (i >= SPOT_GAME0 && spot_match(game_names[i - SPOT_GAME0])) || (i == SPOT_BROWSER && spot_match("Browser"))) spot_hits[spot_n++] = i;   /* 6.7.0: Blackcomb is still found as the Browser */
    if (spot_sel >= spot_n) spot_sel = spot_n ? spot_n - 1 : 0;
    cur_hide();
    int hmax = spot_hmax(), w = SPOT_W * s, h = hmax * s, x = spot_lx * s, y = spot_ly * s;
    for (int j = 0; j < h; j++) for (int i = 0; i < w; i++) fb[(unsigned)(y + j) * fb_pitch + (unsigned)(x + i)] = spot_under[j * w + i];   /* the desktop back, then the bar at its new height */
    spot_lh = 34 + SPOT_ROW * spot_n + (spot_n ? 6 : 0);
    gui_rounded_rect_on_wallpaper(spot_lx, spot_ly, SPOT_W, spot_lh, 0x00faf8f4, 10);
    window_rect(spot_lx + 12, spot_ly + 9, 3, 16, 0x00b5502c);   /* the accent: a caret mark at the left of the typed line */
    gui_text(spot_len ? spot_text : "Open an app", spot_lx + 22, spot_ly + 9, spot_len ? 0x001C1C1E : 0x0075726E);
    if (spot_n) gui_hairline_h(spot_lx + 10, spot_ly + 33, SPOT_W - 20, 0x00DDD8CE);
    for (int r = 0; r < spot_n; r++) {
        int ry = spot_ly + 37 + r * SPOT_ROW, on = r == spot_sel;
        if (on) window_rect(spot_lx + 8, ry, SPOT_W - 16, SPOT_ROW - 2, 0x00b5502c);
        gui_text(spot_name(spot_hits[r]), spot_lx + 22, ry + 2, on ? 0x00FFFFFF : 0x001C1C1E);
    }
    fb_flush(x, y, w, h);
    cur_show();
    con_quiet = 1; uart_puts("spotlight: "); uart_puts(spot_text); uart_putc(' '); uart_dec((unsigned)spot_n); uart_putc('\n');
}
/* 4.1.2: the Launchpad, the Apps tile's card. It reuses Spotlight's whole mechanism: spot_live, the saved pixels (spot_under, sized for
   the bigger of the two), spot_close's restore, and the SPOT_* slot table, so one list (spot_name, slots 1..SPOT_LAST) drives both. */
#define LP_COLS 7                     /* 4.24.0: seven across keeps the entries (28 with Chess) to four rows */
#define LP_ROWS ((SPOT_LAST + LP_COLS - 1) / LP_COLS)
#define LP_CW 86                      /* a cell: the icon over its name */
#define LP_CH 78
#define LP_PAD 22
#define LP_TOP 20
#define LP_ICON 48
#define LP_W (LP_COLS * LP_CW + 2 * LP_PAD)
#define LP_H (LP_ROWS * LP_CH + 2 * LP_TOP)
static int lp_sel;
static int spot_buf(void) {   /* the saved-pixels buffer, taken once at the larger of the two boxes */
    if (!spot_under) {
        int s = (int)window_scale(), a = LP_W * LP_H, b = SPOT_W * spot_hmax();
        spot_under = kmalloc((unsigned)((a > b ? a : b) * s * s) * 4);
    }
    return spot_under != 0;
}
void spot_open(void) {
    if (spot_live || !fb) return;
    int s = (int)window_scale(), w = SPOT_W * s, h = spot_hmax() * s;
    spot_lx = ((int)fb_w / s - SPOT_W) / 2; spot_ly = sg(MENUBAR_H) / s + 24;
    if (!spot_buf()) { uart_puts("oom spotlight\n"); return; }
    spot_bw = SPOT_W; spot_bh = spot_hmax(); lp_mode = 0;
    cur_hide();
    for (int j = 0; j < h; j++) for (int i = 0; i < w; i++) spot_under[j * w + i] = fb[(unsigned)(spot_ly * s + j) * fb_pitch + (unsigned)(spot_lx * s + i)];
    spot_live = 1; spot_tree = 0; spot_len = 0; spot_text[0] = 0; spot_sel = 0;
    con_quiet = 1; uart_puts("spotlight open\n");
    spot_paint();
}
void spot_close(void) {
    if (!spot_live) return;
    int s = (int)window_scale(), w = (spot_tree ? TREE_W : spot_bw) * s, h = (spot_tree ? TREE_H : spot_bh) * s, x = spot_lx * s, y = spot_ly * s;
    cur_hide();
    for (int j = 0; j < h; j++) for (int i = 0; i < w; i++) fb[(unsigned)(y + j) * fb_pitch + (unsigned)(x + i)] = spot_under[j * w + i];
    fb_flush(x, y, w, h);
    spot_live = 0;
    con_quiet = 1; uart_puts(spot_tree ? "treemenu close\n" : lp_mode ? "launchpad close\n" : "spotlight close\n");
    spot_tree = 0; lp_mode = 0;
    cur_show();
    if (app_front && app_shown) app_paint();   /* frames the app presented under the bar were held back */
}
int lp_on_arm(int slot) {   /* the slot starts something here: a kernel-drawn window, the Terminal, or an EL0 app in user_apps; else it is greyed */
    if (slot == 6 || slot == 7 || slot == SPOT_CALC || slot == SPOT_SETTINGS || slot == SPOT_BROWSER || slot == GUI_ICON_COUNT || slot == GUI_ICON_COUNT + 1) return 1;
    const char *name = dock_app_name(slot);
    for (const struct user_app *a = user_apps; a->name; a++) {
        unsigned i = 0;
        while (name[i] && (name[i] | 32) == a->name[i]) i++;
        if (!name[i] && !a->name[i]) return 1;
    }
    return 0;
}
static int lp_art(int slot) {   /* the ICON_ART slot of an entry: a dock tile's own art, else the app's APPS[] art (Brick has none of its own: the apps glyph; Settings: Activity's) */
    static const int order[GUI_ICON_COUNT] = GUI_DOCK_DEFAULT_ORDER;
    return slot < GUI_ICON_COUNT ? order[slot] : slot == GUI_ICON_COUNT ? 23 : slot == GUI_ICON_COUNT + 1 ? GUI_APPS_FOLDER : slot == SPOT_MUSIC ? 24 : slot == SPOT_BROWSER ? 19 : slot == SPOT_BOOKS ? 31
         : slot == SPOT_FINDER ? 0 : slot == SPOT_SETTINGS ? 22 : slot == SPOT_GAME0 ? 30 : slot == SPOT_GAME0 + 1 ? 13 : slot == SPOT_GAME0 + 2 ? 26 : slot == SPOT_GAME0 + 3 ? 9 : slot == SPOT_GAME0 + 10 ? 25 : slot >= SPOT_GAME0 + 4 ? 32 + (slot - SPOT_GAME0 - 4) : 17;   /* 4.24.0: Solitaire to Snake are APPS 32 to 36, Chess (4.25.0) 37 */
}
static void lp_tile(int icon, int x, int y, int size, unsigned under, int grey) {   /* one icon, the dock tile's art on the card (or its selection plate); grey draws it washed out */
    int pw = size * (int)window_scale();
    unsigned long mark = heap_mark();
    unsigned int *tile = kmalloc((unsigned)(pw * pw) * 4);
    unsigned char *art = 0; unsigned aw = 0, ah = 0, ach = 0;
    if (tile && png_decode(icon_art[icon], icon_art_len[icon], &art, &aw, &ah, &ach) == 0 && art && aw == ICON_ART_SIZE && ah == ICON_ART_SIZE && ach == 4) {
        gui_icon_art_scale(art, tile, pw, under);
        if (icon == 2) { int t = cal_today(); if (t >= 0) cal_overlay(tile, pw, t); }
        if (grey) for (int i = 0; i < pw * pw; i++) {
            unsigned c = tile[i]; if (c == under) continue;
            unsigned g = (((c >> 16) & 255) + ((c >> 8) & 255) + (c & 255)) / 3, o = 0;
            for (int sh = 16; sh >= 0; sh -= 8) o |= ((g * 2 + ((under >> sh) & 255) * 3) / 5) << sh;
            tile[i] = o;
        }
        gui_blit_tile(tile, x, y, size, under);
    }
    heap_release(mark);
}
static void lp_paint(void) {
    int s = (int)window_scale(), w = LP_W * s, h = LP_H * s, x = spot_lx * s, y = spot_ly * s;
    cur_hide();
    for (int j = 0; j < h; j++) for (int i = 0; i < w; i++) fb[(unsigned)(y + j) * fb_pitch + (unsigned)(x + i)] = spot_under[j * w + i];
    gui_rounded_rect_on_wallpaper(spot_lx, spot_ly, LP_W, LP_H, 0x00faf8f4, 12);
    for (int k = 0; k < SPOT_LAST; k++) {
        int slot = k + 1, cx = spot_lx + LP_PAD + (k % LP_COLS) * LP_CW, cy = spot_ly + LP_TOP + (k / LP_COLS) * LP_CH;
        int on = k == lp_sel, grey = !lp_on_arm(slot);
        unsigned under = on ? 0x00b5502c : 0x00faf8f4;
        if (on) window_rect(cx + 3, cy + 1, LP_CW - 6, LP_CH - 2, under);
        lp_tile(lp_art(slot), cx + (LP_CW - LP_ICON) / 2, cy + 6, LP_ICON, under, grey);
        const char *name = spot_name(slot);
        gui_text(name, cx + (LP_CW - gui_text_width(name)) / 2, cy + 6 + LP_ICON + 5, on ? 0x00FFFFFF : grey ? 0x00A8A39C : 0x001C1C1E);
    }
    fb_flush(x, y, w, h);
    cur_show();
    con_quiet = 1; uart_puts("launchpad: "); uart_dec((unsigned)(lp_sel + 1)); uart_putc(' '); uart_puts(spot_name(lp_sel + 1)); uart_putc('\n');
}
void lp_open(void) {
    if (spot_live || !fb) return;
    int s = (int)window_scale(), w = LP_W * s, h = LP_H * s, top = sg(MENUBAR_H) / s, bot = gui_dock_band_top();
    spot_lx = ((int)fb_w / s - LP_W) / 2; spot_ly = top + (bot - top - LP_H) / 2;
    if (spot_ly < top + 4) spot_ly = top + 4;
    if (!spot_buf()) { uart_puts("oom launchpad\n"); return; }
    cur_hide();
    for (int j = 0; j < h; j++) for (int i = 0; i < w; i++) spot_under[j * w + i] = fb[(unsigned)(spot_ly * s + j) * fb_pitch + (unsigned)(spot_lx * s + i)];
    spot_bw = LP_W; spot_bh = LP_H; spot_live = 1; lp_mode = 1; lp_sel = 0;
    con_quiet = 1; uart_puts("launchpad open "); uart_dec(SPOT_LAST); uart_putc('\n');
    lp_paint();
}
static void lp_pick(int slot) { spot_close(); dock_activate(slot); }
void lp_click(int lx, int ly) {
    int cx = lx - spot_lx - LP_PAD, cy = ly - spot_ly - LP_TOP;
    if (lx < spot_lx || ly < spot_ly || lx >= spot_lx + LP_W || ly >= spot_ly + LP_H) { spot_close(); return; }   /* outside the card */
    if (cx < 0 || cy < 0 || cx >= LP_COLS * LP_CW || cy >= LP_ROWS * LP_CH) return;
    int k = cy / LP_CH * LP_COLS + cx / LP_CW;
    if (k < SPOT_LAST) lp_pick(k + 1);
}
static int lp_key(unsigned code) {   /* arrows move the selection, Enter opens it, Esc closes; every other key is the card's too */
    if (code == 1) spot_close();
    else if (code == 28 || code == 96) lp_pick(lp_sel + 1);
    else {
        int k = lp_sel;
        if (code == 105 && k % LP_COLS) k--;
        else if (code == 106 && k % LP_COLS < LP_COLS - 1 && k + 1 < SPOT_LAST) k++;
        else if (code == 103 && k >= LP_COLS) k -= LP_COLS;
        else if (code == 108 && k + LP_COLS < SPOT_LAST) k += LP_COLS;
        if (k != lp_sel) { lp_sel = k; lp_paint(); }
    }
    return 1;
}
int spot_key(unsigned code) {   /* a key down with the bar open: every key is Spotlight's */
    if (spot_tree) { tree_key(code); return 1; }
    if (lp_mode) return lp_key(code);
    if (code == 1) spot_close();
    else if (code == 28 || code == 96) { int slot = spot_n ? spot_hits[spot_sel] : -1; spot_close(); if (slot >= 0) dock_activate(slot); }
    else if (code == 103) { if (spot_sel > 0) spot_sel--; spot_paint(); }
    else if (code == 108) { if (spot_sel + 1 < spot_n) spot_sel++; spot_paint(); }
    else if (code == 14) { if (spot_len) spot_text[--spot_len] = 0; spot_sel = 0; spot_paint(); }
    else { int ch = ask_char(code); if (ch >= ' ' && ch < 127 && spot_len + 1 < sizeof spot_text) { spot_text[spot_len++] = (char)ch; spot_text[spot_len] = 0; spot_sel = 0; spot_paint(); } }
    return 1;
}
void dock_select(int slot) { dock_unmagnify(); dock_sel = slot; hover_slot = slot; dock_hover(slot); }
