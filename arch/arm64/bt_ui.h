/* The Bluetooth section of Settings (7.8.0), the eighth, after Wi-Fi (Joshua, board test: pairing lived only in the
   Terminal). The radio's state, then a card of what the last look heard, numbered as `bt pair N` takes them. Look for
   devices (or S) scans for five seconds, a click on a row (or its number) pairs it, Forget (or F) drops the saved pairing.
   It acts through bt.c's own bt_command, the very lines the Terminal types, so there is one pairing path, not two. The
   page draws itself again whenever the radio's state or the list changes (bp_tick, from menubar_tick). The UART says
   `settings bt scan`, `settings bt pair N NAME` and `settings bt forget` (arm64-bt-check.py drives them). */
#include "bt.h"
#define BP_ROWS 6
#define BP_BTN_W 150
static unsigned bp_last = ~0u;
static void bp_paint(int cx, int cy, int wy, int wh) {
    bp_last = bt_sig();
    const char *name[BP_ROWS], *near[BP_ROWS]; unsigned n = bt_list(name, near, BP_ROWS);
    const char *who = bt_peer_name();
    char b[80]; b[0] = 0; s_cat(b, bt_state_word()); if (who[0]) { s_cat(b, ": "); s_cat(b, who); }
    gui_text(b, cx, cy - 2, SET_INK);
    int top = cy + 24;
    if (!n) {
        gui_text("Nothing heard yet. Put your mouse or keyboard", cx, top + 4, SET_HINT);
        gui_text("in pairing mode, then Look for devices.", cx, top + 24, SET_HINT);
    } else {
        gui_rounded_rect_on_wallpaper(cx, top, SET_CARD_W, (int)n * SET_ROW_H, 0x00FFFFFF, 10);
        for (unsigned r = 0; r < n; r++) {
            int ry = top + (int)r * SET_ROW_H;
            if (r) gui_hairline_h(cx + 12, ry, SET_CARD_W - 24, 0x00E8E2D8);
            char num[2] = { (char)('1' + r), 0 };   /* 8.9.0: the number the key takes, so the list works without a mouse */
            gui_text(num, cx + 14, ry + 7, SET_HINT);
            gui_text(name[r], cx + 34, ry + 7, SET_INK);
            gui_text(near[r], cx + SET_CARD_W - 14 - gui_text_width(near[r]), ry + 7, SET_HINT);
        }
    }
    int by = top + (int)(n ? n : 2) * SET_ROW_H + 12;
    static const char *const btn[2] = { "Look for devices", "Forget saved" };
    for (int i = 0; i < 2; i++) {
        int bx = cx + i * (BP_BTN_W + 12);
        gui_rounded_rect_on_wallpaper(bx, by, BP_BTN_W, 28, i ? 0x00EDE6DC : SET_ACCENT, 14);
        gui_text(btn[i], bx + (BP_BTN_W - gui_text_width(btn[i])) / 2, by + 6, i ? SET_INK : 0x00FFFFFF);
    }
    gui_text("Click a device, or press its number, to pair it.", cx, wy + wh - 50, SET_HINT);
    gui_text("S looks for devices, F forgets, Esc closes.", cx, wy + wh - 32, SET_HINT);
}
static void bp_run(const char *line) { unsigned n = 0; while (line[n]) n++; bt_command(line, n); set_paint(); }
static void bp_scan(void) { set_log("bt scan", -1, 0); bp_run("bt scan"); }
static void bp_forget(void) { set_log("bt forget", -1, 0); bp_run("bt forget"); }
static void bp_pair(int r) {   /* row r of the card, 0 based: `bt pair r+1` */
    const char *name[BP_ROWS], *near[BP_ROWS]; unsigned n = bt_list(name, near, BP_ROWS);
    if (r < 0 || (unsigned)r >= n) return;
    set_log("bt pair", r + 1, name[r]);
    char l[] = "bt pair 0"; l[8] = (char)('1' + r); bp_run(l);
}
static int bp_key(unsigned code) {   /* 1 when the key was the page's */
    if (code == 31) { bp_scan(); return 1; }                    /* S */
    if (code == 33) { bp_forget(); return 1; }                  /* F */
    if (code >= 2 && code <= 1 + BP_ROWS) { bp_pair((int)code - 2); return 1; }   /* 1 to 6 */
    return 0;
}
static void bp_click(int lx, int ly, int cx, int cy) {
    const char *name[BP_ROWS], *near[BP_ROWS]; unsigned n = bt_list(name, near, BP_ROWS);
    int top = cy + 24;
    for (unsigned r = 0; r < n; r++)
        if (lx >= cx && lx < cx + SET_CARD_W && ly >= top + (int)r * SET_ROW_H && ly < top + (int)(r + 1) * SET_ROW_H) { bp_pair((int)r); return; }
    int by = top + (int)(n ? n : 2) * SET_ROW_H + 12;
    if (ly < by || ly >= by + 28) return;
    if (lx >= cx && lx < cx + BP_BTN_W) bp_scan();
    else if (lx >= cx + BP_BTN_W + 12 && lx < cx + 2 * BP_BTN_W + 12) bp_forget();
}
static void bp_tick(void) { if (settings_live && !spot_live && set_sec == SET_BT && bt_sig() != bp_last) set_paint(); }
