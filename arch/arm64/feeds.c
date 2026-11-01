/* feeds.c, 7.6.0: live data for the Pi's Stocks and Weather, held in RAM.
 *
 * The i386 kernel fetches quotes into STOCKS.TXT; the Pi's Stocks (user/stocks_pi.c) read that file off the card and,
 * with none, drew a made-up sample. Now opening Stocks fetches the site Worker's /api/stocks (Yahoo quotes, one line per
 * symbol: "price prev time n p0 .. pn-1", cents) and keeps them here in the file's own format; ufile.c's readfile
 * answers STOCKS.TXT from this copy, so nothing is written to the card. No network, or a bad reply: nothing is kept, and
 * the app says it has no prices. Weather (7.6.0) is the same: /api/weather already answers in WEATHER.TXT's lines, kept
 * as they came once they look whole. FEEDS_TEST_PORT (the feedsfix build) points both at a check's own server.
 */
int browser_get(const char *url, char **body, unsigned *n);   /* browser.c */
unsigned net_get_gateway(void);                                /* net.c */
extern unsigned int net_dns_wait_ticks, net_connect_wait_ticks;   /* net.c */
unsigned long heap_mark(void);
void heap_release(unsigned long m);
void kputs(const char *s);

#ifdef FEEDS_TEST_PORT   /* the feedsfix build: arm64-stocks-check.py and arm64-weather-check.py serve fixtures on the host */
#define STR_(x) #x
#define STR(x) STR_(x)
#define FEEDS_HOST "http://10.0.2.2:" STR(FEEDS_TEST_PORT)
#else
#define FEEDS_HOST "https://joshuatree.jaybulb.com"
#endif
#define STOCKS_URL FEEDS_HOST "/api/stocks?range=0"
#define WEATHER_URL FEEDS_HOST "/api/weather"   /* the Worker answers with the caller's own place (request.cf) and Open-Meteo's forecast */
#define ROWS 8
#define PTS 32   /* stocks_pi.c's PTS: the points are cut to this by even sampling, as kernel/stocks.c does */

static char stocks_txt[4096], stocks_new[4096];
static unsigned stocks_len;
/* The open waits on this: short DNS and connect waits, as the Browser's own page view (browser.c show) uses, not the
   20 to 50 s WAN defaults, so a Wi-Fi with no internet costs a few seconds, not the desktop for a minute. */
static int get(const char *url, char **body, unsigned *n) {
    unsigned d = net_dns_wait_ticks, c = net_connect_wait_ticks;
    net_dns_wait_ticks = 300; net_connect_wait_ticks = 400;
    int st = browser_get(url, body, n);
    net_dns_wait_ticks = d; net_connect_wait_ticks = c;
    return st;
}

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
void feeds_stocks(void) {   /* a good reply replaces the kept quotes; a failed one leaves the last good ones */
    if (!net_get_gateway()) { kputs("stocks: no network, no prices\n"); return; }
    unsigned long mark = heap_mark();
    char *body = 0; unsigned n = 0;
    int st = get(STOCKS_URL, &body, &n);
    if (st == 200 && body && n) {
        char *o = stocks_new, *end = stocks_new + sizeof stocks_new;   /* a row is at most about 420 bytes: eight fit */
        const char *p = body;
        unsigned good = 0;
        *o++ = 'r'; *o++ = ' '; *o++ = '0'; *o++ = ' '; *o++ = '0'; *o++ = '\n';
        for (unsigned i = 0; i < ROWS && *p && end - o > 480; i++) good += (unsigned)row(&p, &o, end, i);
        if (good) { stocks_len = (unsigned)(o - stocks_new); for (unsigned k = 0; k < stocks_len; k++) stocks_txt[k] = stocks_new[k]; }
        kputs(good ? "stocks: live prices fetched\n" : "stocks: the reply held no prices\n");
    } else kputs("stocks: could not fetch prices\n");
    heap_release(mark);
}
static char weather_txt[1024];
static unsigned weather_len;
void feeds_weather(void) {   /* the same: a failed fetch keeps the last good reading */
    if (!net_get_gateway()) { kputs("weather: no network, no reading\n"); return; }
    unsigned long mark = heap_mark();
    char *body = 0; unsigned n = 0;
    int st = get(WEATHER_URL, &body, &n);
    int ok = st == 200 && body && n > 5 && n < sizeof weather_txt && body[0] == 'c' && body[1] == 'i' && body[2] == 't' && body[3] == 'y' && body[4] == ' ';
    for (unsigned i = 0; ok && i < n; i++) if ((unsigned char)body[i] < ' ' && body[i] != '\n') ok = 0;   /* text lines only */
    if (ok) { for (unsigned i = 0; i < n; i++) weather_txt[i] = body[i]; weather_len = n; }
    kputs(ok ? "weather: live reading fetched\n" : "weather: could not fetch a reading\n");
    heap_release(mark);
}
/* ufile.c's readfile: STOCKS.TXT or WEATHER.TXT from RAM when a fetch kept one; -1 sends it on to the card. */
static int named(const char *leaf, const char *w) {
    unsigned i = 0;
    for (; w[i]; i++) { char a = leaf[i]; if (a >= 'a' && a <= 'z') a -= 32; if (a != w[i]) return 0; }
    return !leaf[i];
}
int feeds_read(const char *leaf, char *buf, unsigned cap) {
    const char *src; unsigned len;
    if (named(leaf, "STOCKS.TXT")) { src = stocks_txt; len = stocks_len; }
    else if (named(leaf, "WEATHER.TXT")) { src = weather_txt; len = weather_len; }
    else return -1;
    if (!len) return -1;
    unsigned k = len < cap ? len : cap;
    for (unsigned j = 0; j < k; j++) buf[j] = src[j];
    return (int)k;
}
void feeds_for(const char *app) {   /* appwin.c's app_go, before an app starts: the data it reads, fetched now */
    if (named(app, "STOCKS")) feeds_stocks();
    else if (named(app, "WEATHER")) feeds_weather();
}
