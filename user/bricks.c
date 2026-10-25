/* bricks: Bricks, the brick-breaking game (the Breakout idea Jobs and Wozniak built at Atari, under our own name),
 * as a ring-3 program.
 *
 * Same clock as user/paddle.c: one step per kernel tick (100 a second, from SYS_TASKS) when poll has nothing, and only
 * what moved is repainted. Left and Right (or a and d) move the paddle along the bottom. Six rows of bricks; clear
 * them all to win, lose the ball three times and it is over. p pauses, space plays again, esc closes.
 *
 * tools/checks/ring3-bricks-check.py watches the ball move, a key move the paddle and the first serve break a brick.
 */
#include "jtsys.h"
#include "libjt/text.h"
#include "libjt/hires.h"

#define BG    0x00FAF8F6
#define LINE  0x00D9D3CA
#define INK   0x001C1C1E
#define BALLC 0x00B5502C
#define HINT  0x0075726E

#define TOP   40      /* the score band */
#define PW    72
#define PH    8
#define BS    8
#define FP    16
#define ROWS  6
#define COLS  12
#define BH    14
#define BGAP  4
#define BTOP  (TOP + 30)
#define LIVES 3

static const unsigned ROWC[ROWS] = {0x008E3A1E, 0x00B5502C, 0x00C9673F, 0x00D9894F, 0x00A0553F, 0x006B4423};

static struct jt_window_info win JT_DATA = {0, 0, 0, 0};
static unsigned char alive[ROWS][COLS] JT_DATA = {{0}};
static int bx JT_DATA = 0, by JT_DATA = 0, vx JT_DATA = 0, vy JT_DATA = 0;
static int px JT_DATA = 0, left JT_DATA = 0, lives JT_DATA = 0, score JT_DATA = 0, wait JT_DATA = 0, paused JT_DATA = 0, over JT_DATA = 0, held JT_DATA = 1;   /* held: the ball rides the paddle until space */
static int oldbx JT_DATA = -1, oldby JT_DATA = -1, oldpx JT_DATA = -1;

static void say(const char *s) { unsigned n = 0; while (s[n]) n++; jt_write(1, s, n); }
static int utoa10(unsigned v, char *buf) {
    char tmp[12]; int tn = 0, n = 0;
    do { tmp[tn++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (tn) buf[n++] = tmp[--tn];
    buf[n] = 0;
    return n;
}
static unsigned now(void) { struct jt_tasks t = {0, 0, 0, 0, 0}; jt_tasks(&t, 0); return t.ticks; }

static int cw(void) { return (int)win.width; }
static int py(void) { return (int)win.height - 28; }
static int brick_w(void) { return (cw() - 32 - (COLS - 1) * BGAP) / COLS; }
static int brick_x(int c) { return 16 + c * (brick_w() + BGAP); }
static int brick_y(int r) { return BTOP + r * (BH + BGAP); }
static void rect(int x, int y, int w, int h, unsigned c) { jt_fill(&win, x, y, w, h, c); }
static int text(const char *s, int x, int y, unsigned fg, int face) { return jt_text_draw(&win, face, x, y, fg, s); }

static void serve(void) {   /* from just above the paddle, up and a little to the right */
    bx = (px + PW / 2 - BS / 2) * FP; by = (py() - BS - 2) * FP;
    vx = 24; vy = -3 * FP;
    wait = 0; held = 1;
}
static void reset(void) {
    for (int r = 0; r < ROWS; r++) for (int c = 0; c < COLS; c++) alive[r][c] = 1;
    left = ROWS * COLS; lives = LIVES; score = 0; over = paused = 0;
    px = cw() / 2 - PW / 2;
    serve();
}

static void band(void) {
    char num[12];
    rect(0, 0, cw(), TOP, BG);
    rect(0, TOP - 1, cw(), 1, LINE);
    utoa10((unsigned)score, num);
    int x = text("Score ", cw() - 200, 12, HINT, JT_FACE_BODY); x = text(num, x, 12, INK, JT_FACE_BOLD);
    utoa10((unsigned)lives, num);
    x = text("   Balls ", x, 12, HINT, JT_FACE_BODY); text(num, x, 12, INK, JT_FACE_BOLD);
    const char *msg = over ? (left ? "Out of balls. Space plays again." : "Cleared. Space plays again.")
                    : paused ? "Paused. p plays on." : held ? "Space launches   Left and Right move   esc closes" : "Left and Right move   p pauses   esc closes";
    text(msg, 16, 12, over && !left ? 0x002F7B4F : HINT, JT_FACE_BODY);
}
static void full(void) {
    rect(0, TOP, cw(), (int)win.height - TOP, BG);
    band();
    for (int r = 0; r < ROWS; r++) for (int c = 0; c < COLS; c++) if (alive[r][c]) rect(brick_x(c), brick_y(r), brick_w(), BH, ROWC[r]);
    oldbx = oldby = oldpx = -1;
}
static void frame(void) {
    int nbx = bx / FP, nby = by / FP;
    if (oldbx >= 0 && (oldbx != nbx || oldby != nby)) rect(oldbx, oldby, BS, BS, BG);
    if (oldpx >= 0 && oldpx != px) rect(oldpx, py(), PW, PH, BG);
    rect(px, py(), PW, PH, INK);
    if (!over) rect(nbx, nby, BS, BS, BALLC);
    oldbx = nbx; oldby = nby; oldpx = px;
}

static void lose(void) {
    lives--;
    say("bricks: ball lost\n");
    rect(oldbx, oldby, BS, BS, BG); oldbx = -1;
    if (lives <= 0) { over = 1; say("bricks: game over\n"); }
    else serve();
    band();
}
static int hit_brick(void) {   /* the brick under the ball, broken; returns 1 when one was */
    int x = bx / FP, y = by / FP;
    for (int r = 0; r < ROWS; r++) for (int c = 0; c < COLS; c++) {
        if (!alive[r][c]) continue;
        int X = brick_x(c), Y = brick_y(r);
        if (x + BS <= X || x >= X + brick_w() || y + BS <= Y || y >= Y + BH) continue;
        alive[r][c] = 0; left--; score += (ROWS - r) * 10;
        rect(X, Y, brick_w(), BH, BG);
        /* bounce off the side it came through: the smaller overlap */
        int ox = vx > 0 ? x + BS - X : X + brick_w() - x, oy = vy > 0 ? y + BS - Y : Y + BH - y;
        if (ox < oy) vx = -vx; else vy = -vy;
        char line[40] = "bricks: brick "; int l = 14;
        l += utoa10((unsigned)(ROWS * COLS - left), line + l); line[l++] = '\n'; line[l] = 0;
        say(line);
        if (!left) { over = 1; say("bricks: cleared\n"); }
        band();
        return 1;
    }
    return 0;
}
static int clampx(int x) { if (x < 0) x = 0; if (x > cw() - PW) x = cw() - PW; return x; }

static void step(void) {
    if (over || paused) return;
    if (held || wait) { if (wait) wait--; bx = (px + PW / 2 - BS / 2) * FP; return; }   /* the ball rides the paddle until space */
    bx += vx; by += vy;
    int lo = 0, hi = (cw() - BS) * FP, top = TOP * FP;
    if (bx < lo) { bx = lo; vx = -vx; }
    if (bx > hi) { bx = hi; vx = -vx; }
    if (by < top) { by = top; vy = -vy; }
    int x = bx / FP, y = by / FP;
    if (vy > 0 && y + BS >= py() && y <= py() + PH && x + BS >= px && x <= px + PW) {
        by = (py() - BS) * FP; vy = -vy;
        vx = (x + BS / 2 - (px + PW / 2)) * 2;   /* steeper off the ends */
        if (vy > -3 * FP) vy = -3 * FP;
    }
    hit_brick();
    if (y > (int)win.height) lose();
}

__attribute__((section(".text.start"), used))
void _start(int argc, char **argv) {
    (void)argc; (void)argv;
    if (jt_window_open_hires(&win) != 0 || !win.pixels) { say("bricks: no window\n"); jt_exit(1); }
    {   char line[48]; int l = 0;
        const char *pfx = "bricks: ring-3 window ";
        while (*pfx) line[l++] = *pfx++;
        l += utoa10(win.width, line + l); line[l++] = 'x';
        l += utoa10(win.height, line + l); line[l++] = '\n';
        jt_write(1, line, (unsigned)l);
    }
    reset();
    full(); frame();
    unsigned flags = JT_POLL_PRESENT, last = now();
    for (;;) {
        struct jt_event ev;
        int r = jt_window_poll(&ev, flags);
        if (r == 1 && jt_window_resized(&ev, &win)) { px = clampx(px); full(); frame(); flags = JT_POLL_PRESENT; continue; }
        flags = 0;
        if (r == 1) {
            if (ev.kind == JT_EV_KEY) {
                int k = ev.a;
                if (k == '`') { say("bricks: crashing on purpose\n"); *(volatile unsigned *)0 = 1; }
                if (k == JT_KEY_ESC) break;
                if (k == JT_KEY_LEFT || k == 'a') px = clampx(px - 28);
                else if (k == JT_KEY_RIGHT || k == 'd') px = clampx(px + 28);
                else if (k == 'p' && !over) { paused = !paused; band(); }
                else if (k == ' ' && over) { reset(); full(); }
                else if (k == ' ' && held) { held = 0; band(); }
            } else if (ev.kind == JT_EV_CLICK || ev.kind == JT_EV_DRAG) px = clampx(ev.a - PW / 2);   /* a click or drag puts the paddle there */
            frame(); flags = JT_POLL_PRESENT;
            continue;
        }
        if (r != -11 /* -EAGAIN */) break;
        unsigned t = now();
        if (t != last) {
            unsigned n = t - last; if (n > 5) n = 5;
            while (n--) step();
            last = t;
            frame(); flags = JT_POLL_PRESENT;
        } else jt_sched_yield();
    }
    say("bricks: closed\n");
    jt_exit(0);
}
