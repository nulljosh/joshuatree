/* libjt: the phone on-screen keyboard, see osk.h. Same grid the old
   in-kernel keyboard had, drawn with the antialiased text face. */
#include "osk.h"
#include "text.h"

#define PANEL 0x00E9E4DD
#define TILE  0x00FAF8F6
#define INK   0x001C1C1E

static const char *const LETTERS[4] = {"1234567890", "qwertyuiop", "asdfghjkl", "zxcvbnm\b"};

int jt_osk_height(void) { return JT_OSK_ROW_H * JT_OSK_ROWS; }

static void fill(struct jt_window_info *w, int x, int y, int cw, int ch, unsigned c) {
    if (x < 0) { cw += x; x = 0; }
    if (y < 0) { ch += y; y = 0; }
    if (x + cw > (int)w->width) cw = (int)w->width - x;
    if (y + ch > (int)w->height) ch = (int)w->height - y;
    for (int yy = 0; yy < ch; yy++) {
        unsigned *row = w->pixels + (unsigned)(y + yy) * w->width + (unsigned)x;
        for (int xx = 0; xx < cw; xx++) row[xx] = c;
    }
}

static int row_off(int row, int cell) { return row == 2 ? cell / 2 : (row == 3 ? cell : 0); }

void jt_osk_draw(struct jt_window_info *w) {
    int sw = (int)w->width, top = (int)w->height - jt_osk_height(), cell = sw / 10;
    fill(w, 0, top, sw, jt_osk_height(), PANEL);
    for (int row = 0; row < JT_OSK_ROWS; row++) {
        int y = top + row * JT_OSK_ROW_H + 4;
        if (row == 4) {
            static const char *const names[3] = {"space", "enter", "done"};
            int xs[4] = {0, sw * 6 / 10, sw * 8 / 10, sw};
            for (int i = 0; i < 3; i++) {
                fill(w, xs[i] + 3, y, xs[i + 1] - xs[i] - 6, JT_OSK_ROW_H - 8, TILE);
                jt_text_draw(w, JT_FACE_BODY, xs[i] + (xs[i + 1] - xs[i] - jt_text_width(JT_FACE_BODY, names[i])) / 2, y + 6, INK, names[i]);
            }
            continue;
        }
        int off = row_off(row, cell);
        for (int col = 0; LETTERS[row][col]; col++) {
            char k[2] = {LETTERS[row][col], 0};
            const char *label = k[0] == '\b' ? "<" : k;
            int x = off + col * cell;
            fill(w, x + 2, y, cell - 4, JT_OSK_ROW_H - 8, TILE);
            jt_text_draw(w, JT_FACE_BODY, x + (cell - jt_text_width(JT_FACE_BODY, label)) / 2, y + 6, INK, label);
        }
    }
}

int jt_osk_hit(struct jt_window_info *w, int x, int y) {
    int sw = (int)w->width, top = (int)w->height - jt_osk_height();
    if (y < top || x < 0 || x >= sw) return 0;
    int row = (y - top) / JT_OSK_ROW_H, cell = sw / 10;
    if (row >= JT_OSK_ROWS) return 0;
    if (row == 4) return x < sw * 6 / 10 ? ' ' : (x < sw * 8 / 10 ? JT_KEY_ENTER : JT_KEY_ESC);
    int off = row_off(row, cell), len = 0;
    while (LETTERS[row][len]) len++;
    if (x < off) return 0;
    int col = (x - off) / cell;
    if (col >= len) return 0;
    return LETTERS[row][col] == '\b' ? 8 : LETTERS[row][col];
}
