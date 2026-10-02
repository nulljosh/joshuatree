/* toroid: Conway's Game of Life on a torus, as a real ring-3 program.
 *
 * The second app to leave the kernel (roadmap 2.0), done exactly the way
 * user/keyrate.c was. Same game drivers/app_toroid.c ran in ring 0: both
 * edges wrap, a quarter of the cells start alive, space pauses, r reseeds,
 * c clears (and pauses), a click toggles a cell, a click off the grid or
 * esc closes. Built with no kernel include path, linked flat, loaded off
 * the VFS by exec_user, and it reaches the machine only through int 0x80:
 * SYS_WINDOW_OPEN for a framebuffer, SYS_WINDOW_POLL for input and the
 * present, SYS_TIME to pace the generations, SYS_EXIT to leave.
 *
 * Where the grids live: two 160x80 boards, one bit per cell, in this
 * program's own .data (1600 bytes each, 3200 in all). The in-kernel
 * version kept them as 2x 160x80 bytes on the kernel heap; a flat binary
 * has a 7-page image window (user/note.ld) and no .bss, so byte grids
 * would have eaten most of it. Bits fit with room to spare, and the
 * kernel never sees them at all.
 *
 * Pacing: SYS_TIME is whole seconds, the finest clock the ABI has, so the
 * program counts its own poll+yield rounds per second and steps every
 * rounds/12 of them, about the 8-tick cadence the ring-0 loop kept.
 *
 * The backquote key (`) is the deliberate crash, same as Keyrate: a write
 * through a null pointer, a page fault at ring 3, reaped by the kernel.
 * tools/checks/ring3toroid-check.py presses it on purpose.
 */
#include "jtsys.h"
#include "libjt/text.h"

#define BG     0x00FAF8F6 /* GUI_BG */
#define BOARD  0x00F1EDE7
#define INK    0x001C1C1E
#define HINT   0x0075726E

#define TR_MAXW 160
#define TR_MAXH 80
#define TR_CELL 10
#define TR_TOP  48
#define TR_PAD  20
#define TR_ROWB (TR_MAXW / 8)

static struct jt_window_info win JT_DATA = {0, 0, 0, 0};
static unsigned char grid_a[TR_MAXH][TR_ROWB] JT_DATA = {{0}};
static unsigned char grid_b[TR_MAXH][TR_ROWB] JT_DATA = {{0}};
static int tr_w JT_DATA = 0, tr_h JT_DATA = 0, tr_gen JT_DATA = 0, tr_paused JT_DATA = 0;
static unsigned tr_seed JT_DATA = 2463534242u;

static unsigned tr_rand(void) { tr_seed ^= tr_seed << 13; tr_seed ^= tr_seed >> 17; tr_seed ^= tr_seed << 5; return tr_seed; }
static int  get(unsigned char (*g)[TR_ROWB], int y, int x) { return (g[y][x >> 3] >> (x & 7)) & 1; }
static void set(unsigned char (*g)[TR_ROWB], int y, int x, int v) {
    if (v) g[y][x >> 3] |= (unsigned char)(1u << (x & 7));
    else   g[y][x >> 3] &= (unsigned char)~(1u << (x & 7));
}

static void tr_reseed(void) {
    for (int y = 0; y < tr_h; y++) for (int x = 0; x < tr_w; x++) set(grid_a, y, x, (tr_rand() % 4) == 0);
    tr_gen = 0;
}
static void tr_clear(void) {
    for (int y = 0; y < TR_MAXH; y++) for (int b = 0; b < TR_ROWB; b++) grid_a[y][b] = 0;
    tr_gen = 0;
}
static void tr_step(void) {
    for (int y = 0; y < tr_h; y++) {
        int yu = (y + tr_h - 1) % tr_h, yd = (y + 1) % tr_h;
        for (int x = 0; x < tr_w; x++) {
            int xl = (x + tr_w - 1) % tr_w, xr = (x + 1) % tr_w;
            int n = get(grid_a, yu, xl) + get(grid_a, yu, x) + get(grid_a, yu, xr) + get(grid_a, y, xl) + get(grid_a, y, xr)
                  + get(grid_a, yd, xl) + get(grid_a, yd, x) + get(grid_a, yd, xr);
            set(grid_b, y, x, n == 3 || (n == 2 && get(grid_a, y, x)));
        }
    }
    for (int y = 0; y < tr_h; y++) for (int b = 0; b < TR_ROWB; b++) grid_a[y][b] = grid_b[y][b];
    tr_gen++;
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
static int utoa10(unsigned v, char *buf) {
    char tmp[12]; int tn = 0, n = 0;
    do { tmp[tn++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (tn) buf[n++] = tmp[--tn];
    buf[n] = 0;
    return n;
}

static void tr_draw(void) {
    rect(TR_PAD, TR_TOP, tr_w * TR_CELL, tr_h * TR_CELL, BOARD);
    for (int y = 0; y < tr_h; y++) for (int x = 0; x < tr_w; x++)
        if (get(grid_a, y, x)) rect(TR_PAD + x * TR_CELL + 1, TR_TOP + y * TR_CELL + 1, TR_CELL - 2, TR_CELL - 2, INK);
    int ly = (int)win.height - 30;
    rect(TR_PAD, ly - 2, (int)win.width - 2 * TR_PAD, 20, BG);
    char num[12]; utoa10((unsigned)tr_gen, num);
    int x = text("generation ", TR_PAD, ly, HINT);
    x = text(num, x, ly, HINT);
    if (tr_paused) x = text("   paused", x, ly, HINT);
    {   const char *h = "space pause   r reseed   c clear   click a cell   esc closes";
        int hx = (int)win.width - TR_PAD - jt_text_width(JT_FACE_BODY, h);
        if (hx < x + 24) { h = "space  r  c  esc"; hx = (int)win.width - TR_PAD - jt_text_width(JT_FACE_BODY, h); }
        if (hx >= x + 24) text(h, hx, ly, HINT);
    }
}

__attribute__((section(".text.start"), used))
void _start(int argc, char **argv) {
    (void)argc; (void)argv;
    if (jt_window_open(&win) != 0 || !win.pixels) {
        jt_write(2, "toroid: no window\n", 18);
        jt_exit(1);
    }
    {   /* one line, one write: what tools/checks/ring3toroid-check.py asserts on */
        char line[48]; int l = 0;
        const char *pfx = "toroid: ring-3 window ";
        while (*pfx) line[l++] = *pfx++;
        l += utoa10(win.width, line + l); line[l++] = 'x';
        l += utoa10(win.height, line + l); line[l++] = '\n';
        jt_write(1, line, (unsigned)l);
    }
    unsigned now = 0; jt_time(&now);
    tr_seed ^= now; if (!tr_seed) tr_seed = 2463534242u;
    tr_w = ((int)win.width - 2 * TR_PAD) / TR_CELL;
    tr_h = ((int)win.height - TR_TOP - 44) / TR_CELL;
    if (tr_w > TR_MAXW) tr_w = TR_MAXW;
    if (tr_h > TR_MAXH) tr_h = TR_MAXH;
    if (tr_w < 8) tr_w = 8;
    if (tr_h < 8) tr_h = 8;
    tr_paused = 0;
    tr_reseed();
    rect(0, 0, (int)win.width, (int)win.height, BG);
    tr_draw();

    /* Rounds of poll+yield per generation, recalibrated every second from
       SYS_TIME so the cadence tracks the machine: about 12 steps a second. */
    unsigned rounds_per_step = 40, rounds_this_sec = 0, last_sec = now, since_step = 0;
    unsigned flags = JT_POLL_PRESENT;
    for (;;) {
        struct jt_event ev;
        int r = jt_window_poll(&ev, flags);
        if (r == 1 && jt_window_resized(&ev, &win)) { tr_draw(); flags = JT_POLL_PRESENT; continue; } /* JT_EV_RESIZE: remapped, repaint at the new size */
        flags = 0;
        if (r == 1) {
            if (ev.kind == JT_EV_CLICK) {
                int cx = (ev.a - TR_PAD) / TR_CELL, cy = (ev.b - TR_TOP) / TR_CELL;
                if (ev.a < TR_PAD || ev.b < TR_TOP || cx < 0 || cy < 0 || cx >= tr_w || cy >= tr_h) break; /* titlebar X, or off the grid */
                set(grid_a, cy, cx, !get(grid_a, cy, cx));
            } else if (ev.kind == JT_EV_KEY) {
                if (ev.a == JT_KEY_ESC) break;
                if (ev.a == '`') {
                    jt_write(1, "toroid: crashing on purpose\n", 28);
                    *(volatile int *)0 = 1;
                }
                if (ev.a == ' ') tr_paused = !tr_paused;
                else if (ev.a == 'r') tr_reseed();
                else if (ev.a == 'c') { tr_clear(); tr_paused = 1; }
            }
            tr_draw(); flags = JT_POLL_PRESENT;
            continue;
        }
        if (r != -11 /* -EAGAIN */) break;
        jt_sched_yield();
        rounds_this_sec++;
        unsigned t = 0; jt_time(&t);
        if (t != last_sec) {
            if (rounds_this_sec >= 12) rounds_per_step = rounds_this_sec / 12;
            rounds_this_sec = 0; last_sec = t;
        }
        if (!tr_paused && ++since_step >= rounds_per_step) {
            since_step = 0;
            tr_step(); tr_draw(); flags = JT_POLL_PRESENT;
        }
    }
    jt_write(1, "toroid: closed\n", 15);
    jt_exit(0);
}
