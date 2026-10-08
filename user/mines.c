/* mines: Minesweeper as a ring-3 program (roadmap "Apps after 2.2").
 *
 * The beginner board: 9x9 cells, 10 mines. Same shape as user/toroid.c:
 * SYS_WINDOW_OPEN for a framebuffer, SYS_WINDOW_POLL for input and the
 * present, SYS_EXIT to leave. A click reveals a cell; the keyboard works
 * too: arrows move a cursor, space or Enter reveals, f flags, r deals a
 * new board, esc closes. A click off the board (the title bar X) closes.
 *
 * The first board of every run comes from a fixed seed, so a check can
 * replay the same game blind: tools/checks/ring3mines-check.py grows the
 * same board from the same xorshift and reveals every safe cell by
 * keyboard, then expects "mines: won". r reseeds from SYS_TIME.
 */
#include "jtsys.h"
#include "libjt/text.h"

#define BG     0x00FAF8F6 /* GUI_BG */
#define HIDDEN 0x00D9D3CA
#define SHOWN  0x00F1EDE7
#define INK    0x001C1C1E
#define HINT   0x0075726E
#define FLAG   0x00B5502C
#define MINE   0x001C1C1E

#define MW 9
#define MH 9
#define NMINES 10
#define CELL 26
#define TOP  40
#define PAD  20

#define C_MINE  1
#define C_OPEN  2
#define C_FLAG  4

static struct jt_window_info win JT_DATA = {0, 0, 0, 0};
static unsigned char cells[MH][MW] JT_DATA = {{0}};
static int cx JT_DATA = 0, cy JT_DATA = 0, opened JT_DATA = 0, over JT_DATA = 0, won JT_DATA = 0;
static unsigned seed JT_DATA = 0x9E3779B9u; /* ponytail: fixed first deal so the check can replay it; r reseeds from the clock */

static unsigned rnd(void) { seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5; return seed; }
static int utoa10(unsigned v, char *buf) {
    char tmp[12]; int tn = 0, n = 0;
    do { tmp[tn++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (tn) buf[n++] = tmp[--tn];
    buf[n] = 0;
    return n;
}
static void say(const char *s) { unsigned n = 0; while (s[n]) n++; jt_write(1, s, n); }

static void deal(void) {
    for (int y = 0; y < MH; y++) for (int x = 0; x < MW; x++) cells[y][x] = 0;
    for (int i = 0; i < NMINES; ) {
        unsigned r = rnd() % (MW * MH);
        if (cells[r / MW][r % MW] & C_MINE) continue;
        cells[r / MW][r % MW] |= C_MINE;
        i++;
    }
    cx = cy = opened = over = won = 0;
}
static int count(int y, int x) {
    int n = 0;
    for (int dy = -1; dy <= 1; dy++) for (int dx = -1; dx <= 1; dx++) {
        int yy = y + dy, xx = x + dx;
        if ((dy || dx) && yy >= 0 && yy < MH && xx >= 0 && xx < MW && (cells[yy][xx] & C_MINE)) n++;
    }
    return n;
}
static void reveal(int y, int x) {
    if (y < 0 || y >= MH || x < 0 || x >= MW) return;
    if (cells[y][x] & (C_OPEN | C_FLAG)) return;
    cells[y][x] |= C_OPEN;
    if (cells[y][x] & C_MINE) { over = 1; say("mines: boom\n"); return; }
    opened++;
    if (count(y, x) == 0)
        for (int dy = -1; dy <= 1; dy++) for (int dx = -1; dx <= 1; dx++) if (dy || dx) reveal(y + dy, x + dx);
    if (opened == MW * MH - NMINES && !won) { won = over = 1; say("mines: won\n"); }
}

static void rect(int x, int y, int w, int h, unsigned c) {
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > (int)win.width)  w = (int)win.width - x;
    if (y + h > (int)win.height) h = (int)win.height - y;
    for (int yy = 0; yy < h; yy++) {
        unsigned *row = win.pixels + (unsigned)(y + yy) * win.width + (unsigned)x;
        for (int xx = 0; xx < w; xx++) row[xx] = c;
    }
}
static int text(const char *s, int x, int y, unsigned fg) { return jt_text_draw(&win, JT_FACE_BODY, x, y, fg, s); }

static void draw(void) {
    static const unsigned NUM[9] = {0, 0x001F5FA8, 0x002F7B4F, 0x00A13F3F, 0x00234A78, 0x008B4A9C, 0x00376E5E, INK, HINT};
    rect(0, 0, (int)win.width, (int)win.height, BG);
    for (int y = 0; y < MH; y++) for (int x = 0; x < MW; x++) {
        int px = PAD + x * CELL, py = TOP + y * CELL;
        unsigned char c = cells[y][x];
        int show = (c & C_OPEN) || (over && (c & C_MINE));
        rect(px + 1, py + 1, CELL - 2, CELL - 2, show ? SHOWN : HIDDEN);
        if (show && (c & C_MINE)) rect(px + 8, py + 8, CELL - 16, CELL - 16, MINE);
        else if (show) {
            int n = count(y, x);
            if (n) { char d[2] = {(char)('0' + n), 0}; text(d, px + 9, py + 5, NUM[n]); }
        } else if (c & C_FLAG) rect(px + 9, py + 7, 8, 12, FLAG);
        if (x == cx && y == cy && !over) { rect(px, py, CELL, 2, FLAG); rect(px, py + CELL - 2, CELL, 2, FLAG); rect(px, py, 2, CELL, FLAG); rect(px + CELL - 2, py, 2, CELL, FLAG); }
    }
    int tx = PAD + MW * CELL + 30, ty = TOP + 4;
    char num[12];
    text("Minesweeper", tx, ty, INK);
    text(won ? "You won." : over ? "Boom. r deals again." : "10 mines", tx, ty + 28, won ? NUM[2] : over ? NUM[3] : HINT);
    utoa10((unsigned)opened, num);
    int x2 = text("opened ", tx, ty + 52, HINT); text(num, x2, ty + 52, HINT);
    text("click or space reveals   f flags   arrows move", tx, ty + 90, HINT);
    text("r new board   esc closes", tx, ty + 112, HINT);
}

__attribute__((section(".text.start"), used))
void _start(int argc, char **argv) {
    (void)argc; (void)argv;
    if (jt_window_open(&win) != 0 || !win.pixels) { say("mines: no window\n"); jt_exit(1); }
    {   char line[48]; int l = 0;
        const char *pfx = "mines: ring-3 window ";
        while (*pfx) line[l++] = *pfx++;
        l += utoa10(win.width, line + l); line[l++] = 'x';
        l += utoa10(win.height, line + l); line[l++] = '\n';
        jt_write(1, line, (unsigned)l);
    }
    deal();
    draw();
    unsigned flags = JT_POLL_PRESENT;
    for (;;) {
        struct jt_event ev;
        int r = jt_window_poll(&ev, flags);
        if (r == 1 && jt_window_resized(&ev, &win)) { draw(); flags = JT_POLL_PRESENT; continue; }
        flags = 0;
        if (r == 1) {
            if (ev.kind == JT_EV_CLICK) {
                int x = (ev.a - PAD) / CELL, y = (ev.b - TOP) / CELL;
                if (ev.a < PAD || ev.b < TOP || x >= MW || y >= MH) break; /* title bar X, or off the board */
                cx = x; cy = y;
                if (!over) reveal(y, x);
            } else if (ev.kind == JT_EV_KEY) {
                if (ev.a == JT_KEY_ESC) break;
                else if (ev.a == 'r') { unsigned t = 0; jt_time(&t); seed ^= t * 2654435761u; if (!seed) seed = 1; deal(); }
                else if (ev.a == JT_KEY_UP && cy > 0) cy--;
                else if (ev.a == JT_KEY_DOWN && cy < MH - 1) cy++;
                else if (ev.a == JT_KEY_LEFT && cx > 0) cx--;
                else if (ev.a == JT_KEY_RIGHT && cx < MW - 1) cx++;
                else if (!over && (ev.a == ' ' || ev.a == JT_KEY_ENTER)) reveal(cy, cx);
                else if (!over && ev.a == 'f' && !(cells[cy][cx] & C_OPEN)) cells[cy][cx] ^= C_FLAG;
            }
            draw(); flags = JT_POLL_PRESENT;
            continue;
        }
        if (r != -11 /* -EAGAIN */) break;
        jt_sched_yield();
    }
    say("mines: closed\n");
    jt_exit(0);
}
