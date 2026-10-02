/* stocks: the basic watchlist and chart, as a ring-3 program.
 *
 * The kernel's stocks_fetch still owns the network (SYS_HTTP_GET caps a body
 * at 2KB and /api/stocks is bigger). After every fetch it writes STOCKS.TXT,
 * and this program only reads that file: line 1 "r range sel", then per
 * symbol "i stale price prev time dprice dprev n p0 .. pn-1".
 *
 * Up/down pick a symbol, left/right pick a range, R refreshes, Esc exits,
 * backquote crashes on purpose like every ring-3 app. A range change or R
 * calls SYS_REFRESH (range | sel << 8); the desktop loop refetches, rewrites the file
 * and bumps jt_sysinfo.data_stamp, which this program polls to reload and redraw.
 * Clicks inside the window pick rows and tabs and never close it; only a
 * click outside the window (chrome X, dock) does.
 */
#include "jtsys.h"
#include "libjt/text.h"

#define BG     0x00F5F0EB
#define SIDEBG 0x00F1ECE6
#define RULE   0x00E0D8CE
#define SELBG  0x00E2D9CC
#define INK    0x001C1C1E
#define MUTED  0x00807468
#define GREEN  0x0041854B
#define RED    0x00B51616
#define ROWS   8
#define RANGES 5
#define PTS    32
#define SIDE_W 300
#define ROW_H  54
#define TOP    12

static struct jt_window_info win JT_DATA;
static char file[4096] JT_DATA;
static int range JT_DATA, sel JT_DATA, first JT_DATA;
static int have[ROWS] JT_DATA, stale[ROWS] JT_DATA, price[ROWS] JT_DATA, prev[ROWS] JT_DATA, stamp[ROWS] JT_DATA;
static int dprice[ROWS] JT_DATA, dprev[ROWS] JT_DATA, np[ROWS] JT_DATA, pts[ROWS][PTS] JT_DATA;
static const char *const SYM[ROWS] = {"AAPL", "MSFT", "GOOGL", "AMZN", "TSLA", "NVDA", "META", "NFLX"};
static const char *const NAME[ROWS] = {"Apple Inc.", "Microsoft", "Alphabet Inc.", "Amazon.com", "Tesla Inc.", "NVIDIA", "Meta Platforms", "Netflix"};
static const char *const RNAME[RANGES] = {"1D", "1W", "1M", "3M", "1Y"};

static int cat(char *b, int p, const char *s) { while (*s && p < 62) b[p++] = *s++; b[p] = 0; return p; }
static int num(const char **p) {
    int v = 0;
    while (**p == ' ') (*p)++;
    while (**p >= '0' && **p <= '9') v = v * 10 + *(*p)++ - '0';
    return v;
}
static void eol(const char **p) { while (**p && **p != '\n') (*p)++; if (**p) (*p)++; }
static void load(void) {
    int fd = jt_open("STOCKS.TXT", JT_O_RDONLY), n = fd < 0 ? 0 : jt_read(fd, file, sizeof file - 1);
    if (fd >= 0) jt_close(fd);
    file[n < 0 ? 0 : n] = 0;
    const char *p = file;
    if (*p != 'r') return;
    p++; range = num(&p); sel = num(&p);
    if (range < 0 || range >= RANGES) range = 0;
    if (sel < 0 || sel >= ROWS) sel = 0;
    eol(&p);
    while (*p) {
        int i = num(&p);
        if (i < 0 || i >= ROWS) { eol(&p); continue; }
        stale[i] = num(&p); price[i] = num(&p); prev[i] = num(&p); stamp[i] = num(&p);
        dprice[i] = num(&p); dprev[i] = num(&p); np[i] = num(&p);
        if (np[i] > PTS) np[i] = PTS;
        for (int j = 0; j < np[i]; j++) pts[i][j] = num(&p);
        have[i] = np[i] > 0;
        eol(&p);
    }
}

static void pset(int x, int y, unsigned c) {
    if (x >= 0 && y >= 0 && x < (int)win.width && y < (int)win.height) win.pixels[(unsigned)y * win.width + (unsigned)x] = c;
}
static void rect(int x, int y, int w, int h, unsigned c) { for (int j = 0; j < h; j++) for (int i = 0; i < w; i++) pset(x + i, y + j, c); }
static int text(const char *s, int x, int y, unsigned fg, int bold) { return jt_text_draw(&win, bold ? JT_FACE_BOLD : JT_FACE_BODY, x, y, fg, s); }
static int tw(const char *s, int bold) { return jt_text_width(bold ? JT_FACE_BOLD : JT_FACE_BODY, s); }

/* Antialiased line: each step spreads its coverage over the four nearest pixels. */
static void blend(int x, int y, unsigned c, int a) {
    if (x < 0 || y < 0 || x >= (int)win.width || y >= (int)win.height || a <= 0) return;
    unsigned *d = &win.pixels[(unsigned)y * win.width + (unsigned)x], o = *d, r = 0;
    for (int s = 0; s < 24; s += 8) r |= ((((c >> s) & 255) * (unsigned)a + ((o >> s) & 255) * (unsigned)(255 - a)) / 255) << s;
    *d = r;
}
static void line(int x0, int y0, int x1, int y1, unsigned c) {
    int dx = x1 - x0, dy = y1 - y0, n = (dx < 0 ? -dx : dx) > (dy < 0 ? -dy : dy) ? (dx < 0 ? -dx : dx) : (dy < 0 ? -dy : dy);
    if (!n) { blend(x0, y0, c, 255); return; }
    for (int i = 0; i <= n; i++) {
        int fx = x0 * 256 + dx * 256 * i / n, fy = y0 * 256 + dy * 256 * i / n;
        int ix = fx >> 8, iy = fy >> 8, ax = fx & 255, ay = fy & 255;
        blend(ix, iy, c, (256 - ax) * (256 - ay) >> 8); blend(ix + 1, iy, c, ax * (256 - ay) >> 8);
        blend(ix, iy + 1, c, (256 - ax) * ay >> 8); blend(ix + 1, iy + 1, c, ax * ay >> 8);
    }
}
/* 32-bit only: no __divdi3 in a flat binary, so shrink both terms until the product fits. */
static int scale(int d, int span, int h) { while (span > (1 << 20)) { d >>= 1; span >>= 1; } return span ? d * h / span : 0; }
static void chart(int x, int y, int w, int h, const int *v, int n, unsigned c) {
    if (n < 2) return;
    int lo = v[0], hi = v[0];
    for (int i = 1; i < n; i++) { if (v[i] < lo) lo = v[i]; if (v[i] > hi) hi = v[i]; }
    if (hi - lo < 4) { hi += 2; lo -= 2; }
    int px = x, py = y + h - 1 - scale(v[0] - lo, hi - lo, h - 1);
    for (int i = 1; i < n; i++) {
        int cx = x + i * (w - 1) / (n - 1), cy = y + h - 1 - scale(v[i] - lo, hi - lo, h - 1);
        line(px, py, cx, cy, c); line(px, py + 1, cx, cy + 1, c); px = cx; py = cy;
    }
}

static void fmt_price(int x100, char *b) {
    char t[12]; int tn = 0, p = 0, d = x100 / 100, c = x100 % 100;
    if (!d) t[tn++] = '0';
    while (d) { t[tn++] = (char)('0' + d % 10); d /= 10; }
    while (tn) { b[p++] = t[--tn]; if (tn && tn % 3 == 0) b[p++] = ','; }
    b[p++] = '.'; b[p++] = (char)('0' + c / 10); b[p++] = (char)('0' + c % 10); b[p] = 0;
}
static void fmt_signed(int x100, int dollar, char *b) {
    int p = cat(b, 0, x100 < 0 ? "-" : "+"); char t[20];
    if (dollar) p = cat(b, p, "$");
    fmt_price(x100 < 0 ? -x100 : x100, t); cat(b, p, t);
}
static void fmt_pct(int bp, char *b) {
    int p = cat(b, 0, bp < 0 ? "-" : "+"); char t[20];
    fmt_price(bp < 0 ? -bp : bp, t); p = cat(b, p, t); cat(b, p, "%");
}
static int bp_of(int change, int now) {
    int pv = now - change;
    while ((change < 0 ? -change : change) > 200000) { change /= 2; pv /= 2; }
    return pv > 0 ? change * 10000 / pv : 0;
}

static int rows_visible(void) { int n = ((int)win.height - (TOP + 28) - 8) / ROW_H; return n < 1 ? 1 : n; }
static int tab_x(int r) { return SIDE_W + 24 + r * 56; }
static int tab_y(void) { return TOP + 80; }

static void draw(int fetching) {
    int WW = (int)win.width, HH = (int)win.height, vis = rows_visible(), px = SIDE_W + 24, pw = WW - px - 24;
    char b[64];
    if (sel < first) first = sel;
    if (sel >= first + vis) first = sel - vis + 1;
    rect(0, 0, WW, HH, BG);
    rect(0, 0, SIDE_W, HH, SIDEBG);
    rect(SIDE_W, 0, 1, HH, RULE);
    text("Watchlist", 20, TOP, INK, 1);
    text(fetching ? "Updating..." : "R refresh", 190, TOP, MUTED, 0);

    for (int r = 0; r < vis && first + r < ROWS; r++) {
        int i = first + r, y = TOP + 28 + r * ROW_H;
        if (i == sel) rect(8, y, SIDE_W - 16, ROW_H - 4, SELBG);
        text(SYM[i], 20, y + 6, INK, 1);
        text(NAME[i], 20, y + 28, MUTED, 0);
        if (!have[i]) { text("--", SIDE_W - 40, y + 6, MUTED, 0); continue; }
        unsigned col = price[i] >= prev[i] ? GREEN : RED;
        chart(118, y + 10, 60, 26, pts[i], np[i], col);
        fmt_price(price[i], b);
        text(b, SIDE_W - 16 - tw(b, 0), y + 6, INK, 0);
        fmt_pct(bp_of(dprice[i] - dprev[i], dprice[i]), b);
        if (range != 0 || stale[i]) cat(b, 0, stale[i] ? "stale" : "USD");
        int pw2 = tw(b, 0) + 12;
        rect(SIDE_W - 16 - pw2, y + 26, pw2, 20, col);
        text(b, SIDE_W - 16 - pw2 + 6, y + 28, 0x00FFFFFF, 0);
    }

    text(SYM[sel], px, TOP, MUTED, 0);
    text(NAME[sel], px, TOP + 22, INK, 1);
    int n = have[sel] ? np[sel] : 0, delta = 0;
    if (n) delta = range == 0 ? price[sel] - prev[sel] : pts[sel][n - 1] - pts[sel][0];
    unsigned col = delta >= 0 ? GREEN : RED;
    if (n) {
        fmt_price(price[sel], b); text(b, px, TOP + 50, INK, 1);
        fmt_signed(delta, 1, b); text(b, px + 120, TOP + 50, col, 0);
        fmt_pct(bp_of(delta, range == 0 ? price[sel] : pts[sel][n - 1]), b); text(b, px + 220, TOP + 50, col, 0);
    } else text("Prices unavailable", px, TOP + 50, MUTED, 0);

    int ty = tab_y();
    for (int r = 0; r < RANGES; r++) {
        if (r == range) rect(tab_x(r) - 4, ty, 48, 24, SELBG);
        text(RNAME[r], tab_x(r) + 8, ty + 3, r == range ? INK : MUTED, 0);
    }
    int cy0 = ty + 32, ch = HH - cy0 - 96;
    if (ch < 60) ch = 60;
    rect(px, cy0 - 4, pw, 1, RULE);
    chart(px, cy0, pw, ch, pts[sel], n, col);
    rect(px, cy0 + ch + 4, pw, 1, RULE);
    int sy = cy0 + ch + 14;
    text(n ? (stale[sel] ? "Stale quote. R to retry." : "Yahoo Finance / USD / may be delayed") : "R to retry. Quote service unavailable.", px, sy, MUTED, 0);
    if (n) { /* provider time as hh:mm UTC */
        int hr = stamp[sel] / 3600 % 24, mi = stamp[sel] / 60 % 60, p = cat(b, 0, "As of ");
        b[p++] = (char)('0' + hr / 10); b[p++] = (char)('0' + hr % 10); b[p++] = ':'; b[p++] = (char)('0' + mi / 10); b[p++] = (char)('0' + mi % 10); b[p] = 0;
        cat(b, p, " UTC");
        text(b, px, sy + 22, MUTED, 0);
    }
}

static void ask(void) { jt_refresh(JT_REFRESH_STOCKS, range | sel << 8); }

__attribute__((section(".text.start"), used))
void _start(int argc, char **argv) {
    (void)argc; (void)argv;
    if (jt_window_open(&win) != 0 || !win.pixels) { jt_write(2, "stocks: no window\n", 18); jt_exit(1); }
    load();
    draw(0);
    struct jt_sysinfo si;
    unsigned stamp = 0;
    if (jt_sysinfo(&si) > 0) stamp = si.data_stamp;
    ask();
    struct jt_event ev;
    unsigned flags = JT_POLL_PRESENT;
    for (;;) {
        int r = jt_window_poll(&ev, flags);
        if (r == 1 && jt_window_resized(&ev, &win)) { draw(0); flags = JT_POLL_PRESENT; continue; } /* JT_EV_RESIZE: remapped, repaint at the new size */
        flags = 0;
        if (r == -11 /* -EAGAIN */) {
            if (jt_sysinfo(&si) > 0 && si.data_stamp != stamp) { int keep = sel; stamp = si.data_stamp; load(); sel = keep; draw(0); flags = JT_POLL_PRESENT; }
            jt_sched_yield(); continue;
        }
        if (r != 1) break;
        if (ev.kind == JT_EV_CLICK) {
            int mx = ev.a, my = ev.b;
            if (mx < 0 || my < 0 || mx >= (int)win.width || my >= (int)win.height) break; /* chrome X or dock; clicks inside never close */
            if (mx < SIDE_W && my >= TOP + 28) {
                int row = (my - (TOP + 28)) / ROW_H;
                if (first + row < ROWS && row < rows_visible()) { sel = first + row; draw(0); }
            } else if (my >= tab_y() && my < tab_y() + 24) {
                for (int t = 0; t < RANGES; t++)
                    if (mx >= tab_x(t) - 4 && mx < tab_x(t) + 44 && t != range) { range = t; draw(1); jt_window_poll(&ev, JT_POLL_PRESENT); ask(); }
            }
        } else if (ev.kind == JT_EV_KEY) {
            if (ev.a == '`') { jt_write(1, "stocks: crashing on purpose\n", 28); *(volatile int *)0 = 1; }
            if (ev.a == JT_KEY_ESC) break;
            if (ev.a == JT_KEY_UP && sel > 0) { sel--; draw(0); }
            else if (ev.a == JT_KEY_DOWN && sel < ROWS - 1) { sel++; draw(0); }
            else if (ev.a == JT_KEY_LEFT && range > 0) { range--; draw(1); jt_window_poll(&ev, JT_POLL_PRESENT); ask(); }
            else if (ev.a == JT_KEY_RIGHT && range < RANGES - 1) { range++; draw(1); jt_window_poll(&ev, JT_POLL_PRESENT); ask(); }
            else if (ev.a == 'r' || ev.a == 'R') { draw(1); jt_window_poll(&ev, JT_POLL_PRESENT); ask(); }
        }
        flags = JT_POLL_PRESENT;
    }
    jt_exit(0);
}
