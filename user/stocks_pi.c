/* stocks_pi: Stocks for the ARM64 build (Pi and QEMU), a small ring-3 program: a watchlist on the left (symbol, name,
 * price, a green or red change pill, a tiny trend line) and an anti-aliased line chart of the selected symbol on the right.
 *
 * The i386 Stocks (user/stocks.c) is fed by the kernel's stocks_fetch, which the ARM kernel does not have, so this is its
 * own, smaller program with the same data contract: it reads STOCKS.TXT through jt_readfile (the card's JT/STOCKS.TXT,
 * the lines stocks.c documents: "r range sel", then per symbol "i stale price prev time dprice dprev n p0 .. pn-1", all
 * prices in cents). With no such file it draws a built-in sample and says so in its status line, "Sample, not live
 * prices". A file is called "Saved prices": this program never claims a live quote. Nothing is fetched here, no network,
 * no key.
 *
 * Up and Down pick a symbol, a click on a row picks it, Esc or the red dot closes it. One line per pick tells
 * tools/checks/arm64-stocks-check.py what was drawn.
 */
#include "jtsys.h"
#include "libjt/text.h"
#include "libjt/hires.h"

#define BG     0x00F5F0EB
#define SIDEBG 0x00F1ECE6
#define RULE   0x00E0D8CE
#define SELBG  0x00E2D9CC
#define INK    0x001C1C1E
#define MUTED  0x00807468
#define GREEN  0x0041854B
#define RED    0x00B51616
#define ROWS   8
#define PTS    32
#define SIDE_W 300
#define ROW_H  54
#define TOP    12

static struct jt_window_info win JT_DATA;
static char file[4096] JT_DATA;
static int sel JT_DATA, first JT_DATA, saved JT_DATA, range JT_DATA;
static int have[ROWS] JT_DATA, stale[ROWS] JT_DATA, price[ROWS] JT_DATA, prev[ROWS] JT_DATA, stamp[ROWS] JT_DATA;
static int dprice[ROWS] JT_DATA, dprev[ROWS] JT_DATA, np[ROWS] JT_DATA, pts[ROWS][PTS] JT_DATA;
static const char *const SYM[ROWS] = {"AAPL", "MSFT", "GOOGL", "AMZN", "TSLA", "NVDA", "META", "NFLX"};
static const char *const NAME[ROWS] = {"Apple Inc.", "Microsoft", "Alphabet Inc.", "Amazon.com", "Tesla Inc.", "NVIDIA", "Meta Platforms", "Netflix"};
static const char *const RNAME[5] = {"1D", "1W", "1M", "3M", "1Y"};
/* The sample: a start price in cents and a drift per step that makes some rows rise and some fall. */
static const int BASE[ROWS] = {18250, 41020, 16880, 18640, 24310, 11980, 49350, 62500};
static const int DRIFT[ROWS] = {30, -45, 22, 60, -90, 120, -35, 80};

static int cat(char *b, int p, const char *s) { while (*s && p < 62) b[p++] = *s++; b[p] = 0; return p; }
static int num(const char **p) {
    int v = 0;
    while (**p == ' ') (*p)++;
    while (**p >= '0' && **p <= '9') v = v * 10 + *(*p)++ - '0';
    return v;
}
static void eol(const char **p) { while (**p && **p != '\n') (*p)++; if (**p) (*p)++; }
static void load(void) {
    int n = jt_readfile("STOCKS.TXT", file, sizeof file - 1);
    file[n < 0 ? 0 : n] = 0;
    const char *p = file;
    if (*p == 'r') {
        p++; range = num(&p); sel = num(&p);
        if (range < 0 || range >= 5) range = 0;
        if (sel < 0 || sel >= ROWS) sel = 0;
        eol(&p);
        while (*p) {
            int i = num(&p);
            if (i < 0 || i >= ROWS) { eol(&p); continue; }
            stale[i] = num(&p); price[i] = num(&p); prev[i] = num(&p); stamp[i] = num(&p);
            dprice[i] = num(&p); dprev[i] = num(&p); np[i] = num(&p);
            if (np[i] > PTS) np[i] = PTS;
            for (int j = 0; j < np[i]; j++) pts[i][j] = num(&p);
            have[i] = np[i] > 1;
            if (have[i]) saved++;
            eol(&p);
        }
    }
    if (saved) return;
    /* No saved prices: a sample walk per symbol, from a fixed seed so it looks the same every start. */
    unsigned s = 12345;
    for (int i = 0; i < ROWS; i++) {
        int v = BASE[i];
        np[i] = PTS; have[i] = 1; stale[i] = 0; stamp[i] = 0;
        for (int j = 0; j < PTS; j++) {
            s = s * 1103515245u + 12345u;
            v += DRIFT[i] + (int)((s >> 16) % 161) - 80 + (j % 7 == 3 ? DRIFT[i] * 3 : 0);
            pts[i][j] = v;
        }
        price[i] = pts[i][PTS - 1]; prev[i] = pts[i][PTS - 2]; dprice[i] = price[i]; dprev[i] = pts[i][0];
    }
}

static void rect(int x, int y, int w, int h, unsigned c) { jt_fill(&win, x, y, w, h, c); }   /* hires.h: logical pixels, sharp at scale 2 */
static int text(const char *s, int x, int y, unsigned fg, int bold) { return jt_text_draw(&win, bold ? JT_FACE_BOLD : JT_FACE_BODY, x, y, fg, s); }
static int tw(const char *s, int bold) { return jt_text_width(bold ? JT_FACE_BOLD : JT_FACE_BODY, s); }

/* Antialiased line, in device pixels (hires.h: a logical pixel is k by k of them): each step spreads its coverage over the
   four nearest pixels. At scale 1 these are the logical pixels the line always used. */
static void blend(int x, int y, unsigned c, int a) {
    int k = jt_scale(&win), W = (int)win.width * k, H = (int)win.height * k;
    if (x < 0 || y < 0 || x >= W || y >= H || a <= 0) return;
    unsigned *d = &win.pixels[(unsigned)y * (unsigned)W + (unsigned)x], o = *d, r = 0;
    for (int s = 0; s < 24; s += 8) r |= ((((c >> s) & 255) * (unsigned)a + ((o >> s) & 255) * (unsigned)(255 - a)) / 255) << s;
    *d = r;
}
static void line(int x0, int y0, int x1, int y1, unsigned c) {   /* logical end points; drawn from the pixel centres' device positions */
    int k = jt_scale(&win);
    x0 = x0 * k + (k - 1) / 2; y0 = y0 * k + (k - 1) / 2; x1 = x1 * k + (k - 1) / 2; y1 = y1 * k + (k - 1) / 2;
    int dx = x1 - x0, dy = y1 - y0, n = (dx < 0 ? -dx : dx) > (dy < 0 ? -dy : dy) ? (dx < 0 ? -dx : dx) : (dy < 0 ? -dy : dy);
    for (int o = 0; o < 2 * k; o++) {   /* two logical pixels thick */
        if (!n) { blend(x0, y0 + o, c, 255); continue; }
        for (int i = 0; i <= n; i++) {
            int fx = x0 * 256 + dx * 256 * i / n, fy = (y0 + o) * 256 + dy * 256 * i / n;
            int ix = fx >> 8, iy = fy >> 8, ax = fx & 255, ay = fy & 255;
            blend(ix, iy, c, (256 - ax) * (256 - ay) >> 8); blend(ix + 1, iy, c, ax * (256 - ay) >> 8);
            blend(ix, iy + 1, c, (256 - ax) * ay >> 8); blend(ix + 1, iy + 1, c, ax * ay >> 8);
        }
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
        line(px, py, cx, cy, c); px = cx; py = cy;
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

static void draw(void) {
    int WW = (int)win.width, HH = (int)win.height, vis = rows_visible(), px = SIDE_W + 24, pw = WW - px - 24;
    char b[64];
    if (sel < first) first = sel;
    if (sel >= first + vis) first = sel - vis + 1;
    rect(0, 0, WW, HH, BG);
    rect(0, 0, SIDE_W, HH, SIDEBG);
    rect(SIDE_W, 0, 1, HH, RULE);
    text("Watchlist", 20, TOP, INK, 1);
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
        int pw2 = tw(b, 0) + 12;
        rect(SIDE_W - 16 - pw2, y + 26, pw2, 20, col);
        text(b, SIDE_W - 16 - pw2 + 6, y + 28, 0x00FFFFFF, 0);
    }
    text(SYM[sel], px, TOP, MUTED, 0);
    text(NAME[sel], px, TOP + 22, INK, 1);
    int n = have[sel] ? np[sel] : 0, delta = 0;
    if (n) delta = pts[sel][n - 1] - pts[sel][0];
    unsigned col = delta >= 0 ? GREEN : RED;
    if (n) {
        fmt_price(price[sel], b); text(b, px, TOP + 50, INK, 1);
        fmt_signed(delta, 1, b); int dx = text(b, px, TOP + 72, col, 0);   /* its own row: the window can be narrow */
        fmt_pct(bp_of(delta, pts[sel][n - 1]), b); text(b, dx + 14, TOP + 72, col, 0);
    } else text("Prices unavailable", px, TOP + 50, MUTED, 0);
    int cy0 = TOP + 108, ch = HH - cy0 - 72;
    if (ch < 60) ch = 60;
    rect(px, cy0 - 4, pw, 1, RULE);
    chart(px, cy0, pw, ch, pts[sel], n, col);
    rect(px, cy0 + ch + 4, pw, 1, RULE);
    int sy = cy0 + ch + 14;
    if (saved) { cat(b, cat(b, 0, "Saved prices / USD / range "), RNAME[range]); text(b, px, sy, MUTED, 0); }
    else text("Sample, not live prices", px, sy, MUTED, 0);
    text("Up and Down pick a symbol", px, sy + 22, MUTED, 0);
    int ln = cat(b, cat(b, 0, "stocks: sel "), SYM[sel]); b[ln++] = '\n'; jt_write(1, b, (unsigned)ln);
}

__attribute__((section(".text.start"), used))
void _start(int argc, char **argv) {
    (void)argc; (void)argv;
    if (jt_window_open_hires(&win) != 0 || !win.pixels) { jt_write(2, "stocks: no window\n", 18); jt_exit(1); }
    load();
    jt_write(1, saved ? "stocks: saved prices\n" : "stocks: sample, not live prices\n", saved ? 21 : 32);
    draw();
    struct jt_event ev;
    unsigned flags = JT_POLL_PRESENT;
    for (;;) {
        int r = jt_window_poll(&ev, flags);
        if (r == 1 && jt_window_resized(&ev, &win)) { draw(); flags = JT_POLL_PRESENT; continue; }
        flags = 0;
        if (r == -11 /* -EAGAIN */) { jt_sched_yield(); continue; }
        if (r != 1) break;
        if (ev.kind == JT_EV_CLICK) {
            int mx = ev.a, my = ev.b;
            if (mx < 0 || my < 0 || mx >= (int)win.width || my >= (int)win.height) break;   /* chrome X or dock; clicks inside never close */
            if (mx < SIDE_W && my >= TOP + 28) {
                int row = (my - (TOP + 28)) / ROW_H;
                if (first + row < ROWS && row < rows_visible()) { sel = first + row; draw(); }
            }
        } else if (ev.kind == JT_EV_KEY) {
            if (ev.a == JT_KEY_ESC) break;
            if (ev.a == JT_KEY_UP && sel > 0) { sel--; draw(); }
            else if (ev.a == JT_KEY_DOWN && sel < ROWS - 1) { sel++; draw(); }
        }
        flags = JT_POLL_PRESENT;
    }
    jt_write(1, "stocks: closed\n", 15);
    jt_exit(0);
}
