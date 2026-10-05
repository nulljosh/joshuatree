/* windgate: guided breathing, as a real ring-3 program (2.8).
 *
 * The web app's four presets (Box 4-4-4-4, 4-7-8, Relax 4-0-6-0, Coherent 5-0-5-0, in seconds: in, hold, out,
 * hold). A circle grows while you breathe in, holds, shrinks while you breathe out. Beside it: the phase word
 * (Breathe in, Hold, Breathe out), the seconds left in the phase, the round count and the session time, four
 * preset buttons and a Start button. Fully offline: no network, no files.
 *
 * Keys: Left and Right or 1 to 4 pick a preset (and start it over), Enter or Space starts and pauses, Esc closes.
 * A click on a preset button or on the Start button does the same. The backquote key (`) is the deliberate crash:
 * a write through a null pointer, a page fault at ring 3, reaped by the kernel (ring3crash-all-check.py).
 *
 * Built the way user/toroid.c and user/hamurabi.c are: flat binary, loaded off the VFS, and it reaches the machine
 * only through int 0x80: SYS_WINDOW_OPEN for a framebuffer, SYS_WINDOW_POLL for input and the present, SYS_TIME for
 * the clock, SYS_BRK for the back buffer, SYS_EXIT to leave. SYS_TIME is whole seconds, the one clock that is true
 * wall time (QEMU's 100 Hz tick counter runs about twice too fast here, measured). Like Toroid, the program uses
 * the finer counter (SYS_TASKS ticks) only to place itself inside the current second, and recalibrates its rate
 * against SYS_TIME at every second boundary, so the circle moves smoothly and the seconds stay exact. It needs
 * two boundaries to know the rate (under two seconds after launch); until then the exercise clock waits.
 * Between frames the program yields to the scheduler, so it never busy-loops. Every frame is painted into a back
 * buffer and copied over the window in one pass, so the compositor never catches a half-painted frame.
 *
 * Exercise time only counts while the exercise runs: a pause freezes the circle exactly where it is, and a resume
 * carries on from there.
 *
 * Serial lines (tools/checks/ring3windgate-check.py reads these; the host can verify exact timings from them):
 *   windgate: ring-3 window <w>x<h>
 *   windgate: layout circle <cx>,<cy> rmin <a> rmax <b> presets <x,y,w,h> x4 start <x,y,w,h>
 *   windgate: preset <n> selected <Name> pattern <in>,<hold>,<out>,<hold>
 *   windgate: preset <n> phase <inhale|hold-full|exhale|hold-empty> secs <s> round <r> at <ms>
 *       one per phase that starts; "at" is the exercise time in ms at which the phase began, and it is the exact
 *       boundary (a sum of whole phase lengths), not the moment the loop noticed it
 *   windgate: clock ready ticks-per-second <n>   (the rate learned off SYS_TIME; the exercise clock starts counting)
 *   windgate: running at <ms> / windgate: paused at <ms>
 *   windgate: closed
 */
#include "jtsys.h"
#include "libjt/text.h"

typedef unsigned int u32;

#define BG      0x00F4F7FBu /* the web app's light page */
#define INK     0x00232338u /* house ink */
#define MUTED   0x005C6B7Du /* the web app's dim text */
#define ACCENT  0x003F6FD8u /* the web app's own light-theme accent: also the icon's circle blue */
#define PANEL   0x00DBE6F7u /* idle button, the web app's circle tint */
#define GUIDE   0x00E8EEF9u /* the disc the circle breathes inside */
#define GUIDE_LN 0x00C4D3EEu
#define WHITE   0x00FFFFFFu

/* In, hold, out, hold in seconds: the web app's four presets. A 0 skips that phase. */
static const char *const P_NAME[4] = { "Box", "4-7-8", "Relax", "Coherent" };
static const char *const P_TIMES[4] = { "4-4-4-4", "4-7-8", "4-6", "5-5" };
static const unsigned char P_SECS[4][4] = { {4, 4, 4, 4}, {4, 7, 8, 0}, {4, 0, 6, 0}, {5, 0, 5, 0} };
static const char *const PHASE_WORD[4] = { "Breathe in", "Hold", "Breathe out", "Hold" };
static const char *const PHASE_LOG[4] = { "inhale", "hold-full", "exhale", "hold-empty" };

static struct jt_window_info win JT_DATA = {0, 0, 0, 0}; /* the real window: only written by the copy at the end of draw() */
static struct jt_window_info cv JT_DATA = {0, 0, 0, 0};  /* the back buffer, on the SYS_BRK heap, every draw goes here */
#define BB_BYTES 0x220000u /* the kernel's whole window framebuffer (JT_USER_FB_BYTES in kernel/memmap.h): never needs more */

static int preset JT_DATA = 0;
static int running JT_DATA = 0;
static u32 ex_ms JT_DATA = 0;        /* exercise time in ms, only while running */
static int logged_seq JT_DATA = -1;  /* the last phase start written to serial, counted over the whole session */

/* Geometry, set by layout() and reported once so the check clicks without re-deriving it. */
static int g_cx JT_DATA, g_cy JT_DATA, g_rmax JT_DATA, g_rmin JT_DATA;
static int g_pb[4][4] JT_DATA; /* preset buttons x,y,w,h */
static int g_sb[4] JT_DATA;    /* Start button */
static int g_rx JT_DATA, g_rw JT_DATA;

/* ---------- small helpers, no libc ---------- */
static u32 isqrt32(u32 v) {
    u32 r = 0, b = 1u << 30;
    while (b > v) b >>= 2;
    while (b) {
        if (v >= r + b) { v -= r + b; r = (r >> 1) + b; } else r >>= 1;
        b >>= 2;
    }
    return r;
}
static int utoa10(u32 v, char *buf) {
    char tmp[12]; int tn = 0, n = 0;
    do { tmp[tn++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (tn) buf[n++] = tmp[--tn];
    buf[n] = 0;
    return n;
}
static void cat(char *dst, int *n, const char *s) { while (*s) dst[(*n)++] = *s++; dst[*n] = 0; }
static void catu(char *dst, int *n, u32 v) { *n += utoa10(v, dst + *n); }
static void say(const char *s) { int n = 0; while (s[n]) n++; jt_write(1, s, (unsigned)n); }

/* Wall time in ms since launch: exact seconds from SYS_TIME, the part inside the second from the tick counter,
   whose rate is re-learned at every second boundary. Never goes backwards. */
static u32 rtc_base JT_DATA = 0, rtc_last JT_DATA = 0, tk_at_sec JT_DATA = 0, tps JT_DATA = 0, wall_last JT_DATA = 0;
static int rtc_seen JT_DATA = 0;
static u32 wall_ms(void) {
    struct jt_tasks t; jt_tasks(&t, -1);
    unsigned rtc = 0; jt_time(&rtc);
    if (!rtc_seen) { rtc_base = rtc; rtc_last = rtc; tk_at_sec = t.ticks; rtc_seen = 1; }
    else if (rtc != rtc_last) {
        if (rtc_seen >= 2 && rtc == rtc_last + 1 && t.ticks > tk_at_sec) tps = t.ticks - tk_at_sec; /* ticks in the full second just ended */
        tk_at_sec = t.ticks; rtc_last = rtc; rtc_seen++;
    }
    u32 frac = tps ? (t.ticks - tk_at_sec) * 1000u / tps : 0;
    if (frac > 999u) frac = 999u;
    u32 w = (rtc - rtc_base) * 1000u + frac;
    if (w < wall_last) w = wall_last;
    wall_last = w;
    return w;
}

/* ---------- drawing, all into the back buffer cv ---------- */
static void rect(int x, int y, int w, int h, u32 c) {
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > (int)cv.width)  w = (int)cv.width - x;
    if (y + h > (int)cv.height) h = (int)cv.height - y;
    for (int yy = 0; yy < h; yy++) {
        u32 *row = cv.pixels + (u32)(y + yy) * cv.width + (u32)x;
        for (int xx = 0; xx < w; xx++) row[xx] = c;
    }
}
static void blend(int x, int y, u32 c, u32 a) { /* a: 0..255 coverage of colour c over what is there */
    if (x < 0 || y < 0 || x >= (int)cv.width || y >= (int)cv.height || !a) return;
    u32 *p = &cv.pixels[(u32)y * cv.width + (u32)x];
    u32 d = *p, o = 0;
    for (int s = 0; s <= 16; s += 8) {
        u32 dc = (d >> s) & 255u, sc = (c >> s) & 255u;
        o |= ((dc * (255u - a) + sc * a + 127u) / 255u) << s;
    }
    *p = o;
}
/* Anti-aliased disc, radius in sixteenths of a pixel (smooth growth between whole pixels). Pixel (cx,cy) is the centre. */
static void disc(int cx, int cy, int r16, u32 c) {
    if (r16 <= 0) return;
    int R = (r16 + 15) / 16 + 1;
    u32 in16 = r16 > 8 ? (u32)(r16 - 8) : 0, out16 = (u32)(r16 + 8);
    int a_in = (int)(in16 * in16 / 256u), a_out = (int)(out16 * out16 / 256u);
    for (int dy = -R; dy <= R; dy++) {
        int ox2 = a_out - dy * dy;
        if (ox2 < 0) continue;
        int ox = (int)isqrt32((u32)ox2) + 1;
        int ix2 = a_in - dy * dy;
        int ins = (in16 && ix2 >= 0) ? (int)isqrt32((u32)ix2) : -1;
        int y = cy + dy;
        if (ins >= 0) rect(cx - ins, y, 2 * ins + 1, 1, c);
        for (int dx = -ox; dx <= ox; dx++) {
            if (dx >= -ins && dx <= ins) { dx = ins; continue; }
            u32 d16 = isqrt32((u32)(dx * dx + dy * dy) * 256u);
            int cov = r16 + 8 - (int)d16;
            if (cov <= 0) continue;
            if (cov > 16) cov = 16;
            blend(cx + dx, y, c, (u32)(cov * 255 / 16));
        }
    }
}
/* Rounded rectangle with anti-aliased corners. */
static void rrect(int x, int y, int w, int h, int rad, u32 c) {
    rect(x + rad, y, w - 2 * rad, h, c);
    rect(x, y + rad, rad, h - 2 * rad, c);
    rect(x + w - rad, y + rad, rad, h - 2 * rad, c);
    for (int k = 0; k < 4; k++) {
        int ox = (k & 1) ? x + w - rad : x, oy = (k & 2) ? y + h - rad : y;
        int ccx = (k & 1) ? ox : ox + rad - 1, ccy = (k & 2) ? oy : oy + rad - 1;
        for (int j = 0; j < rad; j++) for (int i = 0; i < rad; i++) {
            int dx = ox + i - ccx, dy = oy + j - ccy;
            u32 d16 = isqrt32((u32)(dx * dx + dy * dy) * 256u);
            int cov = rad * 16 - (int)d16;
            if (cov <= 0) continue;
            if (cov > 16) cov = 16;
            blend(ox + i, oy + j, c, (u32)(cov * 255 / 16));
        }
    }
}
static void text(int face, int x, int y, u32 c, const char *s) { jt_text_draw(&cv, face, x, y, c, s); }
static void text_c(int face, int cx, int y, u32 c, const char *s) { text(face, cx - jt_text_width(face, s) / 2, y, c, s); }

/* ---------- the exercise ---------- */
static u32 cycle_ms(int p) { return (u32)(P_SECS[p][0] + P_SECS[p][1] + P_SECS[p][2] + P_SECS[p][3]) * 1000u; }

/* Where the exercise is at an exercise time: the phase, how far into it (ms), how long it lasts (ms), the finished
   rounds, how many phases have started over the whole session (the sequence number), and the exercise time the phase began. */
struct where { int phase, seq; u32 into, len, round, began; };
static struct where where_at(u32 ms) {
    struct where w = {0, 0, 0, 0, 0, 0};
    u32 cyc = cycle_ms(preset), t = ms % cyc;
    int nz = 0, ord = 0;
    for (int p = 0; p < 4; p++) if (P_SECS[preset][p]) nz++;
    w.round = ms / cyc;
    u32 acc = 0;
    for (int p = 0; p < 4; p++) {
        u32 L = (u32)P_SECS[preset][p] * 1000u;
        if (!L) continue;
        if (t < acc + L) { w.phase = p; w.into = t - acc; w.len = L; w.began = w.round * cyc + acc; w.seq = (int)w.round * nz + ord; return w; }
        acc += L; ord++;
    }
    return w;
}
static u32 smooth(u32 into, u32 len) { /* 0..1024, smoothstep: gentle at both ends like a breath */
    u32 u = into * 1024u / len;
    return u * u * (3072u - 2u * u) / 1048576u;
}
/* The circle's radius in sixteenths of a pixel for the given exercise time. */
static int radius16(u32 ms) {
    int lo = g_rmin * 16, hi = g_rmax * 16;
    if (!running && ms == 0) return lo;
    struct where w = where_at(ms);
    if (w.phase == 0) return lo + (int)((u32)(hi - lo) * smooth(w.into, w.len) / 1024u);
    if (w.phase == 1) return hi;
    if (w.phase == 2) return hi - (int)((u32)(hi - lo) * smooth(w.into, w.len) / 1024u);
    return lo;
}

static void layout(void) {
    int w = (int)cv.width, h = (int)cv.height;
    int left = w * 57 / 100;
    int rmax = left / 2 - 16, rv = (h - 34) / 2 - 8;
    if (rv < rmax) rmax = rv;
    if (rmax < 30) rmax = 30;
    g_rmax = rmax; g_rmin = rmax * 55 / 100;
    g_cx = left / 2; g_cy = (h - 34) / 2;
    g_rx = left + 14; g_rw = w - g_rx - 28;
    int bw = (g_rw - 10) / 2, bh = 42;
    for (int i = 0; i < 4; i++) {
        g_pb[i][0] = g_rx + (i & 1) * (bw + 10); g_pb[i][1] = 70 + (i >> 1) * (bh + 8); g_pb[i][2] = bw; g_pb[i][3] = bh;
    }
    g_sb[0] = g_rx; g_sb[1] = h - 34 - 40 - 4; g_sb[2] = g_rw; g_sb[3] = 40;
}

static void button(const int *r, const char *label, const char *sub, int on) {
    rrect(r[0], r[1], r[2], r[3], 9, on ? ACCENT : PANEL);
    u32 fg = on ? WHITE : INK, fg2 = on ? WHITE : MUTED;
    int cx = r[0] + r[2] / 2;
    if (sub) {
        int lh = jt_text_height(JT_FACE_BOLD), sh = jt_text_height(JT_FACE_BODY);
        int y0 = r[1] + (r[3] - lh - sh) / 2;
        text_c(JT_FACE_BOLD, cx, y0, fg, label);
        text_c(JT_FACE_BODY, cx, y0 + lh, fg2, sub);
    } else {
        text_c(JT_FACE_BOLD, cx, r[1] + (r[3] - jt_text_height(JT_FACE_BOLD)) / 2, fg, label);
    }
}

static void draw(void) {
    cv.width = win.width; cv.height = win.height; cv.pitch = win.width;
    layout();
    int w = (int)cv.width, h = (int)cv.height;
    rect(0, 0, w, h, BG);

    /* the circle: a faint guide at full breath, the accent disc breathing inside it */
    disc(g_cx, g_cy, g_rmax * 16 + 16, GUIDE_LN);
    disc(g_cx, g_cy, g_rmax * 16, GUIDE);
    disc(g_cx, g_cy, radius16(ex_ms), ACCENT);

    struct where wh = where_at(ex_ms);
    int idle = (!running && ex_ms == 0);

    /* the right column */
    text(JT_FACE_BOLD, g_rx, 16, INK, "Windgate");
    text(JT_FACE_BODY, g_rx, 16 + jt_text_height(JT_FACE_BOLD), MUTED, "Guided breathing");
    for (int i = 0; i < 4; i++) button(g_pb[i], P_NAME[i], P_TIMES[i], i == preset);

    int cxr = g_rx + g_rw / 2;
    int py = g_pb[2][1] + g_pb[2][3] + 10;
    char line[40]; int n = 0;
    text_c(JT_FACE_BOLD, cxr, py, INK, idle ? "Ready" : PHASE_WORD[wh.phase]);
    py += jt_text_height(JT_FACE_BOLD) + 2;
    /* the display face's box is 66 tall with its digits 38 tall in the middle: draw it 5 up and advance by the digits */
    if (!idle) {
        catu(line, &n, (wh.len - wh.into + 999u) / 1000u);
        text_c(JT_FACE_DISPLAY, cxr, py - 5, ACCENT, line);
    } else {
        text_c(JT_FACE_BODY, cxr, py + 12, MUTED, "Press Start to begin");
    }
    py += 54;
    {   /* the web app's Rep counter, plus the session time */
        u32 s = ex_ms / 1000u;
        n = 0; line[0] = 0;
        cat(line, &n, "Round "); catu(line, &n, wh.round);
        cat(line, &n, "   "); catu(line, &n, s / 60u); cat(line, &n, ":");
        if (s % 60u < 10u) cat(line, &n, "0");
        catu(line, &n, s % 60u);
        text_c(JT_FACE_BODY, cxr, py, MUTED, line);
    }
    button(g_sb, idle ? "Start" : running ? "Pause" : "Resume", 0, 1);

    /* the keys, along the bottom */
    text_c(JT_FACE_BODY, w / 2, h - 28, MUTED, "Left and Right or 1 to 4: preset     Space or Enter: start and pause     Esc: close");

    /* the whole frame goes over the window in one quick pass */
    u32 cnt = win.width * win.height;
    for (u32 i = 0; i < cnt; i++) win.pixels[i] = cv.pixels[i];
}

/* One serial line per phase start, with the exact boundary time. Called every loop with the current exercise time. */
static void log_phases(void) {
    if (!running && ex_ms == 0) return;
    struct where w = where_at(ex_ms);
    if (w.seq == logged_seq) return;
    logged_seq = w.seq;
    char line[96]; int n = 0;
    cat(line, &n, "windgate: preset "); catu(line, &n, (u32)preset);
    cat(line, &n, " phase "); cat(line, &n, PHASE_LOG[w.phase]);
    cat(line, &n, " secs "); catu(line, &n, w.len / 1000u);
    cat(line, &n, " round "); catu(line, &n, w.round);
    cat(line, &n, " at "); catu(line, &n, w.began);
    cat(line, &n, "\n");
    jt_write(1, line, (unsigned)n);
}

static void set_preset(int p) {
    if (p < 0) p = 3;
    if (p > 3) p = 0;
    preset = p; ex_ms = 0; logged_seq = -1;
    char line[96]; int n = 0;
    cat(line, &n, "windgate: preset "); catu(line, &n, (u32)p); cat(line, &n, " selected "); cat(line, &n, P_NAME[p]);
    cat(line, &n, " pattern ");
    for (int i = 0; i < 4; i++) { catu(line, &n, P_SECS[p][i]); if (i < 3) cat(line, &n, ","); }
    cat(line, &n, "\n");
    jt_write(1, line, (unsigned)n);
}
static void toggle(void) {
    running = !running;
    char line[48]; int n = 0;
    cat(line, &n, running ? "windgate: running at " : "windgate: paused at "); catu(line, &n, ex_ms); cat(line, &n, "\n");
    jt_write(1, line, (unsigned)n);
}

static int hit(const int *r, int x, int y) { return x >= r[0] && y >= r[1] && x < r[0] + r[2] && y < r[1] + r[3]; }

__attribute__((section(".text.start"), used))
void _start(int argc, char **argv) {
    (void)argc; (void)argv;
    if (jt_window_open(&win) != 0 || !win.pixels) {
        jt_write(2, "windgate: no window\n", 20);
        jt_exit(1);
    }
    {   /* one line, one write: what tools/checks/ring3windgate-check.py asserts on */
        char line[48]; int n = 0;
        cat(line, &n, "windgate: ring-3 window "); catu(line, &n, win.width); cat(line, &n, "x"); catu(line, &n, win.height); cat(line, &n, "\n");
        jt_write(1, line, (unsigned)n);
    }
    {   /* the back buffer lives on the SYS_BRK heap */
        unsigned base = (unsigned)jt_brk(0), need = (BB_BYTES + 4095u) & ~4095u;
        if (win.width * win.height * 4u > BB_BYTES) { jt_write(2, "windgate: window too big\n", 25); jt_exit(1); }
        unsigned top = (unsigned)jt_brk(base + need); /* the heap lives at 0xFF000000: "negative" as an int, so errors are -4095..-1 only */
        if (top >= 0xFFFFF000u || top < base + need) { jt_write(2, "windgate: no heap\n", 18); jt_exit(1); }
        cv.pixels = (u32 *)base;
    }
    cv.width = win.width; cv.height = win.height; cv.pitch = win.width;
    layout();
    {   /* where everything is, so the check clicks and measures without re-deriving the layout */
        char line[200]; int n = 0;
        cat(line, &n, "windgate: layout circle "); catu(line, &n, (u32)g_cx); cat(line, &n, ","); catu(line, &n, (u32)g_cy);
        cat(line, &n, " rmin "); catu(line, &n, (u32)g_rmin); cat(line, &n, " rmax "); catu(line, &n, (u32)g_rmax);
        cat(line, &n, " presets");
        for (int i = 0; i < 4; i++) {
            cat(line, &n, " ");
            for (int k = 0; k < 4; k++) { catu(line, &n, (u32)g_pb[i][k]); if (k < 3) cat(line, &n, ","); }
        }
        cat(line, &n, " start ");
        for (int k = 0; k < 4; k++) { catu(line, &n, (u32)g_sb[k]); if (k < 3) cat(line, &n, ","); }
        cat(line, &n, "\n");
        jt_write(1, line, (unsigned)n);
    }
    set_preset(0);
    draw();

    unsigned flags = JT_POLL_PRESENT;
    u32 last = wall_ms(), next = 0;
    int was_ready = 0;
    for (;;) {
        struct jt_event ev;
        int r = jt_window_poll(&ev, flags);
        if (r == 1 && jt_window_resized(&ev, &win)) { draw(); flags = JT_POLL_PRESENT; continue; } /* JT_EV_RESIZE: remapped, repaint at the new size */
        flags = 0;
        u32 now = wall_ms();
        if (tps && !was_ready) {   /* the rate is known: from here the exercise clock counts */
            was_ready = 1;
            char line[64]; int n = 0;
            cat(line, &n, "windgate: clock ready ticks-per-second "); catu(line, &n, tps); cat(line, &n, "\n");
            jt_write(1, line, (unsigned)n);
        }
        if (running && was_ready) ex_ms += now - last;   /* exercise time only moves while running: a pause holds the circle where it is */
        last = now;
        log_phases();
        if (r == 1) {
            if (ev.kind == JT_EV_KEY) {
                int k = ev.a;
                if (k == JT_KEY_ESC) break;
                if (k == '`') {
                    jt_write(1, "windgate: crashing on purpose\n", 30);
                    *(volatile int *)0 = 1;
                }
                if (k == JT_KEY_LEFT) set_preset(preset - 1);
                else if (k == JT_KEY_RIGHT) set_preset(preset + 1);
                else if (k >= '1' && k <= '4') set_preset(k - '1');
                else if (k == ' ' || k == JT_KEY_ENTER) toggle();
            } else if (ev.kind == JT_EV_CLICK) {
                if (ev.a < 0 || ev.b < 0 || ev.a >= (int)win.width || ev.b >= (int)win.height) break; /* titlebar X */
                for (int i = 0; i < 4; i++) if (hit(g_pb[i], ev.a, ev.b)) set_preset(i);
                if (hit(g_sb, ev.a, ev.b)) toggle();
            }
            log_phases();
            draw(); flags = JT_POLL_PRESENT; next = now + 40;
            continue;
        }
        if (r != -11 /* -EAGAIN */) break;
        if (running && now >= next) { draw(); flags = JT_POLL_PRESENT; next = now + 40; }
        else jt_sched_yield();
    }
    say("windgate: closed\n");
    jt_exit(0);
}
