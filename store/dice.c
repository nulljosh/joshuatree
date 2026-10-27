/* dice: two dice in a window; Space, Enter or a click rolls them. */
/* A store app (6.56.0): `install dice`, then `userrun dice` or Spotlight. Made with the SDK (tools/mkapp.sh) like any app
   anyone could write: jtsys.h's window calls and libjt's text, nothing from the kernel's source. Each roll is said on the
   UART too ("dice: rolled 3 and 5"), which is how arm64-cardapp-check.py sees it work. */
#include "jtsys.h"
#include "libjt/text.h"
#include "libjt/hires.h"

#define BG     0x00FAF8F4
#define FACE   0x00FFFFFF
#define EDGE   0x00DDD8CE
#define INK    0x001C1C1E
#define HINT   0x0075726E
#define ACCENT 0x00B5502C
#define SIDE   112

static struct jt_window_info win JT_DATA = {0, 0, 0, 0};
static int a JT_DATA = 1, b JT_DATA = 6, rolls JT_DATA = 0;
static unsigned seed JT_DATA = 0;

static unsigned rnd(void) { seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5; return seed; }
static void say(const char *s) { unsigned n = 0; while (s[n]) n++; jt_write(1, s, n); }
static void rect(int x, int y, int w, int h, unsigned c) { jt_fill(&win, x, y, w, h, c); }
static void dot(int cx, int cy) {   /* a round pip, row by row */
    for (int dy = -9; dy <= 9; dy++) { int w = 0; while ((w + 1) * (w + 1) + dy * dy <= 81) w++; rect(cx - w, cy + dy, 2 * w + 1, 1, INK); }
}
static void die(int x, int y, int n) {
    rect(x, y, SIDE, SIDE, EDGE); rect(x + 2, y + 2, SIDE - 4, SIDE - 4, FACE);
    int l = x + 28, m = x + SIDE / 2, r = x + SIDE - 28, t = y + 28, c = y + SIDE / 2, bt = y + SIDE - 28;
    if (n & 1) dot(m, c);
    if (n > 1) { dot(l, t); dot(r, bt); }
    if (n > 3) { dot(r, t); dot(l, bt); }
    if (n == 6) { dot(l, c); dot(r, c); }
}
static void draw(void) {
    char line[24]; int k = 0;
    rect(0, 0, (int)win.width, (int)win.height, BG);
    int x0 = ((int)win.width - (2 * SIDE + 32)) / 2, y0 = 56;
    jt_text_draw(&win, JT_FACE_BOLD, x0, 18, INK, "Dice");
    die(x0, y0, a); die(x0 + SIDE + 32, y0, b);
    const char *p = "Total "; while (*p) line[k++] = *p++;
    int s = a + b; if (s > 9) line[k++] = (char)('0' + s / 10); line[k++] = (char)('0' + s % 10); line[k] = 0;
    jt_text_draw(&win, JT_FACE_BOLD, x0, y0 + SIDE + 24, a == b && rolls ? ACCENT : INK, a == b && rolls ? "Doubles!" : line);
    jt_text_draw(&win, JT_FACE_BODY, x0, y0 + SIDE + 52, HINT, "Space, Enter or a click rolls. Esc closes.");
}
static void roll(void) {
    a = (int)(rnd() % 6) + 1; b = (int)(rnd() % 6) + 1; rolls++;
    char m[] = "dice: rolled 0 and 0\n"; m[13] = (char)('0' + a); m[19] = (char)('0' + b); say(m);
}

__attribute__((section(".text.start"), used))
void _start(void) {
    unsigned t = 0; jt_time(&t); seed = t * 2654435761u | 1;
    if (jt_window_open_hires(&win) != 0 || !win.pixels) { say("dice: no window\n"); jt_exit(1); }
    say("dice: ring-3 window\n");
    draw();
    unsigned flags = JT_POLL_PRESENT;
    for (;;) {
        struct jt_event ev;
        int r = jt_window_poll(&ev, flags);
        if (r == 1 && jt_window_resized(&ev, &win)) { draw(); flags = JT_POLL_PRESENT; continue; }
        flags = 0;
        if (r == 1) {
            if (ev.kind == JT_EV_KEY && ev.a == JT_KEY_ESC) break;
            if (ev.kind == JT_EV_CLICK || (ev.kind == JT_EV_KEY && (ev.a == ' ' || ev.a == JT_KEY_ENTER))) { roll(); draw(); flags = JT_POLL_PRESENT; }
            continue;
        }
        if (r != -11 /* -EAGAIN */) break;
        jt_sched_yield();
    }
    say("dice: closed\n");
    jt_exit(0);
}
