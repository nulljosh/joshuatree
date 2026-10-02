/* Stocks data side. The UI is user/stocks.c, a ring-3 program; this file only
   fetches and parses. Quotes and chart closes come from the site's HTTPS Worker
   bridge (/api/stocks?range=N). After every fetch stocks_write_file leaves
   STOCKS.TXT for the app, and the desktop loop refetches on a SYS_REFRESH request (398). */

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
static struct { int price, prev, time, n, points[STX_MAXPTS], stale; } stx_data[STX_RANGES][STOCKS_MAX];
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
/* Ring-3 Stocks (user/stocks.c) cannot see these statics and SYS_HTTP_GET caps
   a body at 2KB, so after every fetch the kernel leaves STOCKS.TXT for it, the
   Weather way. Line 1 "range R sel S"; then one line per symbol:
   "i stale price prev time dprice dprev n p0 .. pn-1" (dprice/dprev are the 1D
   quote the sidebar pill uses; points are cut to STX_FILE_PTS by even
   sampling). About 2KB, far under the ramfs file cap (JT_USER_IMAGE_MAX). */
#define STX_FILE_PTS 32
static int stx_sel_hint;
char *wx_put_int(char *o, int v);
static void stocks_write_file(int range, int sel) {
    char *b = kmalloc(4096), *o = b; /* heap, not .bss: the ring-3 window sits right after .bss */
    if (!b) return;
    *o++ = 'r'; *o++ = ' '; o = wx_put_int(o, range); *o++ = ' '; o = wx_put_int(o, sel); *o++ = '\n';
    for (int i = 0; i < STOCKS_MAX; i++) {
        int n = stx_data[range][i].n, m = n > STX_FILE_PTS ? STX_FILE_PTS : n;
        o = wx_put_int(o, i); *o++ = ' ';
        o = wx_put_int(o, stx_data[range][i].stale); *o++ = ' ';
        o = wx_put_int(o, stx_data[range][i].price); *o++ = ' ';
        o = wx_put_int(o, stx_data[range][i].prev); *o++ = ' ';
        o = wx_put_int(o, stx_data[range][i].time); *o++ = ' ';
        o = wx_put_int(o, stx_data[0][i].price); *o++ = ' ';
        o = wx_put_int(o, stx_data[0][i].prev); *o++ = ' ';
        o = wx_put_int(o, m);
        for (int j = 0; j < m; j++) { *o++ = ' '; o = wx_put_int(o, stx_data[range][i].points[m > 1 ? j * (n - 1) / (m - 1) : 0]); }
        *o++ = '\n';
    }
    vfs_replace_file("STOCKS.TXT", b, (unsigned int)(o - b));
    jt_data_stamp++;
    kfree(b);
}
/* stkhost=HOST:PORT on the multiboot command line points the fetch at a fake
   server (tools/checks/stocks-aa-check.py), the way wxhost= does for Weather. */
static char stk_host_override[32];
static int stk_port_override;
static void stocks_cmdline(const char *cl) {
    for (const char *pc = cl; pc && *pc; pc++)
        if (pc[0]=='s' && pc[1]=='t' && pc[2]=='k' && pc[3]=='h' && pc[4]=='o' && pc[5]=='s' && pc[6]=='t' && pc[7]=='=') {
            pc += 8; int hp = 0;
            while (*pc && *pc != ' ' && *pc != ':' && hp < 31) stk_host_override[hp++] = *pc++;
            stk_host_override[hp] = 0;
            if (*pc == ':') { pc++; int pt = 0; while (*pc >= '0' && *pc <= '9') pt = pt * 10 + (*pc++ - '0'); stk_port_override = pt; }
            break;
        }
}
static void stocks_fetch(int range) {
    char *body = kmalloc(8192);
    if (!body) return;
    char path[] = "/api/stocks?range=0";
    path[sizeof(path) - 2] = '0' + range;
    int n = net_init(0x0A00020F) ? http_get_timeout(stk_host_override[0] ? stk_host_override : "joshuatree.heyitsmejosh.com", path, stk_port_override ? stk_port_override : 80, body, 8191, 1000) : -1;
    for (int i = 0; i < STOCKS_MAX; i++) stx_data[range][i].stale = 1;
    if (n > 0 && n < 8191 && http_last_status() == 200) {
        body[n] = 0; const char *p = body;
        for (int i = 0; i < STOCKS_MAX && *p; i++) {
            stx_parse_row(p, range, i);
            while (*p && *p != '\n') p++;
            if (*p) p++;
        }
    }
    kfree(body);
    stx_refresh_tick = ticks();
    stocks_write_file(range, stx_sel_hint);
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

/* The dock's Stocks, blocking fallback only (a full window table). The window asks for a
   refetch with SYS_REFRESH (range | sel << 8); here it fetches once and runs once. */
int stocks_ring3_run(void);
static int stx_range_hint;
static void stocks_ring3_open(void){
    stx_range_hint = 0; stx_sel_hint = 0;
    stocks_fetch(0);
    stocks_ring3_run();
}
