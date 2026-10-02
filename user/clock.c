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
 * The face is drawn here, anti-aliased with integer math (no floats at
 * ring 3); text goes through SYS_TEXT, with the 8x16 VGA font as fallback. The backquote key (`) is the deliberate crash: a write through a
 * null pointer, a page fault at ring 3, reaped by the kernel.
 * tools/checks/ring3clock-check.py presses it on purpose.
 */
#include "jtsys.h"
#include "../drivers/vgafont.h"

#define BG    0x00FAF8F6 /* GUI_BG */
#define INK   0x001C1C1E
#define HINT  0x0075726E
#define RULE  0x00E4E0DA
#define CARD  0x00FFFFFF
#define TICK  0x00B9B4AE
#define ORANGE 0x00F09A37
#define GREEN 0x00375A4A
#define RED   0x00C2453D
#define PALE  0x00FFFFFF

/* The face: the hero of the window. Everything right of it is quiet text
   on two white cards. tools/checks/ring3clock-check.py samples FACE_*. */
#define FACE_CX 186
#define FACE_CY 172
#define FACE_R  140
#define COL_X   372   /* the right column */

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

/* sin(i * 6 degrees) * 1024: one step per minute mark. cos(i) = SIN60[(i + 15) % 60]. */
static const short SIN60[60] = {0,107,213,316,416,512,602,685,761,828,887,935,974,1002,1018,1024,1018,1002,974,935,887,828,761,685,602,512,416,316,213,107,0,-107,-213,-316,-416,-512,-602,-685,-761,-828,-887,-935,-974,-1002,-1018,-1024,-1018,-1002,-974,-935,-887,-828,-761,-685,-602,-512,-416,-316,-213,-107};

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
/* Blend c over the pixel at a/255 coverage. */
static void plot(int x, int y, unsigned c, int a) {
    if (a <= 0 || x < 0 || y < 0 || x >= (int)win.width || y >= (int)win.height) return;
    unsigned *p = win.pixels + (unsigned)y * win.width + (unsigned)x;
    if (a >= 255) { *p = c; return; }
    unsigned d = *p, o = 0;
    for (int sh = 0; sh <= 16; sh += 8) {
        int fc = (int)((c >> sh) & 255), bc = (int)((d >> sh) & 255);
        o |= (unsigned)((fc * a + bc * (255 - a)) / 255) << sh;
    }
    *p = o;
}
static int isqrt(unsigned v) {
    unsigned r = 0, bit = 1u << 30;
    while (bit > v) bit >>= 2;
    while (bit) { if (v >= r + bit) { v -= r + bit; r = (r >> 1) + bit; } else r >>= 1; bit >>= 2; }
    return (int)r;
}
/* Coverage of a pixel whose centre is d (1/16 px) inside an edge at r (1/16 px). */
static int cover(int r16, int d16) { int a = (r16 + 8 - d16) * 255 / 16; return a < 0 ? 0 : a > 255 ? 255 : a; }

/* An anti-aliased disc, radius r in 1/16 px, centre in 1/16 px. */
static void disc16(int cx, int cy, int r, unsigned c) {
    int x0 = (cx - r) / 16 - 1, x1 = (cx + r) / 16 + 1, y0 = (cy - r) / 16 - 1, y1 = (cy + r) / 16 + 1;
    for (int y = y0; y <= y1; y++)
        for (int x = x0; x <= x1; x++) {
            int dx = x * 16 + 8 - cx, dy = y * 16 + 8 - cy;
            plot(x, y, c, cover(r, isqrt((unsigned)(dx * dx + dy * dy))));
        }
}
static void disc(int cx, int cy, int r, unsigned c) { disc16(cx * 16, cy * 16, r * 16, c); }
/* An anti-aliased round-capped stroke from A to B, half-width r. All 1/16 px. */
static void capsule16(int ax, int ay, int bx, int by, int r, unsigned c) {
    int x0 = ((ax < bx ? ax : bx) - r) / 16 - 1, x1 = ((ax > bx ? ax : bx) + r) / 16 + 1;
    int y0 = ((ay < by ? ay : by) - r) / 16 - 1, y1 = ((ay > by ? ay : by) + r) / 16 + 1;
    int ex = bx - ax, ey = by - ay, len2 = ex * ex + ey * ey, k = len2 >> 8;
    for (int y = y0; y <= y1; y++)
        for (int x = x0; x <= x1; x++) {
            int px = x * 16 + 8 - ax, py = y * 16 + 8 - ay;
            int t = k ? (px * ex + py * ey) / k : 0;   /* 0..256 along the stroke */
            if (t < 0) t = 0; else if (t > 256) t = 256;
            int qx = px - ex * t / 256, qy = py - ey * t / 256;
            plot(x, y, c, cover(r, isqrt((unsigned)(qx * qx + qy * qy))));
        }
}
/* A line from the face's centre outward at minute-step i (0 = twelve), from r0 to r1 px. */
static void spoke(int i, int r0, int r1, int half16, unsigned c) {
    int s = SIN60[i % 60], co = SIN60[(i + 15) % 60];
    capsule16(FACE_CX * 16 + s * r0 / 64, FACE_CY * 16 - co * r0 / 64,
              FACE_CX * 16 + s * r1 / 64, FACE_CY * 16 - co * r1 / 64, half16, c);
}
/* A white card with 12 px anti-aliased corners and a hairline edge. */
static void card(int x, int y, int w, int h, unsigned fill) {
    int r = 12;
    rect(x + r, y, w - 2 * r, h, RULE);
    rect(x, y + r, w, h - 2 * r, RULE);
    disc(x + r, y + r, r, RULE); disc(x + w - r - 1, y + r, r, RULE);
    disc(x + r, y + h - r - 1, r, RULE); disc(x + w - r - 1, y + h - r - 1, r, RULE);
    x++; y++; w -= 2; h -= 2; r--;
    rect(x + r, y, w - 2 * r, h, fill);
    rect(x, y + r, w, h - 2 * r, fill);
    disc(x + r, y + r, r, fill); disc(x + w - r - 1, y + r, r, fill);
    disc(x + r, y + h - r - 1, r, fill); disc(x + w - r - 1, y + h - r - 1, r, fill);
}
static void text(const char *s, int x, int y, unsigned fg) {
    if (jt_text(s, x, y, fg, JT_TEXT_DRAW) >= 0) return; /* SYS_TEXT */
    for (; *s; s++, x += 8) { /* the 8x16 VGA fallback */
        unsigned char ch = (unsigned char)*s;
        if (ch < VGAFONT_FIRST || ch > VGAFONT_LAST) ch = '?';
        const unsigned char *g = vgafont_glyphs + (ch - VGAFONT_FIRST) * 16;
        for (int r = 0; r < 16; r++)
            for (int c = 0; c < 8; c++)
                if (g[r] & (0x80 >> c)) plot(x + c, y + r, fg, 255);
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

static void face(unsigned sod) {
    int h = (int)(sod / 3600) % 12, m = (int)(sod / 60 % 60), s = (int)(sod % 60);
    disc16(FACE_CX * 16, FACE_CY * 16 + 40, (FACE_R + 2) * 16, RULE);   /* a soft contact shadow */
    disc(FACE_CX, FACE_CY, FACE_R + 1, RULE);                             /* hairline rim */
    disc(FACE_CX, FACE_CY, FACE_R, CARD);
    for (int i = 0; i < 60; i++) {
        if (i % 5) spoke(i, FACE_R - 14, FACE_R - 8, 10, TICK);
        else spoke(i, FACE_R - 24, FACE_R - 8, 26, INK);
    }
    spoke(h * 5 + m / 12, -10, FACE_R * 50 / 100, 56, INK);            /* hour */
    spoke(m, -10, FACE_R * 80 / 100, 40, INK);                          /* minute */
    spoke(s, -24, FACE_R * 88 / 100, 14, ORANGE);                       /* second, with its tail */
    disc16(FACE_CX * 16, FACE_CY * 16, 88, ORANGE);
    disc16(FACE_CX * 16, FACE_CY * 16, 32, CARD);
}

static void draw(unsigned now) {
    jt_text_clear();
    unsigned sod = now % 86400u;
    char buf[12];
    two(buf, (int)(sod / 3600)); buf[2] = ':';
    two(buf + 3, (int)(sod / 60 % 60)); buf[5] = ':';
    two(buf + 6, (int)(sod % 60)); buf[8] = 0;

    rect(0, 0, (int)win.width, (int)win.height, BG);
    face(sod);

    int w = (int)win.width - COL_X - 32;
    text("Now", COL_X, 40, HINT);
    text(buf, COL_X, 62, INK);

    /* Timer */
    card(COL_X, 104, w, 72, CARD);
    text("Timer", COL_X + 18, 116, HINT);
    if (mode == 1) {
        text("Minutes:", COL_X + 18, 142, HINT);
        text(entry, COL_X + 100, 142, INK);
    } else if (timer_sec > 0) {
        char t[16]; int p = 0, mins = timer_sec / 60, secs = timer_sec % 60;
        if (mins > 0) { two(t, mins); t[2] = ':'; p = 3; }
        two(t + p, secs); t[p + 2] = 0;
        text(t, COL_X + 18, 142, INK);
        text(timer_paused ? "Paused" : "Running", COL_X + w - 90, 142, timer_paused ? HINT : GREEN);
    } else {
        text("Off", COL_X + 18, 142, INK);
    }

    /* Alarm */
    card(COL_X, 192, w, 72, CARD);
    text("Alarm", COL_X + 18, 204, HINT);
    if (mode == 2) {
        text("HH:MM", COL_X + 18, 230, HINT);
        text(entry, COL_X + 80, 230, INK);
    } else if (alarm_armed) {
        char a[8];
        two(a, alarm_h); a[2] = ':'; two(a + 3, alarm_m); a[5] = 0;
        text(a, COL_X + 18, 230, INK);
        text("On", COL_X + w - 90, 230, GREEN);
    } else {
        text("Off", COL_X + 18, 230, INK);
    }

    text(mode ? "Enter sets   Esc cancels" : "Space timer   A alarm   R reset   Esc close", COL_X, 290, HINT);

    if (fired_until && (now < fired_until)) {
        card(COL_X, 104, w, 160, RED);
        text("Time's up", COL_X + 18, 176, PALE);
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

        if (ev.kind == JT_EV_CLICK) break; /* the titlebar X, or anywhere in the window */
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
