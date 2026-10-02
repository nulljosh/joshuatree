/* clock: the time, a countdown timer and an alarm, as a real ring-3 program.
 *
 * The tenth app to leave the kernel (roadmap 2.0), done the way
 * user/fieldbook.c was. Same three jobs kernel/clock.h did in ring 0:
 * the time of day, a timer you start with space and pause with space, and
 * one alarm. Built with no kernel include path, linked flat, loaded off the
 * VFS by exec_user, and it reaches the machine only through int 0x80:
 * SYS_WINDOW_OPEN for a framebuffer, SYS_WINDOW_POLL for input and the
 * present, SYS_TIME for the wall clock, SYS_EXIT to leave.
 *
 * The time is SYS_TIME's seconds since the epoch, taken modulo a day, so it
 * reads the same RTC the kernel copy did. The old copy asked for the timer
 * minutes and the alarm time through a kernel prompt box; a ring-3 program
 * has none, so it types them on its own line at the bottom of the window.
 *
 * Glyphs: antialiased DejaVu via libjt/text.h, the time in the DISPLAY face. The backquote key (`) is the deliberate crash: a write through a
 * null pointer, a page fault at ring 3, reaped by the kernel.
 * tools/checks/ring3clock-check.py presses it on purpose.
 */
#include "jtsys.h"
#include "libjt/text.h"

#define BG    0x00FAF8F6 /* GUI_BG */
#define INK   0x001C1C1E
#define HINT  0x0075726E
#define GREEN 0x00375A4A
#define RED   0x00A13F3F
#define PALE  0x00F5F0EB

#define TIME_X 24
#define TIME_Y 40

static struct jt_window_info win JT_DATA = {0, 0, 0, 0};
static int timer_sec JT_DATA = 0;
static int timer_paused JT_DATA = 0;
static int alarm_h JT_DATA = 8;
static int alarm_m JT_DATA = 0;
static int alarm_armed JT_DATA = 0;
static unsigned fired_until JT_DATA = 0;
static unsigned last_t JT_DATA = 0;
static int mode JT_DATA = 0;         /* 0 normal, 1 typing timer minutes, 2 typing alarm HH:MM */
static char entry[8] JT_DATA = {0};
static int entry_len JT_DATA = 0;

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
static int text(const char *s, int x, int y, unsigned fg) { /* returns the x after the last glyph */
    return jt_text_draw(&win, JT_FACE_BODY, x, y, fg, s);
}
/* s fitted to maxw with a trailing "..." (in place, s is a local buffer). */
static void ellipsize(char *s, int maxw) {
    int n = 0;
    while (s[n]) n++;
    if (jt_text_width(JT_FACE_BODY, s) <= maxw) return;
    while (n > 1) {
        s[--n] = 0;
        char t[48]; int k = 0;
        while (s[k] && k < 40) { t[k] = s[k]; k++; }
        t[k++] = '.'; t[k++] = '.'; t[k++] = '.'; t[k] = 0;
        if (jt_text_width(JT_FACE_BODY, t) <= maxw) { for (int i = 0; i <= k; i++) s[i] = t[i]; return; }
    }
}
/* The time: DISPLAY digits (it has no colon), colons drawn as two dots. */
static void big_time(const char *hms, int x, int y) {
    int dh = jt_text_height(JT_FACE_DISPLAY);
    for (int g = 0; g < 3; g++) {
        char pair[3] = {hms[g * 3], hms[g * 3 + 1], 0};
        x = jt_text_draw(&win, JT_FACE_DISPLAY, x, y, INK, pair);
        if (g < 2) {
            rect(x + 6, y + dh / 2 - 12, 5, 5, INK);
            rect(x + 6, y + dh / 2 + 7, 5, 5, INK);
            x += 18;
        }
    }
}
static int utoa10(unsigned v, char *buf) {
    char tmp[12]; int tn = 0, n = 0;
    do { tmp[tn++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (tn) buf[n++] = tmp[--tn];
    buf[n] = 0;
    return n;
}
static void say(const char *pfx, unsigned v) { /* one line, one write: what the check reads */
    char line[40]; int l = 0;
    while (*pfx) line[l++] = *pfx++;
    l += utoa10(v, line + l); line[l++] = '\n';
    jt_write(1, line, (unsigned)l);
}

static void two(char *b, int v) { b[0] = (char)('0' + v / 10); b[1] = (char)('0' + v % 10); }

static void draw(unsigned now) {
    unsigned sod = now % 86400u;
    char buf[12];
    two(buf, (int)(sod / 3600)); buf[2] = ':';
    two(buf + 3, (int)(sod / 60 % 60)); buf[5] = ':';
    two(buf + 6, (int)(sod % 60)); buf[8] = 0;

    rect(0, 0, (int)win.width, (int)win.height, BG);
    big_time(buf, TIME_X, TIME_Y);

    int y = TIME_Y + jt_text_height(JT_FACE_DISPLAY) + 24;
    if (timer_sec > 0) {
        char t[16]; int p = 0, mins = timer_sec / 60, secs = timer_sec % 60;
        if (mins > 0) { two(t, mins); t[2] = ':'; p = 3; }
        two(t + p, secs); t[p + 2] = 0;
        int tx = text("Timer:", 20, y, HINT) + 8;
        tx = text(t, tx, y, INK) + 10;
        text(timer_paused ? "paused" : "running", tx, y, GREEN);
    } else {
        text("Timer: off", 20, y, HINT);
    }
    if (alarm_armed) {
        char a[8];
        two(a, alarm_h); a[2] = ':'; two(a + 3, alarm_m); a[5] = 0;
        int ax = text("Alarm:", 20, y + 25, HINT) + 8;
        text(a, ax, y + 25, INK);
    }
    if (mode) {
        int ex = text(mode == 1 ? "Timer minutes:" : "Alarm HH:MM:", 20, y + 60, HINT) + 8;
        ex = text(entry, ex, y + 60, INK);
        rect(ex + 1, y + 60 + 2, 2, 16, INK); /* caret */
        text("enter sets  esc cancels", 20, y + 84, HINT);
    }
    {   char h[56] = "space starts timer  r resets  a alarm  esc closes";
        ellipsize(h, (int)win.width - 40);
        text(h, 20, (int)win.height - 30, HINT);
    }
    if (fired_until && now < fired_until) {
        int by = (int)win.height / 2 - 20;
        rect(40, by, (int)win.width - 80, 40, RED);
        jt_text_draw(&win, JT_FACE_BOLD, ((int)win.width - jt_text_width(JT_FACE_BOLD, "ALARM!")) / 2, by + 10, PALE, "ALARM!");
    }
}

static void finish_entry(unsigned now) {
    if (mode == 1) {
        int m = 0;
        for (int i = 0; entry[i]; i++) m = m * 10 + (entry[i] - '0');
        timer_sec = m * 60; timer_paused = 0;
        say("clock: timer ", (unsigned)timer_sec);
    } else if (entry_len == 5 && entry[2] == ':') {
        int h = (entry[0] - '0') * 10 + (entry[1] - '0');
        int m = (entry[3] - '0') * 10 + (entry[4] - '0');
        if (h < 24 && m < 60) { alarm_h = h; alarm_m = m; alarm_armed = 1; say("clock: alarm ", (unsigned)(h * 100 + m)); }
    }
    (void)now;
    mode = 0;
}

__attribute__((section(".text.start"), used))
void _start(int argc, char **argv) {
    (void)argc; (void)argv;
    if (jt_window_open(&win) != 0 || !win.pixels) {
        jt_write(2, "clock: no window\n", 17);
        jt_exit(1);
    }
    {   /* one line, one write: what tools/checks/ring3clock-check.py asserts on */
        char line[48]; int l = 0;
        const char *pfx = "clock: ring-3 window ";
        while (*pfx) line[l++] = *pfx++;
        l += utoa10(win.width, line + l); line[l++] = 'x';
        l += utoa10(win.height, line + l); line[l++] = '\n';
        jt_write(1, line, (unsigned)l);
    }

    last_t = (unsigned)jt_time(0);
    draw(last_t);

    unsigned flags = JT_POLL_PRESENT;
    for (;;) {
        struct jt_event ev;
        int r = jt_window_poll(&ev, flags);
        if (r == 1 && jt_window_resized(&ev, &win)) { draw(last_t); flags = JT_POLL_PRESENT; continue; } /* JT_EV_RESIZE: remapped, repaint at the new size */
        flags = 0;
        unsigned now = (unsigned)jt_time(0);
        int dirty = 0;
        if (now != last_t) {   /* a second went by: count the timer down, test the alarm */
            unsigned d = now - last_t;
            last_t = now;
            if (timer_sec > 0 && !timer_paused) {
                timer_sec -= d > (unsigned)timer_sec ? timer_sec : (int)d;
                if (timer_sec == 0) { fired_until = now + 3; jt_write(1, "clock: timer done\n", 18); }
            }
            unsigned sod = now % 86400u;
            if (alarm_armed && (int)(sod / 3600) == alarm_h && (int)(sod / 60 % 60) == alarm_m) {
                alarm_armed = 0; fired_until = now + 3; jt_write(1, "clock: alarm fired\n", 19);
            }
            dirty = 1;
        }
        if (r == -11 /* -EAGAIN */) {
            if (dirty) { draw(now); flags = JT_POLL_PRESENT; }
            else jt_sched_yield();
            continue;
        }
        if (r != 1) break;

        if (ev.kind != JT_EV_KEY) { flags = JT_POLL_PRESENT; continue; }
        int k = ev.a;
        if (k == '`') {
            jt_write(1, "clock: crashing on purpose\n", 27);
            *(volatile int *)0 = 1;
        }
        if (mode) {
            if (k == JT_KEY_ESC) mode = 0;
            else if (k == JT_KEY_ENTER) finish_entry(now);
            else if (k == 8 && entry_len > 0) entry[--entry_len] = 0;
            else if (entry_len < (mode == 1 ? 4 : 5) && ((k >= '0' && k <= '9') || (mode == 2 && k == ':'))) {
                entry[entry_len++] = (char)k; entry[entry_len] = 0;
            }
        } else if (k == JT_KEY_ESC) break;
        else if (k == ' ') {
            if (timer_sec > 0) timer_paused = !timer_paused;
            else { mode = 1; entry_len = 0; entry[0] = 0; }
        }
        else if (k == 'r') { timer_sec = 0; timer_paused = 0; }
        else if (k == 'a') { mode = 2; entry_len = 0; entry[0] = 0; }
        draw(now); flags = JT_POLL_PRESENT;
    }
    jt_write(1, "clock: closed\n", 14);
    jt_exit(0);
}
