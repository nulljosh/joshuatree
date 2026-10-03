/* epiphany: markets, portfolio, a trading simulator and a macro brief, as a
 * real ring-3 program.
 *
 * The nineteenth app to leave the kernel (roadmap 2.0), done
 * the way user/curbfind.c was. Same four tabs the old kernel/epiphany.h drew in
 * ring 0: Markets (a 40-ticker watchlist you add to and remove from, plus
 * crypto, commodities and fear and greed), Portfolio (five holdings valued
 * off the same prices, P/L, an allocation bar, +/- edits shares), Simulator
 * (a random-walk market you trade against, with the mean-reversion edge
 * readout) and Situation (macro pulse and a brief). The / command bar runs
 * "TICKER GP" and "TICKER DES".
 *
 * Live prices come from joshuatree.heyitsmejosh.com/api/quotes through
 * SYS_HTTP_GET (387), one "SYM price prev" line per ticker, about 700
 * bytes, well inside JT_HTTP_BODY_MAX. Any failure leaves the compiled-in
 * prices on screen and the hint line says offline.
 *
 * The one thing that did not move: the GP chart used to draw Stocks' intraday
 * series (stx_data), which only the kernel holds and which cannot cross
 * the 2 KB syscall body (eight symbols of 64 points is about 3 KB). Here GP
 * draws previous close to last price, a straight line from the two numbers
 * /api/quotes does carry, and says so under the chart. It works for all 40
 * tickers, where the old chart only covered eight. Market cap and P/E in DES
 * still show for those eight, from a compiled-in table.
 *
 * Type: the antialiased libjt face. Every state change writes
 * one serial line, which the checks read, and the command bar keeps the
 * "epicmd=" markers tools/checks/epiphany-cmdbar-check.py asserts on.
 */
#include "jtsys.h"
#include "libjt/text.h"

#define BG     0x00FAF8F6 /* GUI_BG */
#define INK    0x001C1C1E
#define MUTED  0x00807468
#define GREEN  0x0041854B
#define RED    0x00B51616
#define RULE   0x00E0D8CE
#define SELBG  0x00E2D9CC
#define ACCENT 0x000A84FF
#define TOP    8

typedef struct { const char *sym, *name; int price, bp; } row_t; /* price x100, change in basis points */

#define POOL_N 40
static row_t pool[POOL_N] JT_DATA = {
    {"AAPL", "Apple", 23800, 106}, {"MSFT", "Microsoft", 41900, -43}, {"GOOGL", "Alphabet", 14200, 252},
    {"AMZN", "Amazon", 19100, -164}, {"TSLA", "Tesla", 24200, 372}, {"NVDA", "NVIDIA", 12800, 330},
    {"META", "Meta", 58000, -109}, {"NFLX", "Netflix", 66000, 182}, {"AMD", "AMD", 15600, 290},
    {"DIS", "Disney", 9800, -60}, {"JPM", "JPMorgan", 21500, 45}, {"COIN", "Coinbase", 24800, 540},
    {"SHOP", "Shopify", 8900, 210}, {"UBER", "Uber", 7400, -85}, {"INTC", "Intel", 2300, -120},
    {"CRM", "Salesforce", 27000, 80}, {"ORCL", "Oracle", 16500, 140}, {"ADBE", "Adobe", 46000, -70},
    {"PYPL", "PayPal", 7200, 30}, {"SQ", "Block", 6800, 190}, {"ABNB", "Airbnb", 13500, -40},
    {"SNOW", "Snowflake", 12000, 260}, {"PLTR", "Palantir", 4200, 410}, {"BA", "Boeing", 17500, -150},
    {"GS", "Goldman Sachs", 52000, 60}, {"BAC", "Bank of America", 4100, 25}, {"V", "Visa", 29000, 50},
    {"MA", "Mastercard", 47000, 40}, {"WMT", "Walmart", 8200, 70}, {"COST", "Costco", 89000, 30},
    {"KO", "Coca-Cola", 6300, -10}, {"PEP", "PepsiCo", 16800, -20}, {"NKE", "Nike", 7800, -130},
    {"SBUX", "Starbucks", 9600, 90}, {"MCD", "McDonald's", 29500, 15}, {"XOM", "Exxon", 11800, 110},
    {"CVX", "Chevron", 15500, 95}, {"PFE", "Pfizer", 2800, -45}, {"JNJ", "Johnson & Johnson", 16000, 20},
    {"UNH", "UnitedHealth", 52500, -210},
};
/* Market cap in billions and P/E x10 for the first eight pool tickers. */
static const struct { int cap_b, pe_x10; } FUND[8] = {
    {3600, 312}, {3100, 350}, {1750, 245}, {2000, 340}, {780, 610}, {3100, 550}, {1480, 280}, {290, 470},
};
static const row_t ALT[] = {
    {"BTC", "Bitcoin", 6420000, 210}, {"ETH", "Ethereum", 245000, -140},
    {"GC", "Gold", 265000, 35}, {"CL", "WTI Crude", 7240, -90}, {"SI", "Silver", 3120, 120},
};
#define ALT_N 5
static const char *TAB_NAME[4] = {"Markets", "Portfolio", "Simulator", "Situation"};
#define HOLD_N 5
static struct { int pool; int shares; int cost; } hold[HOLD_N] JT_DATA = {
    {0, 40, 19500}, {1, 12, 38000}, {2, 25, 11800}, {4, 10, 26000}, {5, 30, 9200},
};

static struct jt_window_info win JT_DATA = {0, 0, 0, 0};
static char body[JT_HTTP_BODY_MAX] JT_DATA;
static int watch[POOL_N] JT_DATA;
static int scroll_top JT_DATA, adding JT_DATA, live JT_DATA;
static int tab JT_DATA, sel JT_DATA;

/* simulator */
#define SIM_PTS 120
static int sim_px[SIM_PTS] JT_DATA, sim_n JT_DATA, sim_cash JT_DATA, sim_pos JT_DATA, sim_paused JT_DATA;
static unsigned rng JT_DATA;
static int rnd(int m) { rng = rng * 1103515245u + 12345u; return (int)((rng >> 16) & 0x7FFF) % m; }

/* command bar */
#define CMD_MAX 24
#define V_NONE 0
#define V_GP   1
#define V_DES  2
static char cmd_buf[CMD_MAX + 1] JT_DATA, cmd_err[40] JT_DATA;
static int cmd_len JT_DATA, cmd_focus JT_DATA, cmd_view JT_DATA, cmd_idx JT_DATA;

static void rect(int x, int y, int w, int h, unsigned c) {
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > (int)win.width)  w = (int)win.width - x;
    if (y + h > (int)win.height) h = (int)win.height - y;
    for (int yy = 0; yy < h; yy++) {
        unsigned *r = win.pixels + (unsigned)(y + yy) * win.width + (unsigned)x;
        for (int xx = 0; xx < w; xx++) r[xx] = c;
    }
}
static int slen(const char *s) { int n = 0; while (s[n]) n++; return n; }
static int text(const char *s, int x, int y, unsigned fg) { return jt_text_draw(&win, JT_FACE_BODY, x, y, fg, s); }
static void right(const char *s, int xr, int y, unsigned fg) { text(s, xr - jt_text_width(JT_FACE_BODY, s), y, fg); }
/* Draw s at (x, y) cut with "..." so it never runs past maxw pixels. */
static void text_fit(const char *s, int x, int y, int maxw, unsigned fg) {
    char out[128]; int n = 0;
    while (s[n] && n < (int)sizeof out - 4) { out[n] = s[n]; n++; }
    out[n] = 0;
    if (s[n] || jt_text_width(JT_FACE_BODY, out) > maxw) {
        while (n > 0) {
            out[n] = '.'; out[n + 1] = '.'; out[n + 2] = '.'; out[n + 3] = 0;
            if (jt_text_width(JT_FACE_BODY, out) <= maxw) break;
            out[--n] = 0;
        }
    }
    text(out, x, y, fg);
}
static void pset(int x, int y, unsigned c) {
    if (x >= 0 && y >= 0 && x < (int)win.width && y < (int)win.height) win.pixels[(unsigned)y * win.width + (unsigned)x] = c;
}
static void line(int x0, int y0, int x1, int y1, unsigned c) { /* Bresenham, two pixels thick */
    int dx = x1 > x0 ? x1 - x0 : x0 - x1, dy = y1 > y0 ? y0 - y1 : y1 - y0;
    int sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1, err = dx + dy;
    for (;;) {
        pset(x0, y0, c); pset(x0, y0 + 1, c);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}
static void chart(int x, int y, int w, int h, const int *v, int n, unsigned c) {
    if (n < 2) return;
    int lo = v[0], hi = v[0];
    for (int i = 1; i < n; i++) { if (v[i] < lo) lo = v[i]; if (v[i] > hi) hi = v[i]; }
    if (hi - lo < 4) { hi += 2; lo -= 2; }
    int px = x, py = y + h - 1 - (int)((long)(v[0] - lo) * (h - 1) / (hi - lo));
    for (int i = 1; i < n; i++) {
        int cx = x + i * (w - 1) / (n - 1), cy = y + h - 1 - (int)((long)(v[i] - lo) * (h - 1) / (hi - lo));
        line(px, py, cx, cy, c); px = cx; py = cy;
    }
}

static int cat(char *b, int p, const char *s) { while (*s && p < 62) b[p++] = *s++; b[p] = 0; return p; }
static int itoa10(int v, char *buf) {
    char tmp[12]; int tn = 0, n = 0; unsigned u = v < 0 ? (unsigned)(-v) : (unsigned)v;
    if (v < 0) buf[n++] = '-';
    do { tmp[tn++] = (char)('0' + u % 10); u /= 10; } while (u);
    while (tn) buf[n++] = tmp[--tn];
    buf[n] = 0;
    return n;
}
static void say(const char *pfx, int a) {
    char l[64]; int n = cat(l, 0, pfx);
    n += itoa10(a, l + n); l[n++] = '\n';
    jt_write(1, l, (unsigned)n);
}
static void price_fmt(int x100, char *b) { /* "1,234.56" */
    int dollars = x100 / 100, cents = x100 % 100, p = 0;
    if (dollars == 0) b[p++] = '0';
    else {
        char t[12]; int ti = 0;
        while (dollars > 0) { t[ti++] = (char)('0' + dollars % 10); dollars /= 10; }
        while (ti > 0) { b[p++] = t[--ti]; if (ti > 0 && ti % 3 == 0) b[p++] = ','; }
    }
    b[p++] = '.'; b[p++] = (char)('0' + cents / 10); b[p++] = (char)('0' + cents % 10); b[p] = 0;
}
static void money(int x100, char *b) {
    int p = cat(b, 0, x100 < 0 ? "-$" : "$");
    char t[24]; price_fmt(x100 < 0 ? -x100 : x100, t); cat(b, p, t);
}
static void pct(int bp, char *b) { int p = cat(b, 0, bp < 0 ? "-" : "+"); char t[24]; price_fmt(bp < 0 ? -bp : bp, t); p = cat(b, p, t); cat(b, p, "%"); }
static unsigned col(int v) { return v >= 0 ? GREEN : RED; }

static int nth(int want, int k) { for (int i = 0; i < POOL_N; i++) if (!!watch[i] == want && k-- == 0) return i; return -1; }
static int count(int want) { int n = 0; for (int i = 0; i < POOL_N; i++) n += !!watch[i] == want; return n; }
static char up(char c) { return (c >= 'a' && c <= 'z') ? (char)(c - 32) : c; }
static int streq_ci(const char *a, const char *b) { while (*a && *b) { if (up(*a) != up(*b)) return 0; a++; b++; } return *a == *b; }
static int find_pool(const char *sym) { for (int i = 0; i < POOL_N; i++) if (streq_ci(sym, pool[i].sym)) return i; return -1; }

/* "SYM price prev" per line from /api/quotes; a 0 price keeps the last number. */
static void fetch_quotes(void) {
    live = 0;
    int n = jt_http_get("/api/quotes", body, sizeof(body) - 1);
    say("epiphany: fetch ", n);
    if (n <= 0 || n >= (int)sizeof(body) - 1) return;
    body[n] = 0;
    for (const char *p = body; *p;) {
        char sym[8]; int si = 0, price = 0, prev = 0;
        while (*p && *p != ' ' && *p != '\n' && si < 7) sym[si++] = *p++;
        sym[si] = 0;
        if (*p == ' ') { p++; while (*p >= '0' && *p <= '9') price = price * 10 + (*p++ - '0'); }
        if (*p == ' ') { p++; while (*p >= '0' && *p <= '9') prev = prev * 10 + (*p++ - '0'); }
        while (*p && *p != '\n') p++;
        if (*p) p++;
        if (price <= 0 || prev < 100) continue;
        int i = find_pool(sym);
        if (i < 0) continue;
        pool[i].price = price;
        pool[i].bp = (price - prev) * 100 / (prev / 100); /* no 64-bit divide without libgcc */
        live++;
    }
}

/* ---- simulator ---- */
static void sim_reset(void) { rng = 4242; sim_n = 1; sim_px[0] = 10000; sim_cash = 1000000; sim_pos = 0; sim_paused = 0; }
static void sim_step(void) {
    int last = sim_px[sim_n - 1];
    int next = last + last * (rnd(201) - 100) / 4000 + (rnd(50) == 0 ? last / 25 : 0);
    if (next < 500) next = 500;
    if (sim_n < SIM_PTS) sim_px[sim_n++] = next;
    else { for (int i = 1; i < SIM_PTS; i++) sim_px[i - 1] = sim_px[i]; sim_px[SIM_PTS - 1] = next; }
}
static int sim_equity(void) { return sim_cash + sim_pos * sim_px[sim_n - 1]; }

/* ---- command bar ---- */
static int cmd_split(const char *in, char *ticker, char *code) {
    int len = slen(in), sp = -1;
    for (int i = len - 1; i >= 0; i--) if (in[i] == ' ') { sp = i; break; }
    if (sp <= 0 || sp >= len - 1) return 0;
    int ti = 0; for (int i = 0; i < sp && ti < 7; i++) ticker[ti++] = up(in[i]); ticker[ti] = 0;
    int ci = 0; for (int i = sp + 1; i < len && ci < 7; i++) code[ci++] = up(in[i]); code[ci] = 0;
    return ti > 0 && ci > 0;
}
static void emit(const char *pfx, const char *a, const char *b) {
    char l[64]; int n = cat(l, 0, pfx); n = cat(l, n, a);
    if (b) { n = cat(l, n, " "); n = cat(l, n, b); }
    l[n++] = '\n'; jt_write(1, l, (unsigned)n);
}
static void cmd_run(void) {
    cmd_buf[cmd_len] = 0;
    char ticker[8], code[8];
    if (!cmd_split(cmd_buf, ticker, code)) { cat(cmd_err, 0, "Type TICKER CODE, e.g. AAPL GP"); return; }
    int id = streq_ci(code, "GP") ? V_GP : streq_ci(code, "DES") ? V_DES : V_NONE;
    if (id == V_NONE) { int p = cat(cmd_err, 0, "Unknown code: "); cat(cmd_err, p, code); emit("epicmd=unknown_code:", code, 0); return; }
    int idx = find_pool(ticker);
    if (idx < 0) { int p = cat(cmd_err, 0, "Unknown ticker: "); cat(cmd_err, p, ticker); emit("epicmd=unknown_ticker:", ticker, 0); return; }
    cmd_err[0] = 0;
    watch[idx] = 1; /* GP and DES both put the ticker on the watchlist */
    cmd_view = id; cmd_idx = idx;
    emit("epicmd=run:", ticker, code);
}

/* ---- drawing ---- */
static void draw_head(const row_t *s, int x, int y) {
    char b[64];
    text(s->sym, x, y, MUTED); text(s->name, x, y + 22, INK);
    price_fmt(s->price, b); text(b, x, y + 50, INK);
    pct(s->bp, b); text(b, x + 140, y + 50, col(s->bp));
}
static void draw_gp(int x, int y, int w, int h) {
    const row_t *s = &pool[cmd_idx];
    draw_head(s, x, y);
    int ch = h - 90; if (ch < 60) ch = 60;
    rect(x, y + 74, w, 1, RULE);
    int prev = (int)((long)s->price * 10000 / (10000 + s->bp)), v[2] = {prev, s->price};
    chart(x, y + 82, w, ch, v, 2, col(s->bp));
    text_fit("Previous close to last price. Intraday history is not in ring 3 yet.", x, y + 82 + ch + 14, w, MUTED);
}
static void draw_des(int x, int y) {
    const row_t *s = &pool[cmd_idx]; char b[64];
    draw_head(s, x, y);
    if (cmd_idx < 8) {
        text("Market cap", x, y + 88, MUTED);
        itoa10(FUND[cmd_idx].cap_b, b); cat(b, slen(b), "B"); text(b, x, y + 108, INK);
        text("P/E", x + 200, y + 88, MUTED);
        int p = itoa10(FUND[cmd_idx].pe_x10 / 10, b); b[p++] = '.'; b[p++] = (char)('0' + FUND[cmd_idx].pe_x10 % 10); b[p] = 0;
        text(b, x + 200, y + 108, INK);
    } else text("Market cap and P/E unavailable for this ticker.", x, y + 88, MUTED);
}
static void draw_markets(int x, int y, int w) {
    char b[64];
    int narrow = w < 560;   /* a phone: one full-width list, the side column would collide with it */
    int cw = narrow ? w - 8 : (w - 32) / 2, x2 = x + cw + 32;
    text(adding ? "Add to watchlist (enter or space adds, esc cancels)" : "Watchlist", x, y, adding ? ACCENT : MUTED);
    int want = adding ? 0 : 1, n = count(want);
    int vis = ((int)win.height - y - 84) / 22; /* rows end above the command bar rule */ if (vis < 3) vis = 3;
    if (sel < scroll_top) scroll_top = sel;
    if (sel >= scroll_top + vis) scroll_top = sel - vis + 1;
    if (scroll_top > n - vis) scroll_top = n - vis;
    if (scroll_top < 0) scroll_top = 0;
    if (n == 0) text(adding ? "Everything is already watched" : "Empty, press a to add", x, y + 24, MUTED);
    if (!adding && n) { itoa10(n, b); cat(b, slen(b), " watched"); right(b, x + cw, y, MUTED); }
    for (int r = scroll_top; r < n && r < scroll_top + vis; r++) {
        const row_t *s = &pool[nth(want, r)]; int ry = y + 24 + (r - scroll_top) * 22;
        if (r == sel) rect(x - 8, ry - 4, cw + 16, 22, SELBG);
        text(s->sym, x, ry, INK);
        price_fmt(s->price, b); right(b, x + cw - (narrow ? 76 : 90), ry, INK);
        pct(s->bp, b); right(b, x + cw, ry, col(s->bp));
    }
    if (narrow) { if (!adding) text("a add   d remove", x, y + 24 + vis * 22 + 4, MUTED); return; }
    text("Crypto and commodities", x2, y, MUTED);
    for (int i = 0; i < ALT_N; i++) {
        int ry = y + 24 + i * 22;
        text(ALT[i].sym, x2, ry, INK);
        price_fmt(ALT[i].price, b); right(b, x2 + cw - 90, ry, INK);
        pct(ALT[i].bp, b); right(b, x2 + cw, ry, col(ALT[i].bp));
    }
    int y3 = y + 24 + ALT_N * 22 + 16, fg = 62;
    text("Fear and greed", x2, y3, MUTED);
    rect(x2, y3 + 24, cw, 10, RULE);
    rect(x2, y3 + 24, cw * fg / 100, 10, col(fg - 50));
    itoa10(fg, b); cat(b, slen(b), "  Greed"); text(b, x2, y3 + 40, INK);
    if (!adding) text("a add   d remove", x2, y3 + 60, MUTED);
}
static void draw_portfolio(int x, int y, int w) {
    char b[64];
    static const char *hd[4] = {"Shares", "Cost", "Value", "P/L"};
    int cx[4] = {x + 170, x + 290, x + 430, x + w};
    if (w < 560) { cx[0] = x + w * 30 / 100; cx[1] = x + w * 52 / 100; cx[2] = x + w * 76 / 100; }   /* a phone: the columns scale with the window */
    text("Holding", x, y, MUTED);
    for (int i = 0; i < 4; i++) right(hd[i], cx[i], y, MUTED);
    int total = 0, cost = 0;
    for (int i = 0; i < HOLD_N; i++) {
        const row_t *s = &pool[hold[i].pool];
        int val = hold[i].shares * s->price, cb = hold[i].shares * hold[i].cost;
        total += val; cost += cb;
        int ry = y + 24 + i * 22;
        if (i == sel) rect(x - 8, ry - 4, w + 16, 22, SELBG);
        text(s->sym, x, ry, INK);
        itoa10(hold[i].shares, b); right(b, cx[0], ry, INK);
        money(cb, b); right(b, cx[1], ry, MUTED);
        money(val, b); right(b, cx[2], ry, INK);
        money(val - cb, b); right(b, cx[3], ry, col(val - cb));
    }
    int ty = y + 24 + HOLD_N * 22 + 10;
    rect(x, ty - 6, w, 1, RULE);
    text("Total", x, ty + 4, INK);
    money(total, b); right(b, cx[2], ty + 4, INK);
    money(total - cost, b); right(b, cx[3], ty + 4, col(total - cost));
    int p = cost > 100 ? ((total - cost) / 100) * 10000 / (cost / 100) : 0;
    pct(p, b); right(b, cx[3], ty + 26, col(p));
    text("Allocation", x, ty + 48, MUTED);
    static const unsigned seg[HOLD_N] = {0x000A84FF, 0x0041854B, 0x00C98A1B, 0x00B51616, 0x00707070};
    int ax = x;
    for (int i = 0; i < HOLD_N && total > 0; i++) {
        int sw = (int)((long)hold[i].shares * pool[hold[i].pool].price * w / total);
        rect(ax, ty + 70, sw, 12, seg[i]); ax += sw;
    }
    right("up/down pick   + / - shares", x + w, ty + 48, MUTED); /* same row as the Allocation label, clear of the command bar below */
}
static void draw_sim(int x, int y, int w, int h) {
    char b[64]; int px = sim_px[sim_n - 1];
    text("Simulator", x, y, MUTED);
    price_fmt(px, b); text(b, x, y + 22, INK);
    int chg = px - sim_px[0];
    pct((int)((long)chg * 10000 / sim_px[0]), b); text(b, x + 120, y + 22, col(chg));
    int ch = h - 190; if (ch < 70) ch = 70;
    rect(x, y + 54, w, 1, RULE);
    if (sim_n > 1) chart(x, y + 62, w, ch, sim_px, sim_n, col(chg));
    int by = y + 62 + ch + 16, eq = sim_equity();
    int n1 = w < 560 ? w * 28 / 100 : 180, n2 = w < 560 ? w * 54 / 100 : 360, n3 = w < 560 ? w * 76 / 100 : 520;   /* the stats row scales on a phone */
    money(eq, b); text("Equity", x, by, MUTED); text(b, x, by + 20, INK);
    money(eq - 1000000, b); text("P/L", x + n1, by, MUTED); text(b, x + n1, by + 20, col(eq - 1000000));
    itoa10(sim_pos, b); text("Position", x + n2, by, MUTED); text(b, x + n2, by + 20, INK);
    int mean = 0, m = sim_n < 20 ? sim_n : 20;
    for (int i = sim_n - m; i < sim_n; i++) mean += sim_px[i];
    mean /= m;
    int edge = (int)((long)(px - mean) * 10000 / mean);
    pct(edge, b); text(w < 560 ? "Vs mean" : "Vs 20-tick mean", x + n3, by, MUTED); text(b, x + n3, by + 20, col(-edge));
    text(sim_paused ? "b buy 10   s sell 10   space resume   r reset" : "b buy 10   s sell 10   space pause   r reset", x, by + 52, MUTED);
}
static void draw_situation(int x, int y) {
    static const struct { const char *n, *v; int bp; } r[] = {
        {"VIX", "16.42", -310}, {"US 10Y", "4.18%", 120}, {"DXY", "103.9", 20}, {"Fed funds", "4.50%", 0}, {"S&P 500", "5,812", 45},
    };
    text("Macro pulse", x, y, MUTED);
    for (int i = 0; i < 5; i++) {
        int ry = y + 24 + i * 22;
        text(r[i].n, x, ry, INK); right(r[i].v, x + 260, ry, INK);
        if (r[i].bp) { char b[32]; pct(r[i].bp, b); right(b, x + 380, ry, col(r[i].bp)); }
    }
    int by = y + 24 + 5 * 22 + 16;
    text("Daily brief", x, by, MUTED);
    text_fit("Risk appetite firm: fear and greed sits in Greed, volatility is soft.", x, by + 24, (int)win.width - x - 20, INK);
    text_fit("Rates steady, dollar flat. Mega-cap tech leads, autos and retail lag.", x, by + 46, (int)win.width - x - 20, INK);
    text_fit("Sample macro data. The live map and People graph are not ported yet.", x, by + 78, (int)win.width - x - 20, MUTED);
}
static void draw(void) {
    int WW = (int)win.width, HH = (int)win.height;
    rect(0, 0, WW, HH, BG);
    for (int i = 0; i < 4; i++) {
        int tx = 24 + i * 110;
        if (i == tab) { rect(tx - 8, TOP, 100, 26, ACCENT); text(TAB_NAME[i], tx, TOP + 4, 0x00FFFFFF); }
        else text(TAB_NAME[i], tx, TOP + 4, MUTED);
    }
    rect(0, TOP + 32, WW, 1, RULE);
    int x = 32, y = TOP + 48, w = WW - 64; if (w > 760) w = 760;
    int bar_y = HH - 56;
    if (cmd_view == V_GP) draw_gp(x, y, w, bar_y - y - 12);
    else if (cmd_view == V_DES) draw_des(x, y);
    else if (tab == 0) draw_markets(x, y, w);
    else if (tab == 1) draw_portfolio(x, y, w);
    else if (tab == 2) draw_sim(x, y, w, bar_y - y);
    else draw_situation(x, y);
    rect(20, bar_y, WW - 40, 1, RULE);
    if (cmd_err[0]) text(cmd_err, 20, bar_y + 8, RED);
    else if (cmd_focus) { char s[CMD_MAX + 4]; int p = cat(s, 0, "/ "); p = cat(s, p, cmd_buf); cat(s, p, "_"); text(s, 20, bar_y + 8, ACCENT); }
    else text("/ command   AAPL GP   AAPL DES", 20, bar_y + 8, MUTED);
    text_fit(live ? "Live quotes, crypto and macro are sample   r refresh   left/right tabs   esc closes"
                  : "Offline, sample prices   r retry   left/right tabs   esc closes", 20, HH - 26, WW - 40, MUTED);
}

/* Returns 0 when the program should close. */
static int handle(const struct jt_event *ev) {
    int k = ev->a;
    if (ev->kind == JT_EV_CLICK) {
        int mx = ev->a, my = ev->b;
        if (mx < 0 || my < 0 || mx >= (int)win.width || my >= (int)win.height) return 0; /* chrome X or dock */
        if (my >= TOP && my < TOP + 28)
            for (int i = 0; i < 4; i++) if (mx >= 16 + i * 110 && mx < 116 + i * 110) { tab = i; sel = 0; adding = 0; cmd_view = 0; }
        return 1;
    }
    if (ev->kind != JT_EV_KEY) return 1;
    if (k == '`') { jt_write(1, "epiphany: crashing on purpose\n", 30); *(volatile int *)0 = 1; } /* deliberate crash, as in every ring-3 app */
    if (cmd_focus) {
        if (k == JT_KEY_ESC) { cmd_focus = 0; cmd_len = 0; cmd_err[0] = 0; }
        else if (k == '\n' || k == JT_KEY_ENTER) { cmd_focus = 0; cmd_run(); }
        else if (k == '\b' || k == 127) { if (cmd_len > 0) cmd_len--; }
        else if (k >= 32 && k < 127 && cmd_len < CMD_MAX) cmd_buf[cmd_len++] = (char)k;
        return 1;
    }
    if (k == '/') { cmd_focus = 1; cmd_len = 0; cmd_buf[0] = 0; cmd_err[0] = 0; return 1; }
    int lim = tab == 0 ? count(adding ? 0 : 1) : HOLD_N;
    if (k == JT_KEY_ESC) {
        if (cmd_view) { cmd_view = 0; cmd_err[0] = 0; return 1; }
        if (adding) { adding = 0; sel = 0; return 1; }
        return 0;
    }
    if (tab == 0 && k == 'a' && !adding && count(0) > 0) { adding = 1; sel = 0; }
    else if (tab == 0 && adding && (k == JT_KEY_ENTER || k == '\n' || k == ' ')) { int p = nth(0, sel); if (p >= 0) watch[p] = 1; adding = 0; sel = 0; }
    else if (tab == 0 && !adding && k == 'd') { int p = nth(1, sel); if (p >= 0) watch[p] = 0; if (sel > 0 && sel >= count(1)) sel--; }
    else if (k == JT_KEY_LEFT && tab > 0) { tab--; sel = 0; cmd_view = 0; }
    else if (k == JT_KEY_RIGHT && tab < 3) { tab++; sel = 0; cmd_view = 0; }
    else if (!adding && k >= '1' && k <= '4') { tab = k - '1'; sel = 0; cmd_view = 0; }
    else if (k == JT_KEY_UP && sel > 0) sel--;
    else if (k == JT_KEY_DOWN && sel < lim - 1) sel++;
    else if (tab == 1 && (k == '+' || k == '=') && hold[sel].shares < 9999) hold[sel].shares++;
    else if (tab == 1 && k == '-' && hold[sel].shares > 0) hold[sel].shares--;
    else if (tab == 2 && k == 'b' && sim_cash >= 10 * sim_px[sim_n - 1]) { sim_cash -= 10 * sim_px[sim_n - 1]; sim_pos += 10; }
    else if (tab == 2 && k == 's' && sim_pos >= 10) { sim_cash += 10 * sim_px[sim_n - 1]; sim_pos -= 10; }
    else if (tab == 2 && k == ' ') sim_paused = !sim_paused;
    else if (tab == 2 && k == 'r') sim_reset();
    else if (k == 'r' || k == 'R') { fetch_quotes(); say("epiphany: live ", live); }
    return 1;
}

__attribute__((section(".text.start"), used))
void _start(int argc, char **argv) {
    (void)argc; (void)argv;
    if (jt_window_open(&win) != 0 || !win.pixels) {
        jt_write(2, "epiphany: no window\n", 20);
        jt_exit(1);
    }
    {   /* one line, one write: what tools/checks/ring3epiphany-check.py asserts on */
        char l[60]; int n = cat(l, 0, "epiphany: ring-3 window ");
        n += itoa10((int)win.width, l + n); l[n++] = 'x';
        n += itoa10((int)win.height, l + n); l[n++] = '\n';
        jt_write(1, l, (unsigned)n);
    }
    for (int i = 0; i < POOL_N; i++) watch[i] = 1;
    sim_reset();
    for (int i = 0; i < 40; i++) sim_step();
    draw(); /* first frame before the network round trip, so the window never opens blank */
    struct jt_event ev;
    int pending = jt_window_poll(&ev, JT_POLL_PRESENT) == 1;
    fetch_quotes();
    say(live ? "epiphany: live " : "epiphany: offline ", live);
    draw();
    if (pending && !handle(&ev)) goto done;

    struct jt_tasks t; jt_tasks(&t, 0);
    unsigned last_step = t.ticks, last_fetch = t.ticks;
    unsigned flags = JT_POLL_PRESENT;
    for (;;) {
        int r = jt_window_poll(&ev, flags);
        if (r == 1 && jt_window_resized(&ev, &win)) { draw(); flags = JT_POLL_PRESENT; continue; } /* JT_EV_RESIZE: remapped, repaint at the new size */
        flags = 0;
        if (r == -11 /* -EAGAIN */) {
            jt_tasks(&t, 0);
            if (t.ticks - last_fetch >= 6000) { last_fetch = t.ticks; fetch_quotes(); draw(); flags = JT_POLL_PRESENT; } /* once a minute */
            else if (tab == 2 && !sim_paused && t.ticks - last_step >= 6) { last_step = t.ticks; sim_step(); draw(); flags = JT_POLL_PRESENT; }
            else jt_sched_yield();
            continue;
        }
        if (r != 1) break;
        if (ev.kind != JT_EV_KEY && ev.kind != JT_EV_CLICK) { flags = JT_POLL_PRESENT; continue; }
        if (!handle(&ev)) break;
        draw(); flags = JT_POLL_PRESENT;
    }
done:
    jt_write(1, "epiphany: closed\n", 17);
    jt_exit(0);
}
