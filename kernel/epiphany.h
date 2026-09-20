/* v0.87+: Epiphany, the offline slice of the real app (github.com/nulljosh/epiphany).
   The real one is a React/Supabase/Stripe stack that is HTTPS end to end, and
   this kernel has no TLS, so it cannot run here as-is (roadmap.md). What ports
   honestly is the part that needs no network: the tab shell and the four tabs'
   local logic, over baked-in demo data.

   Markets    equities from Stocks' own table, plus crypto, commodities, fear/greed
   Portfolio  holdings valued off the same prices, P/L, allocation bar; +/- edits shares
   Simulator  a live-ticking random-walk market you trade against, with edge readout
   Situation  macro pulse and a short brief
   Not ported: the live map, People graph, accounts/billing/sync (all need network).

   Keys: left/right or 1-4 switch tab, up/down select, + / - shares, b/s/space in
   the simulator, esc closes. Clicking a tab switches to it; the red dot closes. */

#define EPI_TABS 4
static const char *epi_tab_name[EPI_TABS] = {"Markets", "Portfolio", "Simulator", "Situation"};
#define EPI_ACCENT 0x000A84FF

typedef struct { const char *sym, *name; int price_x100, bp; } epi_row_t;
static const epi_row_t epi_alt[] = {
    {"BTC", "Bitcoin", 6420000, 210}, {"ETH", "Ethereum", 245000, -140},
    {"GC", "Gold", 265000, 35}, {"CL", "WTI Crude", 7240, -90}, {"SI", "Silver", 3120, 120},
};
#define EPI_ALT_N 5

/* Epiphany's own watchlist: a pool of tickers, a flag per ticker for whether it is watched. */
#define EPI_POOL_N 14
static const epi_row_t epi_pool[EPI_POOL_N] = {
    {"AAPL", "Apple", 23800, 106}, {"MSFT", "Microsoft", 41900, -43}, {"GOOGL", "Alphabet", 14200, 252},
    {"AMZN", "Amazon", 19100, -164}, {"TSLA", "Tesla", 24200, 372}, {"NVDA", "NVIDIA", 12800, 330},
    {"META", "Meta", 58000, -109}, {"NFLX", "Netflix", 66000, 182}, {"AMD", "AMD", 15600, 290},
    {"DIS", "Disney", 9800, -60}, {"JPM", "JPMorgan", 21500, 45}, {"COIN", "Coinbase", 24800, 540},
    {"SHOP", "Shopify", 8900, 210}, {"UBER", "Uber", 7400, -85},
};
static int epi_watch[EPI_POOL_N] = {1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0};
/* k-th watched (or, with want=0, unwatched) pool index; -1 past the end */
static int epi_nth(int want, int k) { for (int i = 0; i < EPI_POOL_N; i++) if (!!epi_watch[i] == want && k-- == 0) return i; return -1; }
static int epi_count(int want) { int n = 0; for (int i = 0; i < EPI_POOL_N; i++) n += !!epi_watch[i] == want; return n; }
static int epi_adding;

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

static void epi_tab_markets(int x, int y, int w, int sel) {
    char b[64];
    int cw = (w - 32) / 2, x2 = x + cw + 32;
    font_draw_string(epi_adding ? "Add to watchlist (enter or space adds, esc cancels)" : "Watchlist", x, y, epi_adding ? EPI_ACCENT : STX_MUTED, -1);
    int want = epi_adding ? 0 : 1, n = epi_count(want);
    if (n == 0) font_draw_string(epi_adding ? "Everything is already watched" : "Empty, press a to add", x, y + 24, STX_MUTED, -1);
    for (int r = 0; r < n && r < 9; r++) {
        const epi_row_t *s = &epi_pool[epi_nth(want, r)]; int ry = y + 24 + r * 22;
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
        stocks_entry_t *s = &stocks_entries[epi_hold[i].stock];
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
        int sw = (int)(epi_hold[i].shares * stocks_entries[epi_hold[i].stock].price_x100 * w / total);
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
    font_draw_string("Demo data. The live map and People graph need the network stack.", x, by + 78, STX_MUTED, -1);
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
    if (tab == 0) epi_tab_markets(x, y, w, sel);
    else if (tab == 1) epi_tab_portfolio(x, y, w, sel);
    else if (tab == 2) epi_tab_sim(x, y, w, HH - y);
    else epi_tab_situation(x, y, w);
    font_draw_string("Demo data, not live   left/right tabs   esc closes", 20, HH - 24, STX_MUTED, -1);
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
        if (!(sc & 0x80)) { char c = SC[sc & 0x7F]; if (c == 27) return KEY_ESC; if (c) return c; }
        return -1;
    }
    if (mouse_click_edge()) return KEY_CLICK;
    return -1;
}

static void gui_launch_epiphany(void) {
    int tab = 0, sel = 0, frame = 0;
    epi_adding = 0;
    epi_sim_reset();
    for (int i = 0; i < 40; i++) epi_sim_step();
    mouse_click_edge_sync();
    for (;;) {
        epi_draw(tab, sel);
        sleep_ticks(2);
        int k;
        while ((k = epi_poll()) != -1) {
            int lim = tab == 0 ? epi_count(epi_adding ? 0 : 1) : EPI_HOLD_N; if (lim > 9 && tab == 0) lim = 9;
            if (k == KEY_ESC) { if (epi_adding) { epi_adding = 0; sel = 0; continue; } return; }
            if (tab == 0 && k == 'a' && !epi_adding && epi_count(0) > 0) { epi_adding = 1; sel = 0; continue; }
            if (tab == 0 && epi_adding && (k == KEY_ENTER || k == ' ')) { int p = epi_nth(0, sel); if (p >= 0) epi_watch[p] = 1; epi_adding = 0; sel = 0; continue; }
            if (tab == 0 && !epi_adding && k == 'd') { int p = epi_nth(1, sel); if (p >= 0) epi_watch[p] = 0; if (sel > 0 && sel >= epi_count(1)) sel--; continue; }
            if (k == KEY_LEFT && tab > 0) { tab--; sel = 0; }
            else if (k == KEY_RIGHT && tab < EPI_TABS - 1) { tab++; sel = 0; }
            else if (!epi_adding && k >= '1' && k <= '4') { tab = k - '1'; sel = 0; }
            else if (k == KEY_UP && sel > 0) sel--;
            else if (k == KEY_DOWN && sel < lim - 1) sel++;
            else if (tab == 1 && (k == '+' || k == '=') && epi_hold[sel].shares < 9999) epi_hold[sel].shares++;
            else if (tab == 1 && k == '-' && epi_hold[sel].shares > 0) epi_hold[sel].shares--;
            else if (tab == 2 && k == 'b' && epi_sim_cash >= 10 * epi_sim_px[epi_sim_n - 1]) { epi_sim_cash -= 10 * epi_sim_px[epi_sim_n - 1]; epi_sim_pos += 10; }
            else if (tab == 2 && k == 's' && epi_sim_pos >= 10) { epi_sim_cash += 10 * epi_sim_px[epi_sim_n - 1]; epi_sim_pos -= 10; }
            else if (tab == 2 && k == ' ') epi_sim_paused = !epi_sim_paused;
            else if (tab == 2 && k == 'r') epi_sim_reset();
            else if (k == KEY_CLICK) {
                int mx = app_cursor_x, my = app_cursor_y;
                if (!gui_app_windowed && mx < 80 && my < 36) return;
                if (my >= stx_top() && my < stx_top() + 28) for (int i = 0; i < EPI_TABS; i++) if (mx >= 16 + i * 110 && mx < 116 + i * 110) { tab = i; sel = 0; }
            }
        }
        if (tab == 2 && !epi_sim_paused && ++frame % 3 == 0) epi_sim_step();
    }
}
