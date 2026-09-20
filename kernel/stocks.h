/* v0.87+: Stocks, rebuilt in the shape of macOS's own Stocks app: a watchlist
   sidebar (symbol, name, sparkline, price, colored change pill) beside a
   detail pane (big price, range tabs 1D/1W/1M/3M/1Y, a real line chart, and
   the Open/High/Low/Mkt Cap/P/E stats grid). Epiphany (epiphany.h) is the
   other, richer take on the same data; this one stays the basic native one.

   Data is baked-in demo data, clearly labeled, not live: every plain-HTTP
   quote source forces HTTPS and this kernel has no TLS (roadmap.md has the
   real curl tests). Chart history is not stored either: each range is a
   deterministic seeded random walk pinned to the stock's real baked-in price
   at the right edge (and, for 1D, to price minus change at the left edge), so
   the picture is stable across opens and always agrees with the numbers.

   Keys: up/down pick a stock, left/right change range, esc closes. Clicking
   a watchlist row or a range tab does the same; the red dot closes. */

#define STOCKS_MAX 8
#define STOCKS_SYMBOL_MAX 8
#define STOCKS_NAME_MAX 32

typedef struct {
    char symbol[STOCKS_SYMBOL_MAX];     /* e.g. "AAPL" */
    char name[STOCKS_NAME_MAX];         /* e.g. "Apple Inc." */
    int price_x100;                     /* $150.42 as 15042, so price is always x100 for exact cents */
    int change_x100;                    /* +$5.34 as +534, negative for down */
    int cap_b;                          /* market cap, $ billions */
    int pe_x10;                         /* P/E ratio x10 */
} stocks_entry_t;

static stocks_entry_t stocks_entries[STOCKS_MAX] = {
    {"AAPL", "Apple Inc.", 23800, 250, 3600, 312},
    {"MSFT", "Microsoft", 41900, -180, 3100, 350},
    {"GOOGL", "Alphabet Inc.", 14200, 350, 1750, 245},
    {"AMZN", "Amazon.com", 19100, -320, 2000, 340},
    {"TSLA", "Tesla Inc.", 24200, 870, 780, 610},
    {"NVDA", "NVIDIA", 12800, 410, 3100, 550},
    {"META", "Meta Platforms", 58000, -640, 1480, 280},
    {"NFLX", "Netflix", 66000, 1180, 290, 470},
};

static void stocks_format_price(int x100, char *buf, int max) {
    /* Format 15042 as "150.42" */
    int dollars = x100 / 100;
    int cents = x100 % 100;
    int pos = 0;

    if (dollars == 0) {
        buf[pos++] = '0';
    } else {
        char tmp[16]; int ti = 0;
        int v = dollars;
        while (v > 0) { tmp[ti++] = '0' + v % 10; v /= 10; }
        while (ti > 0 && pos < max - 1) { buf[pos++] = tmp[--ti]; if (ti > 0 && ti % 3 == 0 && pos < max - 1) buf[pos++] = ','; } /* thousands separators */
    }

    if (pos < max - 1) buf[pos++] = '.';
    if (pos < max - 1) buf[pos++] = '0' + (cents / 10);
    if (pos < max - 1) buf[pos++] = '0' + (cents % 10);

    if (pos >= max) pos = max - 1;
    buf[pos] = 0;
}

static void stocks_format_change(int x100, int *out_dollars, int *out_cents, int *out_sign) {
    *out_sign = (x100 < 0) ? -1 : 1;
    x100 = x100 < 0 ? -x100 : x100;
    *out_dollars = x100 / 100;
    *out_cents = x100 % 100;
}

/* ---- shared drawing/formatting helpers (Epiphany uses these too) ---- */
#define STX_GREEN 0x0041854B
#define STX_RED   0x00B51616
#define STX_INK   0x001C1C1E
#define STX_MUTED 0x00807468

static int font_strlen_local(const char *s) { int n = 0; while (s[n]) n++; return n; }
static int stx_cat(char *b, int pos, const char *s) { while (*s && pos < 62) b[pos++] = *s++; b[pos] = 0; return pos; }
/* "+1.23" / "-4.50" with an optional leading $ (dollar=1) */
static void stx_signed(int x100, int dollar, char *b) {
    int p = 0; b[0] = 0;
    p = stx_cat(b, p, x100 < 0 ? "-" : "+");
    if (dollar) p = stx_cat(b, p, "$");
    char t[20]; stocks_format_price(x100 < 0 ? -x100 : x100, t, sizeof t);
    stx_cat(b, p, t);
}
/* basis points as "+1.06%" */
static void stx_pct(int bp, char *b) {
    int p = stx_cat(b, 0, bp < 0 ? "-" : "+");
    char t[20]; stocks_format_price(bp < 0 ? -bp : bp, t, sizeof t);
    p = stx_cat(b, p, t); stx_cat(b, p, "%");
}
static int stx_bp(int change_x100, int price_x100) {
    int prev = price_x100 - change_x100;
    return prev > 0 ? (int)(change_x100 * 10000 / prev) : 0;
}
static void stx_line(int x0, int y0, int x1, int y1, unsigned int c) {
    int dx = x1 > x0 ? x1 - x0 : x0 - x1, sx = x0 < x1 ? 1 : -1;
    int dy = y1 > y0 ? y0 - y1 : y1 - y0, sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    for (;;) {
        window_rect(x0, y0, 2, 2, c);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}
/* Line chart of v[0..n) scaled into the box; lo/hi padded so a flat series still draws. */
static void stx_chart(int x, int y, int w, int h, const int *v, int n, unsigned int c) {
    int lo = v[0], hi = v[0];
    for (int i = 1; i < n; i++) { if (v[i] < lo) lo = v[i]; if (v[i] > hi) hi = v[i]; }
    if (hi - lo < 4) { hi += 2; lo -= 2; }
    int px = x, py = y + h - 1 - (int)((v[0] - lo) * (h - 1) / (hi - lo));
    for (int i = 1; i < n; i++) {
        int cx = x + i * (w - 1) / (n - 1);
        int cy = y + h - 1 - (int)((v[i] - lo) * (h - 1) / (hi - lo));
        stx_line(px, py, cx, cy, c); px = cx; py = cy;
    }
}
static unsigned int stx_rng;
static int stx_rand(int m) { stx_rng = stx_rng * 1103515245u + 12345u; return (int)((stx_rng >> 16) & 0x7FFF) % m; }

#define STX_RANGES 5
#define STX_MAXPTS 64
static const char *stx_range_name[STX_RANGES] = {"1D", "1W", "1M", "3M", "1Y"};
static const int stx_range_n[STX_RANGES]   = {48, 35, 30, 45, 60};
static const int stx_range_vol[STX_RANGES] = {25, 60, 90, 120, 150};   /* per-step move, basis points */
static const int stx_range_drift[STX_RANGES] = {0, 300, 800, 1500, 3500}; /* max start-vs-now swing, basis points */

/* Deterministic history for one stock and range; last point is exactly the
   baked-in price. Returns the point count. */
static int stx_series(int idx, int range, int *out) {
    stocks_entry_t *s = &stocks_entries[idx];
    int n = stx_range_n[range];
    stx_rng = (unsigned)(idx * 7919 + range * 104729 + 17);
    int start = s->price_x100 - s->change_x100;
    if (range > 0) {
        int d = stx_rand(2 * stx_range_drift[range] + 1) - stx_range_drift[range] / 2;
        start = (int)(s->price_x100 * (10000 - d) / 10000);
    }
    int vol = (int)(s->price_x100 * stx_range_vol[range] / 10000);
    if (vol < 2) vol = 2;
    int v = start;
    for (int i = 0; i < n; i++) { out[i] = v; v += stx_rand(2 * vol + 1) - vol; }
    int last = out[n - 1];
    for (int i = 0; i < n; i++) {
        out[i] += (int)((s->price_x100 - last) * i / (n - 1));
        if (out[i] < 100) out[i] = 100;
    }
    out[n - 1] = s->price_x100;
    return n;
}

#define STX_SIDE_W 300
#define STX_ROW_H  54

/* Windowed apps have no titlebar strip to clear, full-screen ones do. */
static int stx_top(void) { return gui_app_windowed ? 8 : 40; }
static int stx_row_y0(void) { return stx_top() + 28; }
static int stx_rows_visible(void) { int n = ((int)window_height() - stx_row_y0() - 8) / STX_ROW_H; return n < 1 ? 1 : n; }
static int stx_pane_x(void) { return STX_SIDE_W + 24; }
static int stx_tab_x(int r) { return stx_pane_x() + r * 56; }
static int stx_tab_y(void) { return stx_top() + 80; }

static void stocks_draw(int sel, int range, int first) {
    int WW = (int)window_width(), HH = (int)window_height(), T = stx_top();
    window_clear(GUI_BG);
    gui_draw_app_titlebar("Stocks");
    window_rect(0, T - 4, STX_SIDE_W, HH - T + 4, 0x00F1ECE6);
    window_rect(STX_SIDE_W, T - 4, 1, HH - T + 4, 0x00E0D8CE);
    font_draw_string("Watchlist", 20, T, STX_INK, -1);
    font_draw_string("demo data", STX_SIDE_W - 16 - font_string_width("demo data"), T, STX_MUTED, -1);

    for (int r = 0; r < stx_rows_visible() && first + r < STOCKS_MAX; r++) {
        int i = first + r;
        stocks_entry_t *s = &stocks_entries[i];
        int y = stx_row_y0() + r * STX_ROW_H;
        if (i == sel) window_rect(8, y, STX_SIDE_W - 16, STX_ROW_H - 4, 0x00E2D9CC);
        font_draw_string(s->symbol, 20, y + 8, STX_INK, -1);
        font_draw_string(s->name, 20, y + 28, STX_MUTED, -1);
        int pts[STX_MAXPTS]; int n = stx_series(i, 0, pts);
        unsigned int col = s->change_x100 >= 0 ? STX_GREEN : STX_RED;
        stx_chart(118, y + 10, 60, 26, pts, n, col);
        char b[64]; stocks_format_price(s->price_x100, b, sizeof b);
        font_draw_string(b, STX_SIDE_W - 16 - font_string_width(b), y + 6, STX_INK, -1);
        stx_pct(stx_bp(s->change_x100, s->price_x100), b);
        int pw = font_string_width(b) + 12;
        window_rect(STX_SIDE_W - 16 - pw, y + 26, pw, 20, col);
        font_draw_string(b, STX_SIDE_W - 16 - pw + 6, y + 28, 0x00FFFFFF, -1);
    }

    stocks_entry_t *s = &stocks_entries[sel];
    int px = stx_pane_x(), pw = WW - px - 24;
    font_draw_string(s->symbol, px, T, STX_MUTED, -1);
    font_draw_string(s->name, px, T + 22, STX_INK, -1);

    int pts[STX_MAXPTS]; int n = stx_series(sel, range, pts);
    int delta = pts[n - 1] - pts[0];
    unsigned int col = delta >= 0 ? STX_GREEN : STX_RED;
    char b[64];
    stocks_format_price(s->price_x100, b, sizeof b);
    font_draw_string(b, px, T + 50, STX_INK, -1);
    stx_signed(delta, 1, b);
    font_draw_string(b, px + 120, T + 50, col, -1);
    stx_pct(stx_bp(delta, pts[n - 1]), b);
    font_draw_string(b, px + 220, T + 50, col, -1);

    int ty = stx_tab_y();
    for (int r = 0; r < STX_RANGES; r++) {
        int x = stx_tab_x(r);
        if (r == range) window_rect(x - 4, ty, 48, 24, 0x00E2D9CC);
        font_draw_string(stx_range_name[r], x + 8, ty + 4, r == range ? STX_INK : STX_MUTED, -1);
    }
    int cy0 = ty + 32, ch = HH - cy0 - 96; if (ch < 60) ch = 60;
    window_rect(px, cy0 - 4, pw, 1, 0x00E0D8CE);
    stx_chart(px, cy0, pw, ch, pts, n, col);
    window_rect(px, cy0 + ch + 4, pw, 1, 0x00E0D8CE);

    /* stats grid from the 1D series */
    int d1[STX_MAXPTS]; int dn = stx_series(sel, 0, d1);
    int lo = d1[0], hi = d1[0];
    for (int i = 1; i < dn; i++) { if (d1[i] < lo) lo = d1[i]; if (d1[i] > hi) hi = d1[i]; }
    int sy = cy0 + ch + 14, colw = pw / 3;
    const char *lab[6] = {"Open", "High", "Low", "Mkt Cap", "P/E", "Prev close"};
    char val[6][32];
    stocks_format_price(d1[0], val[0], 32);
    stocks_format_price(hi, val[1], 32);
    stocks_format_price(lo, val[2], 32);
    { char q[16]; stocks_format_price(s->cap_b * 100, q, sizeof q); q[font_strlen_local(q) - 3] = 0; int p = stx_cat(val[3], 0, "$"); p = stx_cat(val[3], p, q); stx_cat(val[3], p, "B"); }
    stocks_format_price(s->pe_x10 * 10, val[4], 32);
    stocks_format_price(s->price_x100 - s->change_x100, val[5], 32);
    for (int i = 0; i < 6; i++) {
        int x = px + (i % 3) * colw, y = sy + (i / 3) * 40;
        font_draw_string(lab[i], x, y, STX_MUTED, -1);
        font_draw_string(val[i], x, y + 16, STX_INK, -1);
    }
    window_present();
}

static void gui_launch_stocks(void) {
    int sel = 0, range = 0, first = 0;
    for (;;) {
        int vis = stx_rows_visible();
        if (sel < first) first = sel;
        if (sel >= first + vis) first = sel - vis + 1;
        stocks_draw(sel, range, first);
        sleep_ticks(5);
        mouse_click_edge_sync();
        int k = get_key_or_click();
        if (k == KEY_ESC) return;
        if (k == KEY_UP && sel > 0) sel--;
        else if (k == KEY_DOWN && sel < STOCKS_MAX - 1) sel++;
        else if (k == KEY_LEFT && range > 0) range--;
        else if (k == KEY_RIGHT && range < STX_RANGES - 1) range++;
        else if (k == KEY_CLICK) {
            int mx = app_cursor_x, my = app_cursor_y;
            if (!gui_app_windowed && mx < 80 && my < 36) return; /* the red dot */
            if (mx < STX_SIDE_W && my >= stx_row_y0()) {
                int i = first + (my - stx_row_y0()) / STX_ROW_H;
                if (i < STOCKS_MAX) sel = i;
            } else if (my >= stx_tab_y() && my < stx_tab_y() + 24) {
                for (int r = 0; r < STX_RANGES; r++)
                    if (mx >= stx_tab_x(r) - 4 && mx < stx_tab_x(r) + 44) range = r;
            }
        }
    }
}
