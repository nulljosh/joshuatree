/* stub */
#include "jtsys.h"
#include "libjt/text.h"
static struct jt_window_info win JT_DATA = {0, 0, 0, 0};
__attribute__((section(".text.start"), used))
void _start(int argc, char **argv) {
    (void)argc; (void)argv;
    if (jt_window_open(&win) != 0 || !win.pixels) jt_exit(1);
    jt_write(1, "music: window\n", 14);
    for (;;) {
        struct jt_event ev;
        int r = jt_window_poll(&ev, JT_POLL_PRESENT);
        if (r == -11) { jt_sched_yield(); continue; }
        if (r != 1) break;
        if (ev.kind == JT_EV_KEY && ev.a == '`') { jt_write(1, "music: crashing on purpose\n", 27); *(volatile int *)0 = 1; }
        if (ev.kind == JT_EV_KEY && ev.a == JT_KEY_ESC) break;
    }
    jt_exit(0);
}
