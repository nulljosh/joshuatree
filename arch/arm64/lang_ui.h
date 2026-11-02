/* 9.2.0: the ninth section of Settings, Language (lang.c). Two columns of six, each language in its own words, a check by
   the one in use, and the line saying what follows it. Left and Right (or a click) pick; the choice is saved to
   JT/LANG.TXT at once and the window, the menu bar and the dock's labels are drawn in it from then on. The UART says
   `settings lang N code` (arm64-lang-check.py). */
#include "lang.h"
static void lg_paint(int cx, int cy, int wy, int wh) {
    int s = (int)window_scale(), half = SET_CARD_W / 2;
    gui_rounded_rect_on_wallpaper(cx, cy, SET_CARD_W, 6 * SET_ROW_H, 0x00FFFFFF, 10);
    for (int i = 0; i < LANG_COUNT; i++) {
        int x = cx + i / 6 * half, ry = cy + i % 6 * SET_ROW_H;
        if (i % 6) gui_hairline_h(x + 12, ry, half - 24, 0x00E8E2D8);
        gui_text(lang_name(i), x + 14, ry + 7, i == lang_cur ? SET_ACCENT : SET_INK);
        if (i == lang_cur) glyph_paint(GL_CHECK, 0, (x + half - 30) * s, (ry + 7) * s, 16 * s, SET_ACCENT);
    }
    gui_text(tr(S_LANG_HINT), cx, cy + 6 * SET_ROW_H + 8, SET_HINT);
    gui_text("Left and Right pick, Esc closes", cx, wy + wh - 32, SET_HINT);
}
static void lg_pick(int i) {
    lang_set(i);
    set_log("lang", i, lang_code(i));
    cur_hide(); menubar_wifi(mb_wifi); cur_show();   /* "Screen reader on" in the new words */
    set_paint();
}
static int lg_key(unsigned code) {
    if (code != 105 && code != 106) return 0;
    lg_pick((lang_cur + (code == 106 ? 1 : LANG_COUNT - 1)) % LANG_COUNT);
    return 1;
}
static void lg_click(int lx, int ly, int cx, int cy) {
    if (lx < cx || lx >= cx + SET_CARD_W || ly < cy || ly >= cy + 6 * SET_ROW_H) return;
    lg_pick((lx - cx) / (SET_CARD_W / 2) * 6 + (ly - cy) / SET_ROW_H);
}
