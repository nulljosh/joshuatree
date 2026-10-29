/* jtw (6.58.0): a window for a Plank program, in calls Plank's `extern fn` can make (int is a C long long, str a char *).
   A Plank app declares the ones it uses, for example `extern fn jtw_open() -> int`, then: jtw_open once; draw with jtw_fill
   and jtw_text or jtw_mono (logical pixels, colours as 0xRRGGBB, sharp at the Pi's scale 2); jtw_present to show the frame; jtw_wait for
   the next event: 1 a key (jtw_key: the character, or 256 up, 257 down, 258 Enter, 259 Esc, 261 left, 262 right), 2 a click
   (jtw_x, jtw_y), 4 the window changed size (draw it all again), -1 the window is gone. Built into Plank apps by
   tools/plankapp.sh, pulled in only by an app that calls it. */
#include "jtsys.h"
#include "libjt/text.h"
#include "libjt/hires.h"

static struct jt_window_info win JT_DATA = { 0, 0, 0, 0 };
static struct jt_event ev JT_DATA;
static unsigned pending JT_DATA = 0;

long long jtw_open(void) { return jt_window_open_hires(&win) == 0 && win.pixels ? 0 : -1; }
long long jtw_width(void) { return win.width; }
long long jtw_height(void) { return win.height; }
void jtw_fill(long long x, long long y, long long w, long long h, long long rgb) { if (win.pixels) jt_fill(&win, (int)x, (int)y, (int)w, (int)h, (unsigned)rgb); }
long long jtw_text(long long x, long long y, const char *s, long long rgb, long long bold) {
    return win.pixels ? jt_text_draw(&win, bold ? JT_FACE_BOLD : JT_FACE_BODY, (int)x, (int)y, (unsigned)rgb, s) : x;
}
long long jtw_mono(long long x, long long y, const char *s, long long rgb) {   /* 6.98.0: Tree Sans Mono, an 8 px cell */
    return win.pixels ? jt_mono_draw(&win, (int)x, (int)y, (unsigned)rgb, s) : x;
}
void jtw_present(void) { pending = JT_POLL_PRESENT; }
long long jtw_wait(void) {
    for (;;) {
        int r = jt_window_poll(&ev, pending);
        pending = 0;
        if (r == 1) {
            if (ev.kind == JT_EV_RESIZE) { if (jt_window_resized(&ev, &win)) return 4; continue; }
            if (ev.kind == JT_EV_KEY || ev.kind == JT_EV_CLICK) return ev.kind;
            continue;
        }
        if (r != -11 /* -EAGAIN */) return -1;
        jt_sched_yield();
    }
}
long long jtw_key(void) { return ev.a; }
long long jtw_x(void) { return ev.a; }
long long jtw_y(void) { return ev.b; }
