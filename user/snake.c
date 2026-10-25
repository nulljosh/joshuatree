/* snake: Snake, as a ring-3 program (roadmap "Paddle ... and Snake").
 *
 * A grid sized to the window, a snake that grows by one cell for each apple, and walls that end the run, as does
 * your own tail. The arrows (or w, a, s, d) turn it; it cannot reverse onto itself. It moves on the kernel tick
 * clock like user/paddle.c, a cell every eight ticks to start and a little faster with every five apples, and only
 * the cells that changed are repainted. p pauses, and n then moves one cell a press (how a script plays it exactly);
 * space plays again, esc closes.
 *
 * Apples come from a fixed xorshift seed, so tools/checks/ring3-snake-check.py knows where the first one is,
 * steers onto it and expects "snake: apple 1".
 */
#include "jtsys.h"
#include "libjt/text.h"
#include "libjt/hires.h"

#define BG    0x00FAF8F6
#define GRID  0x00F1EDE7
#define LINE  0x00D9D3CA
#define BODY  0x00375A4A
#define HEAD  0x001C1C1E
#define APPLE 0x00B5502C
#define HINT  0x0075726E

#define TOP   40
#define CELL  16
#define MAXW  64
#define MAXH  40
#define MAXN  (MAXW * MAXH)

static struct jt_window_info win JT_DATA = {0, 0, 0, 0};
static unsigned short body[MAXN] JT_DATA = {0};   /* cells as y * MAXW + x, head first */
static int len JT_DATA = 0, gw JT_DATA = 0, gh JT_DATA = 0, dir JT_DATA = 0, next_dir JT_DATA = 0;
static int apple JT_DATA = -1, apples JT_DATA = 0, over JT_DATA = 0, paused JT_DATA = 0, every JT_DATA = 8, wait JT_DATA = 0, moving JT_DATA = 0;   /* moving: an arrow started it */
static unsigned seed JT_DATA = 0x1B8735BFu;   /* chosen so the first apple sits ahead of the head in the default window */

static unsigned rnd(void) { seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5; return seed; }
static void say(const char *s) { unsigned n = 0; while (s[n]) n++; jt_write(1, s, n); }
static int utoa10(unsigned v, char *buf) {
    char tmp[12]; int tn = 0, n = 0;
    do { tmp[tn++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (tn) buf[n++] = tmp[--tn];
    buf[n] = 0;
    return n;
}
static void say_n(const char *pfx, int v) {
    char line[48]; int l = 0;
    while (*pfx) line[l++] = *pfx++;
    l += utoa10((unsigned)v, line + l); line[l++] = '\n'; line[l] = 0;
    say(line);
}
static unsigned now(void) { struct jt_tasks t = {0, 0, 0, 0, 0}; jt_tasks(&t, 0); return t.ticks; }
static void rect(int x, int y, int w, int h, unsigned c) { jt_fill(&win, x, y, w, h, c); }
static int text(const char *s, int x, int y, unsigned fg, int face) { return jt_text_draw(&win, face, x, y, fg, s); }
static int ox(void) { return ((int)win.width - gw * CELL) / 2; }
static void cell(int c, unsigned col) {
    int x = ox() + (c % MAXW) * CELL, y = TOP + 8 + (c / MAXW) * CELL;
    if (col == GRID) rect(x, y, CELL, CELL, ((c % MAXW) + (c / MAXW)) & 1 ? GRID : BG);
    else rect(x + 1, y + 1, CELL - 2, CELL - 2, col);
}
static int on_snake(int c) { for (int i = 0; i < len; i++) if (body[i] == c) return 1; return 0; }
static void place_apple(void) {
    do { int y = (int)(rnd() % (unsigned)gh); apple = y * MAXW + (int)(rnd() % (unsigned)gw); } while (on_snake(apple));
    cell(apple, APPLE);
    say_n("snake: apple at ", apple);
}

static void band(void) {
    char num[12];
    rect(0, 0, (int)win.width, TOP, BG);
    rect(0, TOP - 1, (int)win.width, 1, LINE);
    utoa10((unsigned)apples, num);
    int x = text("Apples ", (int)win.width - 120, 12, HINT, JT_FACE_BODY); text(num, x, 12, HEAD, JT_FACE_BOLD);
    text(over ? "Game over. Space plays again." : paused ? "Paused. p plays on." : !moving ? "An arrow starts it   p pauses   esc closes" : "Arrows steer   p pauses   esc closes",
         16, 12, HINT, JT_FACE_BODY);
}
static void full(void) {
    rect(0, TOP, (int)win.width, (int)win.height - TOP, BG);
    for (int y = 0; y < gh; y++) for (int x = 0; x < gw; x++) cell(y * MAXW + x, GRID);
    for (int i = len - 1; i >= 0; i--) cell(body[i], i ? BODY : HEAD);
    if (apple >= 0) cell(apple, APPLE);
    band();
}
static void start(void) {
    gw = ((int)win.width - 16) / CELL; if (gw > MAXW) gw = MAXW;
    gh = ((int)win.height - TOP - 16) / CELL; if (gh > MAXH) gh = MAXH;
    len = 4; dir = next_dir = 0; apples = 0; over = paused = moving = 0; every = 8; wait = 0; apple = -1;
    int y = gh / 2;
    for (int i = 0; i < len; i++) body[i] = (unsigned short)(y * MAXW + 6 - i);
    full();
    place_apple();
}

static void advance(void);
static void step(void) {   /* on the clock: one cell every `every` ticks */
    if (over || paused || !moving) return;
    if (++wait < every) return;
    wait = 0;
    advance();
}
static void advance(void) {   /* one cell. dir: 0 right, 1 down, 2 left, 3 up */
    dir = next_dir;
    int hx = body[0] % MAXW, hy = body[0] / MAXW;
    hx += dir == 0 ? 1 : dir == 2 ? -1 : 0;
    hy += dir == 1 ? 1 : dir == 3 ? -1 : 0;
    int nc = hy * MAXW + hx, grow = nc == apple;
    if (hx < 0 || hy < 0 || hx >= gw || hy >= gh || (on_snake(nc) && !(nc == body[len - 1] && !grow))) {
        over = 1; say_n("snake: over ", apples); band(); return;
    }
    if (!grow) cell(body[len - 1], GRID);
    else len++;
    for (int i = len - 1; i > 0; i--) body[i] = body[i - 1];
    body[0] = (unsigned short)nc;
    cell(body[1], BODY); cell(nc, HEAD);
    if (grow) {
        apples++; say_n("snake: apple ", apples);
        if (apples % 5 == 0 && every > 3) every--;
        if (len >= gw * gh) { over = 1; say("snake: full\n"); band(); return; }
        place_apple(); band();
    }
}

__attribute__((section(".text.start"), used))
void _start(int argc, char **argv) {
    (void)argc; (void)argv;
    if (jt_window_open_hires(&win) != 0 || !win.pixels) { say("snake: no window\n"); jt_exit(1); }
    {   char line[48]; int l = 0;
        const char *pfx = "snake: ring-3 window ";
        while (*pfx) line[l++] = *pfx++;
        l += utoa10(win.width, line + l); line[l++] = 'x';
        l += utoa10(win.height, line + l); line[l++] = '\n';
        jt_write(1, line, (unsigned)l);
    }
    start();
    say_n("snake: grid ", gw * 1000 + gh);
    say_n("snake: first apple ", apple);
    unsigned flags = JT_POLL_PRESENT, last = now();
    for (;;) {
        struct jt_event ev;
        int r = jt_window_poll(&ev, flags);
        if (r == 1 && jt_window_resized(&ev, &win)) { full(); flags = JT_POLL_PRESENT; continue; }
        flags = 0;
        if (r == 1) {
            if (ev.kind == JT_EV_KEY) {
                int k = ev.a, want = -1;
                if (k == '`') { say("snake: crashing on purpose\n"); *(volatile unsigned *)0 = 1; }
                if (k == JT_KEY_ESC) break;
                if (k == JT_KEY_RIGHT || k == 'd') want = 0;
                else if (k == JT_KEY_DOWN || k == 's') want = 1;
                else if (k == JT_KEY_LEFT || k == 'a') want = 2;
                else if (k == JT_KEY_UP || k == 'w') want = 3;
                else if (k == 'p' && !over) { paused = !paused; band(); }
                else if (k == 'n' && paused && !over) advance();   /* paused: one cell per press, for a scripted game */
                else if (k == ' ' && over) start();
                if (want >= 0 && !moving && !over) { moving = 1; band(); }
                if (want >= 0 && want != (dir + 2) % 4) next_dir = want;   /* never straight back onto the neck */
            }
            flags = JT_POLL_PRESENT;
            continue;
        }
        if (r != -11 /* -EAGAIN */) break;
        unsigned t = now();
        if (t != last) {
            unsigned n = t - last; if (n > 5) n = 5;
            while (n--) step();
            last = t; flags = JT_POLL_PRESENT;
        } else jt_sched_yield();
    }
    say("snake: closed\n");
    jt_exit(0);
}
