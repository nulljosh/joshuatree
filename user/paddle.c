/* paddle: Paddle, a bounce game against the computer (the Pong idea under our own name), as a ring-3 program.
 *
 * Same shape as user/mines.c, plus a clock: the game steps once per kernel
 * tick (100 a second, from SYS_TASKS) whenever poll has nothing, so the ball
 * moves at the same speed on any machine. Up and Down (or w and s) move your
 * paddle on the left; the computer plays the right one and is beatable. First
 * to 7 wins, space plays again, p pauses, esc closes.
 *
 * Only what moved is repainted: the old ball and paddles are wiped to the
 * court colour and drawn again, so a step is a few hundred pixels.
 *
 * 13.24.0 (Joshua's board test: "we can't even control our character"): a key
 * press glides the paddle instead of jumping it, and the key's own repeat keeps
 * it gliding while held; a click or drag in the court puts it under the
 * pointer and the wheel nudges it. The ball leaves a short trail, a paddle
 * flashes when it returns the ball, each hit, wall and point has a short
 * blip, the score sits big in the middle of the band, and the band counts the
 * rally and keeps the best one. The ball blinks while it waits to serve.
 * tools/checks/ring3-paddle-check.py watches the ball move, a key move the
 * paddle and a point get scored.
 */
#include "jtsys.h"
#include "libjt/text.h"
#include "libjt/hires.h"

#define BG    0x00FAF8F6
#define LINE  0x00D9D3CA
#define INK   0x001C1C1E
#define BALLC 0x00B5502C
#define HITC  0x007A3520   /* a returned paddle flashes a dark clay: never INK or BALLC, which ring3-paddle-check.py finds the paddle and ball by */
#define HINT  0x0075726E

#define TOP   64      /* the score band above the court: tall enough for the big score (13.24.0; 40 before) */
#define PW    8
#define PH    56
#define BS    8
#define PX    20      /* the paddles' distance from the sides */
#define FP    16      /* positions are in sixteenths of a pixel */
#define WIN_AT 7
#define GLIDE  4      /* pixels a tick while a key holds the paddle */
#define HOLD   7      /* ticks one press glides: a tap moves 28 px; the key repeat keeps a held key going */
#define TRAIL  4
#define TRAILC 0x00E9B9A2   /* the ball's colour washed toward the court */
#define FLASH  8
#define RATE   8000

static struct jt_window_info win JT_DATA = {0, 0, 0, 0};
static int bx JT_DATA = 0, by JT_DATA = 0, vx JT_DATA = 0, vy JT_DATA = 0;   /* the ball, in FP units */
static int ly JT_DATA = 0, ry JT_DATA = 0;                                   /* paddle tops, in pixels */
static int me JT_DATA = 0, cpu JT_DATA = 0, wait JT_DATA = 0, paused JT_DATA = 0, over JT_DATA = 0, ready JT_DATA = 0;   /* ready: space served the first ball */
static int oldbx JT_DATA = -1, oldby JT_DATA = -1, oldly JT_DATA = -1, oldry JT_DATA = -1;
static unsigned seed JT_DATA = 0x6D2B79F5u;
static int pv JT_DATA = 0, hold JT_DATA = 0, aim JT_DATA = -1, dragged JT_DATA = 0;   /* dragged: a drag ended by this click, which only lets go */   /* the glide: direction and ticks left; aim: a pointer's target top, -1 none */
static int tx[TRAIL] JT_DATA = {-1, -1, -1, -1}, ty[TRAIL] JT_DATA = {-1, -1, -1, -1};   /* the trail, newest first */
static int lflash JT_DATA = 0, rflash JT_DATA = 0, rally JT_DATA = 0, best JT_DATA = 0;
static unsigned char blip[RATE / 12] JT_DATA = {0};

static void beep(unsigned hz, unsigned ms, unsigned vol) {   /* a soft square wave with a quick fade: hit, wall, point */
    unsigned n = RATE * ms / 1000; if (n > sizeof blip) n = sizeof blip;
    unsigned half = RATE / hz / 2; if (!half) half = 1;
    for (unsigned i = 0; i < n; i++) {
        unsigned amp = vol * (n - i) / n;
        blip[i] = (unsigned char)((i / half) & 1 ? 128 + amp : 128 - amp);
    }
    jt_audio_play(blip, n, RATE, JT_AUDIO_END);
}

static unsigned rnd(void) { seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5; return seed; }
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
static int chh(void) { return (int)win.height - TOP; }
static void rect(int x, int y, int w, int h, unsigned c) { jt_fill(&win, x, y, w, h, c); }
static int text(const char *s, int x, int y, unsigned fg, int face) { return jt_text_draw(&win, face, x, y, fg, s); }

static void serve(int toward) {   /* toward: +1 the computer, -1 you */
    bx = (cw() / 2 - BS / 2) * FP; by = (TOP + chh() / 2 - BS / 2) * FP;
    vx = toward * 3 * FP;
    vy = (int)(rnd() % 5) * 8 - 16;   /* -16..16: a slight angle, up or down */
    if (!vy) vy = 8;
    wait = 70;
}
static void reset(void) {
    me = cpu = over = paused = ready = 0;
    ly = ry = TOP + chh() / 2 - PH / 2;
    serve(1);
}

static void court(void) {   /* the centre line, dashed */
    for (int y = TOP + 6; y < (int)win.height - 6; y += 18) rect(cw() / 2 - 1, y, 2, 10, LINE);
}
static void scores(void) {
    char a[12], b[12];
    rect(0, 0, cw(), TOP, BG);
    rect(0, TOP - 1, cw(), 1, LINE);
    utoa10((unsigned)me, a); utoa10((unsigned)cpu, b);
    int mid = cw() / 2;   /* you on the left of the line, the computer on the right, over their own halves */
    text(a, mid - 18 - jt_text_width(JT_FACE_DISPLAY, a), 4, INK, JT_FACE_DISPLAY);
    text(b, mid + 18, 4, INK, JT_FACE_DISPLAY);
    const char *msg = over ? (me > cpu ? "You win. Space plays again." : "The computer wins. Space plays again.")
                    : paused ? "Paused. p plays on." : !ready ? "Space serves   Up and Down or the mouse move" : "p pauses   esc closes";
    text(msg, 16, TOP / 2 - 8, over && me > cpu ? 0x002F7B4F : HINT, JT_FACE_BODY);
    if (ready && !over) {   /* the rally, and the best one this session, at the right end */
        char line[32] = "Rally "; int l = 6;
        l += utoa10((unsigned)rally, line + l);
        const char *bb = "   best "; while (*bb) line[l++] = *bb++;
        l += utoa10((unsigned)best, line + l); line[l] = 0;
        text(line, cw() - 16 - jt_text_width(JT_FACE_BODY, line), TOP / 2 - 8, HINT, JT_FACE_BODY);
    }
}
static void full(void) {
    rect(0, TOP, cw(), chh(), BG);
    scores(); court();
    oldbx = oldby = oldly = oldry = -1;
    for (int i = 0; i < TRAIL; i++) tx[i] = ty[i] = -1;
}
static void frame(void) {   /* wipe what moved, draw it again */
    int nbx = bx / FP, nby = by / FP;
    for (int i = 0; i < TRAIL; i++) if (tx[i] >= 0) rect(tx[i], ty[i], BS, BS, BG);   /* the old trail goes first */
    if (oldbx >= 0 && (oldbx != nbx || oldby != nby)) {
        rect(oldbx, oldby, BS, BS, BG);
        for (int i = TRAIL - 1; i > 0; i--) { tx[i] = tx[i - 1]; ty[i] = ty[i - 1]; }
        tx[0] = oldbx; ty[0] = oldby;
    }
    if (oldly >= 0 && oldly != ly) rect(PX, oldly, PW, PH, BG);
    if (oldry >= 0 && oldry != ry) rect(cw() - PX - PW, oldry, PW, PH, BG);
    court();
    int live = !over && !wait && ready;
    for (int i = TRAIL - 1; i >= 0; i--) if (live && tx[i] >= 0) {   /* each older one smaller, centred on where the ball was */
        int sz = BS - 1 - i * 3 / 2; if (sz < 3) sz = 3;
        rect(tx[i] + (BS - sz) / 2, ty[i] + (BS - sz) / 2, sz, sz, TRAILC);
    }
    if (!live) for (int i = 0; i < TRAIL; i++) tx[i] = ty[i] = -1;
    rect(PX, ly, PW, PH, lflash ? HITC : INK);
    rect(cw() - PX - PW, ry, PW, PH, rflash ? HITC : INK);
    int show = !over && (!wait || !ready || (wait / 12) % 2 == 0);   /* it blinks while it waits to serve */
    if (show) rect(nbx, nby, BS, BS, BALLC);
    else if (!over) rect(nbx, nby, BS, BS, BG);
    oldbx = nbx; oldby = nby; oldly = ly; oldry = ry;
}

static void point(int mine) {
    if (mine) me++; else cpu++;
    beep(mine ? 660 : 196, 140, 60); rally = 0;
    char line[40] = "paddle: score "; int l = 14;
    l += utoa10((unsigned)me, line + l); line[l++] = '-';
    l += utoa10((unsigned)cpu, line + l); line[l++] = '\n'; line[l] = 0;
    say(line);
    if (me >= WIN_AT || cpu >= WIN_AT) { over = 1; say(me > cpu ? "paddle: you win\n" : "paddle: cpu wins\n"); }
    else serve(mine ? -1 : 1);
    rect(oldbx, oldby, BS, BS, BG); oldbx = -1;
    scores();
}
static int clampy(int y) { if (y < TOP) y = TOP; if (y > (int)win.height - PH) y = (int)win.height - PH; return y; }

static void glide(void) {   /* the player's paddle, every tick, served or not */
    if (over || paused) return;
    if (hold) { ly = clampy(ly + pv); hold--; }
    else if (aim >= 0) {   /* ease toward the pointer: a third of the way each tick, at least a pixel */
        int d = aim - ly; if (!d) { aim = -1; return; }
        int m = d / 3; if (!m) m = d > 0 ? 1 : -1;
        ly = clampy(ly + m);
        if (ly == clampy(aim)) aim = -1;
    }
    if (lflash) lflash--;
    if (rflash) rflash--;
}
static void step(void) {
    if (over || paused || !ready) return;
    /* the computer follows the ball once it crosses into its half, a little slower than the ball can move: without
       the half-court wait a steady player and the computer rallied forever (tools/checks/games-sim-check.py) */
    int target = by / FP + BS / 2 - PH / 2;
    if (vx > 0 && bx / FP > cw() / 2) { if (ry < target - 2) ry += 2; else if (ry > target + 2) ry -= 2; }
    ry = clampy(ry);
    if (wait) { wait--; return; }
    bx += vx; by += vy;
    int top = TOP * FP, bot = ((int)win.height - BS) * FP;
    if (by < top) { by = top; vy = -vy; beep(330, 20, 22); }
    if (by > bot) { by = bot; vy = -vy; beep(330, 20, 22); }
    int x = bx / FP, y = by / FP;
    /* a paddle sends it back, steeper the further from the middle it hits, and a touch faster */
    if (vx < 0 && x <= PX + PW && x + BS >= PX && y + BS >= ly && y <= ly + PH) {
        bx = (PX + PW) * FP; vx = -vx + 2; vy = (y + BS / 2 - (ly + PH / 2)) * 2;
        if (vx > 7 * FP) vx = 7 * FP;
        lflash = FLASH; rally++; if (rally > best) best = rally; beep(523, 35, 50); scores();
    }
    int rx = cw() - PX - PW;
    if (vx > 0 && x + BS >= rx && x <= rx + PW && y + BS >= ry && y <= ry + PH) {
        bx = (rx - BS) * FP; vx = -vx - 2; vy = (y + BS / 2 - (ry + PH / 2)) * 2;
        if (vx < -7 * FP) vx = -7 * FP;
        rflash = FLASH; beep(392, 35, 40);
    }
    if (x + BS < 0) point(0);
    else if (x > cw()) point(1);
}

__attribute__((section(".text.start"), used))
void _start(int argc, char **argv) {
    (void)argc; (void)argv;
    if (jt_window_open_hires(&win) != 0 || !win.pixels) { say("paddle: no window\n"); jt_exit(1); }
    {   char line[48]; int l = 0;
        const char *pfx = "paddle: ring-3 window ";
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
        if (r == 1 && jt_window_resized(&ev, &win)) { ly = clampy(ly); ry = clampy(ry); full(); frame(); flags = JT_POLL_PRESENT; continue; }
        flags = 0;
        if (r == 1) {
            if (ev.kind == JT_EV_KEY) {
                int k = ev.a;
                if (k == '`') { say("paddle: crashing on purpose\n"); *(volatile unsigned *)0 = 1; }
                if (k == JT_KEY_ESC) break;
                if (k == JT_KEY_UP || k == 'w') { pv = -GLIDE; hold = HOLD; aim = -1; }
                else if (k == JT_KEY_DOWN || k == 's') { pv = GLIDE; hold = HOLD; aim = -1; }
                else if (k == 'p' && !over) { paused = !paused; scores(); }
                else if (k == ' ' && over) { reset(); full(); }
                else if (k == ' ' && !ready) { ready = 1; scores(); }
            } else if (ev.kind == JT_EV_CLICK || ev.kind == JT_EV_DRAG) {   /* the paddle goes under the pointer; a plain click serves or plays again */
                if (ev.b >= TOP) { aim = clampy(ev.b - PH / 2); hold = 0; }
                if (ev.kind == JT_EV_DRAG) dragged = 1;
                else if (dragged) dragged = 0;   /* the release that ends a drag (jtsys.h): it never restarts or serves */
                else if (ev.b >= TOP) { if (over) { reset(); full(); } else if (!ready) { ready = 1; scores(); } }
            } else if (ev.kind == JT_EV_WHEEL) { pv = ev.a > 0 ? -GLIDE : GLIDE; hold = HOLD; aim = -1; }   /* a > 0 is wheel up, as in claude.c and portfolio.c */
            frame(); flags = JT_POLL_PRESENT;
            continue;
        }
        if (r != -11 /* -EAGAIN */) break;
        unsigned t = now();
        if (t != last) {
            unsigned n = t - last; if (n > 5) n = 5;   /* a long stall does not fast-forward the game */
            while (n--) { glide(); step(); }
            last = t;
            frame(); flags = JT_POLL_PRESENT;
        } else jt_sched_yield();
    }
    say("paddle: closed\n");
    jt_exit(0);
}
