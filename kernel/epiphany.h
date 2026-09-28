/* v0.87+: Epiphany, the offline slice of the real app (github.com/nulljosh/epiphany).
   The real one is a React/Supabase/Stripe stack that is HTTPS end to end, and
   this kernel has no TLS, so it cannot run here as-is (roadmap.md). What ports
   honestly is the part that needs no network: the tab shell and the four tabs'
   local logic. Equity prices are live through Stocks' own quote fetch
   (epi_refresh); holdings, crypto, commodities and macro stay sample data.

   Markets    equities from Stocks' own table, plus crypto, commodities, fear/greed
   Portfolio  holdings valued off the same prices, P/L, allocation bar; +/- edits shares
   Simulator  a live-ticking random-walk market you trade against, with edge readout
   Situation  macro pulse and a short brief
   Not ported: the live map, People graph, accounts/billing/sync (all need network).

   Keys: left/right or 1-4 switch tab, up/down select, + / - shares, b/s/space in
   the simulator, esc closes. Clicking a tab switches to it; the red dot closes. */

/* Sample holdings. epi_refresh overwrites price and change with live quotes;
   these numbers only show when the network is down. */
static stocks_entry_t epi_demo_entries[STOCKS_MAX] = {
    {"AAPL", "Apple Inc.", 23800, 250, 3600, 312},
    {"MSFT", "Microsoft", 41900, -180, 3100, 350},
    {"GOOGL", "Alphabet Inc.", 14200, 350, 1750, 245},
    {"AMZN", "Amazon.com", 19100, -320, 2000, 340},
    {"TSLA", "Tesla Inc.", 24200, 870, 780, 610},
    {"NVDA", "NVIDIA", 12800, 410, 3100, 550},
    {"META", "Meta Platforms", 58000, -640, 1480, 280},
    {"NFLX", "Netflix", 66000, 1180, 290, 470},
};

#define EPI_TABS 4
static const char *epi_tab_name[EPI_TABS] = {"Markets", "Portfolio", "Simulator", "Situation"};
#define EPI_ACCENT 0x000A84FF

typedef struct { const char *sym, *name; int price_x100, bp; } epi_row_t;
static const epi_row_t epi_alt[] = {
    {"BTC", "Bitcoin", 6420000, 210}, {"ETH", "Ethereum", 245000, -140},
    {"GC", "Gold", 265000, 35}, {"CL", "WTI Crude", 7240, -90}, {"SI", "Silver", 3120, 120},
};
#define EPI_ALT_N 5

/* Epiphany's own watchlist: a pool of tickers, a flag per ticker for whether it is watched.
   All 40 carry live quotes through /api/quotes (worker.js); the numbers here are the offline fallback. */
#define EPI_POOL_N 40
static epi_row_t epi_pool[EPI_POOL_N] = {
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
static int epi_watch[EPI_POOL_N] = {[0 ... EPI_POOL_N - 1] = 1};
static int epi_scroll; /* first visible watchlist row */
/* k-th watched (or, with want=0, unwatched) pool index; -1 past the end */
static int epi_nth(int want, int k) { for (int i = 0; i < EPI_POOL_N; i++) if (!!epi_watch[i] == want && k-- == 0) return i; return -1; }
static int epi_count(int want) { int n = 0; for (int i = 0; i < EPI_POOL_N; i++) n += !!epi_watch[i] == want; return n; }
static int epi_adding;
static int epi_live; /* how many pool tickers carry a live quote */

/* "SYM price prev" per line from /api/quotes; a 0 price keeps the last number. */
static void epi_fetch_quotes(void) {
    static char body[2048];
    int n = net_init(0x0A00020F) ? http_get_timeout("joshuatree.heyitsmejosh.com", "/api/quotes", 80, body, sizeof(body) - 1, 1500) : -1;
    if (n <= 0 || n >= (int)sizeof(body) - 1 || http_last_status() != 200) return;
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
        for (int i = 0; i < EPI_POOL_N; i++) {
            const char *a = sym, *b = epi_pool[i].sym;
            while (*a && *a == *b) { a++; b++; }
            if (*a || *b) continue;
            epi_pool[i].price_x100 = price;
            epi_pool[i].bp = (price - prev) * 100 / (prev / 100); /* no 64-bit divide in a freestanding kernel */
            epi_live++;
            break;
        }
    }
}

/* Pull the 1D quotes Stocks uses and copy fresh ones in. A stale or failed
   row keeps its last price, so an outage never blanks the portfolio. */
static void epi_refresh(void) {
    stocks_fetch(0);
    epi_live = 0;
    for (int i = 0; i < STOCKS_MAX; i++) {
        if (stx_data[0][i].stale) continue;
        int price = stx_data[0][i].price, prev = stx_data[0][i].prev;
        epi_demo_entries[i].price_x100 = price; /* holdings are Stocks' 8, chart points included */
        epi_demo_entries[i].change_x100 = price - prev;
    }
    epi_fetch_quotes();
}

#define EPI_HOLD_N 5
static struct { int stock; int shares; int cost_x100; } epi_hold[EPI_HOLD_N] = {
    {0, 40, 19500}, {1, 12, 38000}, {2, 25, 11800}, {4, 10, 26000}, {5, 30, 9200},
};

/* simulator state */
#define EPI_SIM_PTS 120
static int epi_sim_px[EPI_SIM_PTS], epi_sim_n, epi_sim_cash, epi_sim_pos, epi_sim_paused;
static void epi_sim_reset(void) {
    stx_rng = 4242; epi_sim_n = 1; epi_sim_px[0] = 10000; epi_sim_cash = 1000000; epi_sim_pos = 0; epi_sim_paused = 0;
}
static void epi_sim_step(void) {
    int last = epi_sim_px[epi_sim_n - 1];
    int next = last + last * (stx_rand(201) - 100) / 4000 + (stx_rand(50) == 0 ? last / 25 : 0); /* drift plus rare jumps */
    if (next < 500) next = 500;
    if (epi_sim_n < EPI_SIM_PTS) epi_sim_px[epi_sim_n++] = next;
    else { for (int i = 1; i < EPI_SIM_PTS; i++) epi_sim_px[i - 1] = epi_sim_px[i]; epi_sim_px[EPI_SIM_PTS - 1] = next; }
}
static int epi_sim_equity(void) { return epi_sim_cash + epi_sim_pos * epi_sim_px[epi_sim_n - 1]; }

static void epi_money(int x100, char *b) { /* "$1,234.56" without commas, sign-aware */
    int p = stx_cat(b, 0, x100 < 0 ? "-$" : "$");
    char t[24]; stocks_format_price(x100 < 0 ? -x100 : x100, t, sizeof t); stx_cat(b, p, t);
}
static void epi_num(int v, char *b) {
    char t[12]; int ti = 0, p = 0;
    if (v < 0) { b[p++] = '-'; v = -v; }
    if (v == 0) t[ti++] = '0';
    while (v > 0) { t[ti++] = '0' + v % 10; v /= 10; }
    while (ti > 0) b[p++] = t[--ti];
    b[p] = 0;
}
static void epi_right(const char *s, int xr, int y, unsigned int c) { font_draw_string(s, xr - font_string_width(s), y, c, -1); }
static unsigned int epi_col(int v) { return v >= 0 ? STX_GREEN : STX_RED; }

/* ---- command bar: "<TICKER> CODE", Enter, it's there. GP and GP alone
   ships tonight (full chart) plus DES (a description panel); the table is
   the only thing TOP/WEI/FX/CRYPTO need touched to drop in later. */
#define EPI_CMD_MAX 24
static char epi_cmd_buf[EPI_CMD_MAX + 1];
static int epi_cmd_len;
static int epi_cmd_focus;   /* command bar has keyboard focus, swallowing other keys */
static char epi_cmd_err[40];
#define EPI_CMD_NONE 0
#define EPI_CMD_GP   1
#define EPI_CMD_DES  2
static int epi_cmd_view;    /* active result view, EPI_CMD_NONE when nothing is up */
static int epi_cmd_idx;     /* epi_pool index the view is showing */

typedef struct { const char *code; int id; } epi_cmd_def_t;
static const epi_cmd_def_t epi_cmd_table[] = { {"GP", EPI_CMD_GP}, {"DES", EPI_CMD_DES} };
#define EPI_CMD_TABLE_N ((int)(sizeof(epi_cmd_table) / sizeof(epi_cmd_table[0])))

static char epi_upper(char c) { return (c >= 'a' && c <= 'z') ? (char)(c - 32) : c; }
static int epi_streq_ci(const char *a, const char *b) {
    while (*a && *b) { if (epi_upper(*a) != epi_upper(*b)) return 0; a++; b++; }
    return *a == *b;
}
static int epi_find_pool(const char *sym) {
    for (int i = 0; i < EPI_POOL_N; i++) if (epi_streq_ci(sym, epi_pool[i].sym)) return i;
    return -1;
}

/* Splits "AAPL GP" on the last space into an uppercased ticker and code.
   0 on anything malformed (no space, or an empty half). */
static int epi_cmd_split(const char *in, char *ticker, char *code) {
    int len = font_strlen_local(in), sp = -1;
    for (int i = len - 1; i >= 0; i--) if (in[i] == ' ') { sp = i; break; }
    if (sp <= 0 || sp >= len - 1) return 0;
    int ti = 0; for (int i = 0; i < sp && ti < 7; i++) ticker[ti++] = epi_upper(in[i]); ticker[ti] = 0;
    int ci = 0; for (int i = sp + 1; i < len && ci < 7; i++) code[ci++] = epi_upper(in[i]); code[ci] = 0;
    return ti > 0 && ci > 0;
}

/* Runs the typed line: unknown code or ticker leaves a one-line error in
   the bar and never touches epi_cmd_view, so a bad command can't blank
   out a result already on screen. */
static void epi_cmd_run(void) {
    epi_cmd_buf[epi_cmd_len] = 0;
    char ticker[8], code[8];
    if (!epi_cmd_split(epi_cmd_buf, ticker, code)) { stx_cat(epi_cmd_err, 0, "Type TICKER CODE, e.g. AAPL GP"); return; }
    int id = EPI_CMD_NONE;
    for (int i = 0; i < EPI_CMD_TABLE_N; i++) if (epi_streq_ci(code, epi_cmd_table[i].code)) { id = epi_cmd_table[i].id; break; }
    if (id == EPI_CMD_NONE) {
        int p = stx_cat(epi_cmd_err, 0, "Unknown code: "); stx_cat(epi_cmd_err, p, code);
        serial_puts("epicmd=unknown_code:"); serial_puts(code); serial_puts("\n"); /* discriminating marker for tools/checks/epiphany-cmdbar-check.py */
        return;
    }
    int idx = epi_find_pool(ticker);
    if (idx < 0) {
        int p = stx_cat(epi_cmd_err, 0, "Unknown ticker: "); stx_cat(epi_cmd_err, p, ticker);
        serial_puts("epicmd=unknown_ticker:"); serial_puts(ticker); serial_puts("\n");
        return;
    }
    epi_cmd_err[0] = 0;
    epi_watch[idx] = 1; /* GP/DES both add the ticker to the watchlist if it wasn't already on it */
    epi_cmd_view = id; epi_cmd_idx = idx;
    serial_puts("epicmd=run:"); serial_puts(ticker); serial_puts(" "); serial_puts(code); serial_puts("\n"); /* discriminating marker for tools/checks/epiphany-cmdbar-check.py */
}

/* Full chart panel for GP. Only the Stocks app's own STOCKS_MAX symbols
   carry real chart history (stx_data); any other pool ticker is a real,
   live price with no chart series yet, so it gets the same "unavailable"
   treatment Stocks itself shows for a stale/missing quote. */
static void epi_cmd_draw_gp(int x, int y, int w, int h) {
    epi_row_t *s = &epi_pool[epi_cmd_idx];
    char b[64];
    font_draw_string(s->sym, x, y, STX_MUTED, -1);
    font_draw_string(s->name, x, y + 22, STX_INK, -1);
    stocks_format_price(s->price_x100, b, sizeof b); font_draw_string(b, x, y + 50, STX_INK, -1);
    stx_pct(s->bp, b); font_draw_string(b, x + 140, y + 50, epi_col(s->bp), -1);
    int ch = h - 90; if (ch < 60) ch = 60;
    window_rect(x, y + 74, w, 1, 0x00E0D8CE);
    if (epi_cmd_idx < STOCKS_MAX && stx_data[0][epi_cmd_idx].n > 1 && !stx_data[0][epi_cmd_idx].stale) {
        stx_chart(x, y + 82, w, ch, stx_data[0][epi_cmd_idx].points, stx_data[0][epi_cmd_idx].n, epi_col(s->bp));
        font_draw_string("Yahoo Finance / USD / may be delayed", x, y + 82 + ch + 14, STX_MUTED, -1);
    } else {
        font_draw_string(epi_cmd_idx < STOCKS_MAX ? "Stale quote. R to retry." : "No chart history for this ticker yet, live price only.",
                          x, y + 82, STX_MUTED, -1);
    }
}

/* Name, price, day change, and cap/P/E when the data actually has them
   (only the Stocks-tracked symbols carry those two fields today). */
static void epi_cmd_draw_des(int x, int y, int w) {
    epi_row_t *s = &epi_pool[epi_cmd_idx];
    char b[64]; (void)w;
    font_draw_string(s->sym, x, y, STX_MUTED, -1);
    font_draw_string(s->name, x, y + 22, STX_INK, -1);
    stocks_format_price(s->price_x100, b, sizeof b); font_draw_string(b, x, y + 50, STX_INK, -1);
    stx_pct(s->bp, b); font_draw_string(b, x + 140, y + 50, epi_col(s->bp), -1);
    if (epi_cmd_idx < STOCKS_MAX) {
        stocks_entry_t *e = &stocks_entries[epi_cmd_idx];
        font_draw_string("Market cap", x, y + 88, STX_MUTED, -1);
        epi_num(e->cap_b, b); stx_cat(b, font_strlen_local(b), "B"); font_draw_string(b, x, y + 108, STX_INK, -1);
        font_draw_string("P/E", x + 200, y + 88, STX_MUTED, -1);
        epi_num(e->pe_x10 / 10, b); int p = font_strlen_local(b); b[p++] = '.'; b[p++] = '0' + e->pe_x10 % 10; b[p] = 0;
        font_draw_string(b, x + 200, y + 108, STX_INK, -1);
    } else {
        font_draw_string("Market cap and P/E unavailable for this ticker.", x, y + 88, STX_MUTED, -1);
    }
}

static void epi_tab_markets(int x, int y, int w, int sel) {
    char b[64];
    int cw = (w - 32) / 2, x2 = x + cw + 32;
    font_draw_string(epi_adding ? "Add to watchlist (enter or space adds, esc cancels)" : "Watchlist", x, y, epi_adding ? EPI_ACCENT : STX_MUTED, -1);
    int want = epi_adding ? 0 : 1, n = epi_count(want);
    int vis = ((int)window_height() - y - 60) / 22; if (vis < 3) vis = 3;
    if (sel < epi_scroll) epi_scroll = sel;
    if (sel >= epi_scroll + vis) epi_scroll = sel - vis + 1;
    if (epi_scroll > n - vis) epi_scroll = n - vis;
    if (epi_scroll < 0) epi_scroll = 0;
    if (n == 0) font_draw_string(epi_adding ? "Everything is already watched" : "Empty, press a to add", x, y + 24, STX_MUTED, -1);
    if (!epi_adding && n) { epi_num(n, b); stx_cat(b, font_strlen_local(b), " watched"); epi_right(b, x + cw, y, STX_MUTED); }
    for (int r = epi_scroll; r < n && r < epi_scroll + vis; r++) {
        const epi_row_t *s = &epi_pool[epi_nth(want, r)]; int ry = y + 24 + (r - epi_scroll) * 22;
        if (r == sel) window_rect(x - 8, ry - 4, cw + 16, 22, 0x00E2D9CC);
        font_draw_string(s->sym, x, ry, STX_INK, -1);
        stocks_format_price(s->price_x100, b, sizeof b); epi_right(b, x + cw - 90, ry, STX_INK);
        stx_pct(s->bp, b); epi_right(b, x + cw, ry, epi_col(s->bp));
    }
    font_draw_string("Crypto and commodities", x2, y, STX_MUTED, -1);
    for (int i = 0; i < EPI_ALT_N; i++) {
        int ry = y + 24 + i * 22;
        font_draw_string(epi_alt[i].sym, x2, ry, STX_INK, -1);
        stocks_format_price(epi_alt[i].price_x100, b, sizeof b); epi_right(b, x2 + cw - 90, ry, STX_INK);
        stx_pct(epi_alt[i].bp, b); epi_right(b, x2 + cw, ry, epi_col(epi_alt[i].bp));
    }
    int y3 = y + 24 + EPI_ALT_N * 22 + 16, fg = 62; /* fear and greed, 0-100 */
    font_draw_string("Fear and greed", x2, y3, STX_MUTED, -1);
    window_rect(x2, y3 + 24, cw, 10, 0x00E0D8CE);
    window_rect(x2, y3 + 24, cw * fg / 100, 10, epi_col(fg - 50));
    epi_num(fg, b); stx_cat(b, font_strlen_local(b), "  Greed");
    font_draw_string(b, x2, y3 + 40, STX_INK, -1);
    if (!epi_adding) font_draw_string("a add   d remove", x2, y3 + 68, STX_MUTED, -1);
}

static void epi_tab_portfolio(int x, int y, int w, int sel) {
    char b[64];
    const char *hd[4] = {"Shares", "Cost", "Value", "P/L"};
    int cx[4] = {x + 170, x + 290, x + 430, x + w};
    font_draw_string("Holding", x, y, STX_MUTED, -1);
    for (int i = 0; i < 4; i++) epi_right(hd[i], cx[i], y, STX_MUTED);
    int total = 0, cost = 0;
    for (int i = 0; i < EPI_HOLD_N; i++) {
        stocks_entry_t *s = &epi_demo_entries[epi_hold[i].stock];
        int val = epi_hold[i].shares * s->price_x100, cb = epi_hold[i].shares * epi_hold[i].cost_x100;
        total += val; cost += cb;
        int ry = y + 24 + i * 22;
        if (i == sel) window_rect(x - 8, ry - 4, w + 16, 22, 0x00E2D9CC);
        font_draw_string(s->symbol, x, ry, STX_INK, -1);
        epi_num(epi_hold[i].shares, b); epi_right(b, cx[0], ry, STX_INK);
        epi_money(cb, b); epi_right(b, cx[1], ry, STX_MUTED);
        epi_money(val, b); epi_right(b, cx[2], ry, STX_INK);
        epi_money(val - cb, b); epi_right(b, cx[3], ry, epi_col(val - cb));
    }
    int ty = y + 24 + EPI_HOLD_N * 22 + 10;
    window_rect(x, ty - 6, w, 1, 0x00E0D8CE);
    font_draw_string("Total", x, ty + 4, STX_INK, -1);
    epi_money(total, b); epi_right(b, cx[2], ty + 4, STX_INK);
    epi_money(total - cost, b); epi_right(b, cx[3], ty + 4, epi_col(total - cost));
    int pct = cost > 100 ? ((total - cost) / 100) * 10000 / (cost / 100) : 0; /* /100 first keeps it in 32 bits */
    stx_pct(pct, b); epi_right(b, cx[3], ty + 26, epi_col(pct));
    font_draw_string("Allocation", x, ty + 48, STX_MUTED, -1);
    static const unsigned int seg[EPI_HOLD_N] = {0x000A84FF, 0x0041854B, 0x00C98A1B, 0x00B51616, 0x00707070};
    int ax = x;
    for (int i = 0; i < EPI_HOLD_N && total > 0; i++) {
        int sw = (int)(epi_hold[i].shares * epi_demo_entries[epi_hold[i].stock].price_x100 * w / total);
        window_rect(ax, ty + 70, sw, 12, seg[i]); ax += sw;
    }
    font_draw_string("up/down pick   + / - shares", x, ty + 90, STX_MUTED, -1);
}

static void epi_tab_sim(int x, int y, int w, int h) {
    char b[64]; int px = epi_sim_px[epi_sim_n - 1];
    font_draw_string("Simulator", x, y, STX_MUTED, -1);
    stocks_format_price(px, b, sizeof b);
    font_draw_string(b, x, y + 22, STX_INK, -1);
    int chg = px - epi_sim_px[0];
    stx_pct((int)(chg * 10000 / epi_sim_px[0]), b);
    font_draw_string(b, x + 120, y + 22, epi_col(chg), -1);
    int ch = h - 190; if (ch < 70) ch = 70;
    window_rect(x, y + 54, w, 1, 0x00E0D8CE);
    if (epi_sim_n > 1) stx_chart(x, y + 62, w, ch, epi_sim_px, epi_sim_n, epi_col(chg));
    int by = y + 62 + ch + 16, eq = epi_sim_equity();
    epi_money(eq, b); font_draw_string("Equity", x, by, STX_MUTED, -1); font_draw_string(b, x, by + 20, STX_INK, -1);
    epi_money(eq - 1000000, b); font_draw_string("P/L", x + 180, by, STX_MUTED, -1); font_draw_string(b, x + 180, by + 20, epi_col(eq - 1000000), -1);
    epi_num(epi_sim_pos, b); font_draw_string("Position", x + 360, by, STX_MUTED, -1); font_draw_string(b, x + 360, by + 20, STX_INK, -1);
    /* edge readout: distance from the recent mean, in basis points, the input a mean-reversion trader watches */
    int mean = 0, m = epi_sim_n < 20 ? epi_sim_n : 20;
    for (int i = epi_sim_n - m; i < epi_sim_n; i++) mean += epi_sim_px[i];
    mean /= m;
    int edge = (int)((px - mean) * 10000 / mean);
    stx_pct(edge, b); font_draw_string("Vs 20-tick mean", x + 520, by, STX_MUTED, -1); font_draw_string(b, x + 520, by + 20, epi_col(-edge), -1);
    font_draw_string(epi_sim_paused ? "b buy 10   s sell 10   space resume   r reset" : "b buy 10   s sell 10   space pause   r reset", x, by + 52, STX_MUTED, -1);
}

static void epi_tab_situation(int x, int y, int w) {
    static const struct { const char *n; const char *v; int bp; } r[] = {
        {"VIX", "16.42", -310}, {"US 10Y", "4.18%", 120}, {"DXY", "103.9", 20}, {"Fed funds", "4.50%", 0}, {"S&P 500", "5,812", 45},
    };
    font_draw_string("Macro pulse", x, y, STX_MUTED, -1);
    for (int i = 0; i < 5; i++) {
        int ry = y + 24 + i * 22;
        font_draw_string(r[i].n, x, ry, STX_INK, -1);
        epi_right(r[i].v, x + 260, ry, STX_INK);
        if (r[i].bp) { char b[32]; stx_pct(r[i].bp, b); epi_right(b, x + 380, ry, epi_col(r[i].bp)); }
    }
    int by = y + 24 + 5 * 22 + 16;
    font_draw_string("Daily brief", x, by, STX_MUTED, -1);
    font_draw_string("Risk appetite firm: fear and greed sits in Greed, volatility is soft.", x, by + 24, STX_INK, -1);
    font_draw_string("Rates steady, dollar flat. Mega-cap tech leads, autos and retail lag.", x, by + 46, STX_INK, -1);
    font_draw_string("Sample macro data. The live map and People graph are not ported yet.", x, by + 78, STX_MUTED, -1);
    (void)w;
}

static void epi_draw(int tab, int sel) {
    int WW = (int)window_width(), HH = (int)window_height(), T = stx_top();
    window_clear(GUI_BG);
    gui_draw_app_titlebar("Epiphany");
    for (int i = 0; i < EPI_TABS; i++) {
        int tx = 24 + i * 110;
        if (i == tab) { window_rect(tx - 8, T, 100, 26, EPI_ACCENT); font_draw_string(epi_tab_name[i], tx, T + 4, 0x00FFFFFF, -1); }
        else font_draw_string(epi_tab_name[i], tx, T + 4, STX_MUTED, -1);
    }
    window_rect(0, T + 32, WW, 1, 0x00E0D8CE);
    int x = 32, y = T + 48, w = WW - 64; if (w > 760) w = 760;
    int bar_y = HH - 56;
    if (epi_cmd_view == EPI_CMD_GP) epi_cmd_draw_gp(x, y, w, bar_y - y - 12);
    else if (epi_cmd_view == EPI_CMD_DES) epi_cmd_draw_des(x, y, w);
    else if (tab == 0) epi_tab_markets(x, y, w, sel);
    else if (tab == 1) epi_tab_portfolio(x, y, w, sel);
    else if (tab == 2) epi_tab_sim(x, y, w, bar_y - y);
    else epi_tab_situation(x, y, w);

    window_rect(20, bar_y, WW - 40, 1, 0x00E0D8CE);
    if (epi_cmd_err[0]) font_draw_string(epi_cmd_err, 20, bar_y + 10, STX_RED, -1);
    else if (epi_cmd_focus) {
        char shown[EPI_CMD_MAX + 2]; int p = stx_cat(shown, 0, "/ "); p = stx_cat(shown, p, epi_cmd_buf); stx_cat(shown, p, "_");
        font_draw_string(shown, 20, bar_y + 10, EPI_ACCENT, -1);
    } else font_draw_string("/ command   AAPL GP   AAPL DES", 20, bar_y + 10, STX_MUTED, -1);

    font_draw_string(epi_live ? "Live quotes, crypto and macro are sample   r refresh   left/right tabs   esc closes"
                              : "Offline, sample prices   r retry   left/right tabs   esc closes", 20, HH - 24, STX_MUTED, -1);
    window_present();
}

/* Non-blocking twin of get_key_or_click: -1 when nothing is pending, so the
   simulator can tick on its own clock. */
static int epi_poll(void) {
    gui_app_mouse_tick();
    int sc = kbd_pop();
    if (sc >= 0) {
        if (sc == 0xE0) {
            int s2; do { s2 = kbd_pop(); } while (s2 < 0);
            return s2 == 0x48 ? KEY_UP : s2 == 0x50 ? KEY_DOWN : s2 == 0x4B ? KEY_LEFT : s2 == 0x4D ? KEY_RIGHT : -1;
        }
        if (!(sc & 0x80)) { char c = kbd_map(sc); if (c == 27) return KEY_ESC; if (c) return c; }
        return -1;
    }
    if (mouse_click_edge()) return KEY_CLICK;
    return -1;
}

static void gui_launch_epiphany(void) {
    int tab = 0, sel = 0, frame = 0;
    epi_adding = 0; epi_scroll = 0;
    epi_cmd_focus = 0; epi_cmd_len = 0; epi_cmd_buf[0] = 0; epi_cmd_err[0] = 0; epi_cmd_view = EPI_CMD_NONE;
    epi_sim_reset();
    for (int i = 0; i < 40; i++) epi_sim_step();
    mouse_click_edge_sync();
    epi_draw(tab, sel);
    epi_refresh();
    for (;;) {
        if (ticks() - stx_refresh_tick >= 6000) epi_refresh(); /* once a minute, same as Stocks */
        epi_draw(tab, sel);
        sleep_ticks(2);
        int k;
        while ((k = epi_poll()) != -1) {
            if (epi_cmd_focus) {
                if (k == KEY_ESC) { epi_cmd_focus = 0; epi_cmd_len = 0; epi_cmd_err[0] = 0; continue; }
                if (k == '\n' || k == KEY_ENTER) { epi_cmd_focus = 0; epi_cmd_run(); continue; }
                if (k == '\b') { if (epi_cmd_len > 0) epi_cmd_len--; continue; }
                if (k >= 32 && k < 127 && epi_cmd_len < EPI_CMD_MAX) epi_cmd_buf[epi_cmd_len++] = (char)k;
                continue; /* focused bar swallows every other key */
            }
            if (k == '/') { epi_cmd_focus = 1; epi_cmd_len = 0; epi_cmd_buf[0] = 0; epi_cmd_err[0] = 0; continue; }
            int lim = tab == 0 ? epi_count(epi_adding ? 0 : 1) : EPI_HOLD_N;
            if (k == KEY_ESC) { if (epi_cmd_view) { epi_cmd_view = 0; epi_cmd_err[0] = 0; continue; } if (epi_adding) { epi_adding = 0; sel = 0; continue; } return; }
            if (tab == 0 && k == 'a' && !epi_adding && epi_count(0) > 0) { epi_adding = 1; sel = 0; continue; }
            if (tab == 0 && epi_adding && (k == KEY_ENTER || k == ' ')) { int p = epi_nth(0, sel); if (p >= 0) epi_watch[p] = 1; epi_adding = 0; sel = 0; continue; }
            if (tab == 0 && !epi_adding && k == 'd') { int p = epi_nth(1, sel); if (p >= 0) epi_watch[p] = 0; if (sel > 0 && sel >= epi_count(1)) sel--; continue; }
            if (k == KEY_LEFT && tab > 0) { tab--; sel = 0; epi_cmd_view = 0; }
            else if (k == KEY_RIGHT && tab < EPI_TABS - 1) { tab++; sel = 0; epi_cmd_view = 0; }
            else if (!epi_adding && k >= '1' && k <= '4') { tab = k - '1'; sel = 0; epi_cmd_view = 0; }
            else if (k == KEY_UP && sel > 0) sel--;
            else if (k == KEY_DOWN && sel < lim - 1) sel++;
            else if (tab == 1 && (k == '+' || k == '=') && epi_hold[sel].shares < 9999) epi_hold[sel].shares++;
            else if (tab == 1 && k == '-' && epi_hold[sel].shares > 0) epi_hold[sel].shares--;
            else if (tab == 2 && k == 'b' && epi_sim_cash >= 10 * epi_sim_px[epi_sim_n - 1]) { epi_sim_cash -= 10 * epi_sim_px[epi_sim_n - 1]; epi_sim_pos += 10; }
            else if (tab == 2 && k == 's' && epi_sim_pos >= 10) { epi_sim_cash += 10 * epi_sim_px[epi_sim_n - 1]; epi_sim_pos -= 10; }
            else if (tab == 2 && k == ' ') epi_sim_paused = !epi_sim_paused;
            else if (tab == 2 && k == 'r') epi_sim_reset();
            else if (k == 'r' || k == 'R') epi_refresh();
            else if (k == KEY_CLICK) {
                if (!gui_app_windowed) return;
                int mx = app_cursor_x - app_view_x, my = app_cursor_y - app_view_y;
                if (mx < 0 || my < 0 || mx >= (int)window_width() || my >= (int)window_height()) return; /* chrome X or dock */
                if (my >= stx_top() && my < stx_top() + 28) for (int i = 0; i < EPI_TABS; i++) if (mx >= 16 + i * 110 && mx < 116 + i * 110) { tab = i; sel = 0; epi_adding = 0; }
            }
        }
        if (tab == 2 && !epi_sim_paused && ++frame % 3 == 0) epi_sim_step();
    }
}
