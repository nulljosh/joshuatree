/* feeds.c, 7.6.0: live data for the Pi's Stocks, held in RAM.
 *
 * The i386 kernel fetches quotes into STOCKS.TXT; the Pi's Stocks (user/stocks_pi.c) read that file off the card and,
 * with none, drew a made-up sample. Now opening Stocks fetches the site Worker's /api/stocks (Yahoo quotes, one line per
 * symbol: "price prev time n p0 .. pn-1", cents) and keeps them here in the file's own format; ufile.c's readfile
 * answers STOCKS.TXT from this copy, so nothing is written to the card. No network, or a bad reply: nothing is kept, and
 * the app says it has no prices. STOCKS_URL is the source; the stocksfix test build points it at the check's server.
 */
int browser_get(const char *url, char **body, unsigned *n);   /* browser.c */
unsigned net_get_gateway(void);                                /* net.c */
unsigned long heap_mark(void);
void heap_release(unsigned long m);
void kputs(const char *s);

#ifdef STOCKS_TEST_PORT   /* the stocksfix build: arm64-stocks-check.py serves a fixture on the host */
#define STR_(x) #x
#define STR(x) STR_(x)
#define STOCKS_URL "http://10.0.2.2:" STR(STOCKS_TEST_PORT) "/api/stocks?range=0"
#else
#define STOCKS_URL "https://joshuatree.jaybulb.com/api/stocks?range=0"
#endif
#define ROWS 8
#define PTS 32   /* stocks_pi.c's PTS: the points are cut to this by even sampling, as kernel/stocks.c does */

static char stocks_txt[4096];
static unsigned stocks_len;

static char *put(char *o, const char *end, unsigned v) {
    char t[12]; int n = 0;
    do t[n++] = (char)('0' + v % 10); while ((v /= 10) && n < 11);
    while (n && o < end) *o++ = t[--n];
    return o;
}
/* One wire line into "i stale price prev time dprice dprev m p..": 0 when it is not a whole, sane quote. */
static int row(const char **pp, char **oo, const char *end, unsigned i) {
    unsigned v[4 + 64], c = 0;
    const char *p = *pp;
    while (c < 4 + 64) {
        unsigned x = 0, d = 0;
        while (*p >= '0' && *p <= '9' && d < 10) { x = x * 10 + (unsigned)(*p++ - '0'); d++; }
        if (!d) break;
        v[c++] = x;
        if (*p == ' ') p++; else break;
    }
    while (*p && *p != '\n') p++;
    if (*p) p++;
    *pp = p;
    unsigned n = c >= 4 ? v[3] : 0;
    if (c < 5 || n < 2 || n > 64 || c != 4 + n || !v[0] || !v[1]) return 0;
    unsigned m = n > PTS ? PTS : n;
    char *o = *oo;
    o = put(o, end, i); *o++ = ' '; *o++ = '0';
    for (unsigned k = 0; k < 3; k++) { *o++ = ' '; o = put(o, end, v[k]); }   /* price prev time */
    *o++ = ' '; o = put(o, end, v[0]); *o++ = ' '; o = put(o, end, v[1]);    /* the day's quote is this quote: range 0 */
    *o++ = ' '; o = put(o, end, m);
    for (unsigned j = 0; j < m; j++) { *o++ = ' '; o = put(o, end, v[4 + (m > 1 ? j * (n - 1) / (m - 1) : 0)]); }
    *o++ = '\n';
    *oo = o;
    return 1;
}
void feeds_stocks(void) {
    stocks_len = 0;
    if (!net_get_gateway()) { kputs("stocks: no network, no prices\n"); return; }
    unsigned long mark = heap_mark();
    char *body = 0; unsigned n = 0;
    int st = browser_get(STOCKS_URL, &body, &n);
    if (st == 200 && body && n) {
        char *o = stocks_txt, *end = stocks_txt + sizeof stocks_txt;   /* a row is at most about 420 bytes: eight fit */
        const char *p = body;
        unsigned good = 0;
        *o++ = 'r'; *o++ = ' '; *o++ = '0'; *o++ = ' '; *o++ = '0'; *o++ = '\n';
        for (unsigned i = 0; i < ROWS && *p && end - o > 480; i++) good += (unsigned)row(&p, &o, end, i);
        if (good) stocks_len = (unsigned)(o - stocks_txt);
        kputs(good ? "stocks: live prices fetched\n" : "stocks: the reply held no prices\n");
    } else kputs("stocks: could not fetch prices\n");
    heap_release(mark);
}
/* ufile.c's readfile: STOCKS.TXT from RAM when a fetch kept one; -1 sends it on to the card. */
int feeds_read(const char *leaf, char *buf, unsigned cap) {
    const char *w = "STOCKS.TXT"; unsigned i = 0;
    for (; w[i]; i++) { char a = leaf[i]; if (a >= 'a' && a <= 'z') a -= 32; if (a != w[i]) return -1; }
    if (leaf[i] || !stocks_len) return -1;
    unsigned k = stocks_len < cap ? stocks_len : cap;
    for (unsigned j = 0; j < k; j++) buf[j] = stocks_txt[j];
    return (int)k;
}
