/* v0.87+: Stocks, rebuilt in the shape of macOS's own Stocks app: a watchlist
   sidebar (symbol, name, sparkline, price, colored change pill) beside a
   detail pane (big price, range tabs 1D/1W/1M/3M/1Y, a real line chart, and
   provider timestamp). Epiphany (epiphany.h) is the
   other, richer take on the same data; this one stays the basic native one.

   Quotes and chart closes come from the same site's HTTPS Worker bridge.
   Both native HTTP and the v86 proxy reach the same Worker endpoint.
   R refreshes; the quote timestamp is shown in UTC. Provider delays apply.

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
    {"AAPL", "Apple Inc.", 0, 0, 0, 0}, {"MSFT", "Microsoft", 0, 0, 0, 0},
    {"GOOGL", "Alphabet Inc.", 0, 0, 0, 0}, {"AMZN", "Amazon.com", 0, 0, 0, 0},
    {"TSLA", "Tesla Inc.", 0, 0, 0, 0}, {"NVDA", "NVIDIA", 0, 0, 0, 0},
    {"META", "Meta Platforms", 0, 0, 0, 0}, {"NFLX", "Netflix", 0, 0, 0, 0},
};
#define STX_RANGES 5
#define STX_MAXPTS 64
static const char *stx_range_name[STX_RANGES] = {"1D", "1W", "1M", "3M", "1Y"};
static struct { int price, prev, time, n, points[STX_MAXPTS], stale; } stx_data[STX_RANGES][STOCKS_MAX];
static int stx_loading;
static unsigned int stx_refresh_tick;

/* Strict bounded integers; a partial/malformed reply never replaces a quote. */
static int stx_number(const char **p, int *out) {
    int v = 0, n = 0;
    while (**p >= '0' && **p <= '9') {
        int d = *(*p)++ - '0';
        if (v > (2147483647 - d) / 10) return 0;
        v = v * 10 + d; n++;
    }
    if (!n || (**p != ' ' && **p != '\n')) return 0;
    *out = v; return 1;
}
static int stx_parse_row(const char *p, int range, int i) {
    int values[4 + STX_MAXPTS], count = 0;
    while (count < 4 + STX_MAXPTS) {
        if (!stx_number(&p, &values[count++])) return 0;
        if (*p++ == '\n') break;
    }
    if (p[-1] != '\n' || count < 5 || values[3] < 1 || values[3] > STX_MAXPTS || count != 4 + values[3]) return 0;
    if (values[0] < 1 || values[0] > 10000000 || values[1] < 1 || values[1] > 10000000 || values[2] < 1) return 0;
    for (int j = 4; j < count; j++) if (values[j] < 1 || values[j] > 10000000) return 0;
    stx_data[range][i].price = values[0]; stx_data[range][i].prev = values[1];
    stx_data[range][i].time = values[2]; stx_data[range][i].n = values[3];
    for (int j = 4; j < count; j++) stx_data[range][i].points[j - 4] = values[j];
    stx_data[range][i].stale = 0;
    stocks_entries[i].price_x100 = values[0];
    stocks_entries[i].change_x100 = values[0] - values[1];
    return 1;
}
static void stocks_fetch(int range) {
    static char body[8192];
    char path[] = "/api/stocks?range=0";
    path[sizeof(path) - 2] = '0' + range;
    int n = net_init(0x0A00020F) ? http_get_timeout("joshuatree.heyitsmejosh.com", path, 80, body, sizeof(body) - 1, 1000) : -1;
    for (int i = 0; i < STOCKS_MAX; i++) stx_data[range][i].stale = 1;
    if (n > 0 && n < (int)sizeof(body) - 1 && http_last_status() == 200) {
        body[n] = 0; const char *p = body;
        for (int i = 0; i < STOCKS_MAX && *p; i++) {
            stx_parse_row(p, range, i);
            while (*p && *p != '\n') p++;
            if (*p) p++;
        }
    }
    stx_refresh_tick = ticks();
}

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
    return prev > 0 ? (int)((double)change_x100 * 10000 / prev) : 0;
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
    if (n < 2) return;
    int lo = v[0], hi = v[0];
    for (int i = 1; i < n; i++) { if (v[i] < lo) lo = v[i]; if (v[i] > hi) hi = v[i]; }
    if (hi - lo < 4) { hi += 2; lo -= 2; }
    int px = x, py = y + h - 1 - (int)((double)(v[0] - lo) * (h - 1) / (hi - lo));
    for (int i = 1; i < n; i++) {
        int cx = x + i * (w - 1) / (n - 1);
        int cy = y + h - 1 - (int)((double)(v[i] - lo) * (h - 1) / (hi - lo));
        stx_line(px, py, cx, cy, c); px = cx; py = cy;
    }
}
static unsigned int stx_rng;
static int stx_rand(int m) { stx_rng = stx_rng * 1103515245u + 12345u; return (int)((stx_rng >> 16) & 0x7FFF) % m; }

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
    font_draw_string(stx_loading ? "Updating..." : "R refresh", 190, T, STX_MUTED, -1);

    for (int r = 0; r < stx_rows_visible() && first + r < STOCKS_MAX; r++) {
        int i = first + r;
        stocks_entry_t *s = &stocks_entries[i];
        int y = stx_row_y0() + r * STX_ROW_H;
        if (i == sel) window_rect(8, y, STX_SIDE_W - 16, STX_ROW_H - 4, 0x00E2D9CC);
        font_draw_string(s->symbol, 20, y + 8, STX_INK, -1);
        font_draw_string(s->name, 20, y + 28, STX_MUTED, -1);
        if (!stx_data[range][i].n) {
            font_draw_string("--", STX_SIDE_W - 40, y + 6, STX_MUTED, -1); continue;
        }
        int *pts = stx_data[range][i].points, n = stx_data[range][i].n;
        unsigned int col = stx_data[range][i].price >= stx_data[range][i].prev ? STX_GREEN : STX_RED;
        stx_chart(118, y + 10, 60, 26, pts, n, col);
        char b[64]; stocks_format_price(stx_data[range][i].price, b, sizeof b);
        font_draw_string(b, STX_SIDE_W - 16 - font_string_width(b), y + 6, STX_INK, -1);
        stx_pct(stx_bp(stx_data[0][i].price - stx_data[0][i].prev, stx_data[0][i].price), b);
        if (range != 0 || stx_data[range][i].stale) stx_cat(b, 0, stx_data[range][i].stale ? "stale" : "USD");
        int pw = font_string_width(b) + 12;
        window_rect(STX_SIDE_W - 16 - pw, y + 26, pw, 20, col);
        font_draw_string(b, STX_SIDE_W - 16 - pw + 6, y + 28, 0x00FFFFFF, -1);
    }

    stocks_entry_t *s = &stocks_entries[sel];
    int px = stx_pane_x(), pw = WW - px - 24;
    font_draw_string(s->symbol, px, T, STX_MUTED, -1);
    font_draw_string(s->name, px, T + 22, STX_INK, -1);

    int *pts = stx_data[range][sel].points, n = stx_data[range][sel].n;
    int delta = n ? (range == 0 ? stx_data[range][sel].price - stx_data[range][sel].prev : pts[n - 1] - pts[0]) : 0;
    unsigned int col = delta >= 0 ? STX_GREEN : STX_RED;
    char b[64];
    if (n) {
        stocks_format_price(stx_data[range][sel].price, b, sizeof b);
        font_draw_string(b, px, T + 50, STX_INK, -1);
        stx_signed(delta, 1, b); font_draw_string(b, px + 120, T + 50, col, -1);
        stx_pct(stx_bp(delta, range == 0 ? stx_data[range][sel].price : pts[n - 1]), b);
        font_draw_string(b, px + 220, T + 50, col, -1);
    } else font_draw_string("Prices unavailable", px, T + 50, STX_MUTED, -1);

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

    int sy = cy0 + ch + 14;
    font_draw_string(n ? (stx_data[range][sel].stale ? "Stale quote. R to retry." : "Yahoo Finance / USD / may be delayed") : "R to retry. Quote service unavailable.", px, sy, STX_MUTED, -1);
    if (n) {
        int stamp = stx_data[range][sel].time, days = stamp / 86400, y = 1970, m = 1;
        while (days >= 365 + cal_is_leap(y)) { days -= 365 + cal_is_leap(y); y++; }
        while (days >= cal_days_in_month(y, m)) { days -= cal_days_in_month(y, m); m++; }
        char date[11]; cal_date_str(y, m, days + 1, date);
        int k = stx_cat(b, 0, "As of "); k = stx_cat(b, k, date);
        int hour = stamp / 3600 % 24, minute = stamp / 60 % 60;
        char clock[] = " 00:00 UTC";
        clock[1] += hour / 10; clock[2] += hour % 10;
        clock[4] += minute / 10; clock[5] += minute % 10;
        stx_cat(b, k, clock);
        font_draw_string(b, px, sy + 22, STX_MUTED, -1);
    }
    window_present();
}

static void gui_launch_stocks(void) {
    int sel = 0, range = 0, first = 0, fetched_range = -1;

    for (;;) {
        if (fetched_range != range || ticks() - stx_refresh_tick >= 6000) {
            stx_loading = 1; stocks_draw(sel, range, first);
            stocks_fetch(range); stx_loading = 0; fetched_range = range;
        }
        int vis = stx_rows_visible();
        if (sel < first) first = sel;
        if (sel >= first + vis) first = sel - vis + 1;
        stocks_draw(sel, range, first);
        sleep_ticks(5);
        mouse_click_edge_sync();
        int k = get_key_or_click_until(stx_refresh_tick + 6000);
        if (k == 'r' || k == 'R') fetched_range = -1;
        if (k == KEY_ESC) return;
        if (k == KEY_UP && sel > 0) sel--;
        else if (k == KEY_DOWN && sel < STOCKS_MAX - 1) sel++;
        else if (k == KEY_LEFT && range > 0) range--;
        else if (k == KEY_RIGHT && range < STX_RANGES - 1) range++;
        else if (k == KEY_CLICK) {
            /* app_cursor_x/y are full-screen; content draws through the viewport, so convert.
               A click off the content (window chrome X, the dock) closes, as everywhere else. */
            if (!gui_app_windowed) return;
            int mx = app_cursor_x - app_view_x, my = app_cursor_y - app_view_y;
            if (mx < 0 || my < 0 || mx >= (int)window_width() || my >= (int)window_height()) return;
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
