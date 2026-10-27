/* A text browser behind the one command line (cmd.c). `browse URL` fetches the page (https through tls.c, http
   through the shared stack), follows up to 3 redirects, turns the HTML into readable text and prints it in pages of
   PAGE_LINES lines; `more`, `up`, `down`, `top`, `bottom` move through it, `find WORD` jumps to a line, `links` lists
   the numbered links and `open N` follows one. The last 8 pages are history for `back` and `forward`; `reload` fetches
   the page again. Every print ends with one status line: the URL and `line X of Y`. Every line fits the 53-column
   rule through ask.c's say_wrapped; everything goes out through cmd_out, so a Terminal app can own the output later.
   One page at a time: the raw reply up to 4 MiB on the heap (6.12.0, was a 64 KiB cap), chunked transfer undone and a
   gzip body inflated, the text kept to 8 KiB. */
#include "../../drivers/net.h"
#include "../../drivers/http.h"
#include "tls.h"
#include "../../drivers/inflate.h"
#include "../../drivers/png.h"
#include "../../drivers/jpeg.h"

extern void (*cmd_out)(const char *s);         /* cmd.c */
void cmd_dec(unsigned v);
#define kputs cmd_out
#define kdec cmd_dec
void say_wrapped(const char *prefix, const char *s, unsigned n);   /* ask.c */
unsigned long heap_mark(void);
void heap_release(unsigned long m);
void *kmalloc(unsigned int n);

#define URL_MAX 256
#define LINK_MAX 32
#define RAW_MAX (4u << 20)  /* the reply as it came, headers and all */
#define PAGE_MAX (8u << 20)  /* the most a gzip body may inflate to (its trailer names the size): with RAW_MAX, 12 of the 16 MiB heap */
#define TEXT_MAX 8192       /* the readable text: half the Terminal's log, so a page never pushes itself out */
#define HIST_MAX 8
/* 6.13.0: style marks in the page text, for the Blackcomb window (browser_ui.h); the Terminal prints without them. A line
   that starts with BR_BIG is an h1 or h2, BR_SMALL an h3 to h6; BR_BOLD and BR_PLAIN turn bold on and off inside a line. */
#define BR_BIG '\x01'
#define BR_SMALL '\x02'
#define BR_BOLD '\x03'
#define BR_PLAIN '\x04'
/* 6.14.0: an inline image is a line of its own, BR_IMG then 0x10 + its number. The window draws the picture there. */
#define BR_IMG '\x05'
#define IMG_MAX 4
#define IMG_W 400           /* logical px: an image is kept at most this wide and tall, scaled down to fit, never up */
#define IMG_H 300
static char img_url[IMG_MAX][URL_MAX];
static unsigned nimg, img_w[IMG_MAX], img_h[IMG_MAX];   /* images on the page, and each one's kept size (0 when it did not load) */
static unsigned img_px[IMG_MAX][IMG_W * IMG_H];
/* 6.15.0: forms and cookies. A text field or a submit button is a numbered link like any other, its "URL" field:K or
   submit:F; `fill N TEXT` sets a field and opening a submit link sends its form, GET as a query or POST as a body. */
#define FORM_MAX 4
#define FIELD_MAX 16
static struct { char action[URL_MAX]; int post; } forms[FORM_MAX];
static struct { unsigned form; char name[32], value[64]; unsigned char kind; } fields[FIELD_MAX];   /* kind 0 text, 1 password, 2 hidden */
static unsigned nforms, nfields;
static const char *post_body; static unsigned post_len;   /* the next fetch POSTs this, on its first hop only */
/* The cookie jar: name=value per host, sent back to that host alone. A Secure cookie is taken and sent over https only.
   ponytail: no Domain or Path matching and no expiry beyond Max-Age=0; it lives until the Pi restarts. */
#define COOKIE_MAX 24
static struct { char host[128], name[32], value[192]; int secure; } jar[COOKIE_MAX];
static unsigned njar;         /* 0RGB, img_w[k] to a row. ponytail: static, 1.9 MB; the heap is a bump that callers roll back */
#define PAGE_LINES 16       /* ponytail: a fixed page; the Terminal's own rows if its height should set it */
/* Budgets in ticks (100 a second). A fetch answers or fails in about 10 s: 3 s for DNS, 4 s for the SYN-ACK, 8 s for
   each piece of the reply (tls.c's own read budget matches). The stack's defaults are 20 s each, which on a real LAN
   with a dead hop reads as "stuck on fetching". */
#define DNS_TICKS 300
#define CONNECT_TICKS 400
#define HTTP_TICKS 800

static char cur[URL_MAX];                 /* the page on screen, the base for relative links */
static char links[LINK_MAX][URL_MAX];
static unsigned nlinks;
static char text[TEXT_MAX], title[64];    /* the page as lines of text */
static unsigned tlen, nline, view;        /* text length, its line count, the first line of the view */
static char hist[HIST_MAX][URL_MAX];
static unsigned hn, hi;                   /* pages in history, and which one is on screen */
/* 6.16.0: tabs, up to TAB_MAX. Each keeps its own history; a switch fetches that tab's page again, since one page is held
   in memory at a time. A new tab starts empty, which Blackcomb shows as its start page. */
#define TAB_MAX 4
static struct { char hist[HIST_MAX][URL_MAX]; unsigned hn, hi; } tabs[TAB_MAX];
static unsigned ntabs = 1, tab_cur;
static unsigned loads;                    /* pages that arrived so far: the Browser window sees a change and re-reads the page */

static unsigned slen(const char *s) { unsigned n = 0; while (s[n]) n++; return n; }
static int starts(const char *s, const char *w) { for (; *w; s++, w++) if ((*s | 32) != (*w | 32)) return 0; return 1; }
static void scopy(char *d, const char *s, unsigned n, unsigned max) { if (n >= max) n = max - 1; for (unsigned i = 0; i < n; i++) d[i] = s[i]; d[n] = 0; }

/* scheme://host[:port]/path -> parts. 1 on a URL this browser can fetch. */
struct url { int tls; char host[128]; unsigned short port; const char *path; unsigned path_len; };
static int parse_url(const char *u, struct url *o) {
    if (starts(u, "https://")) { o->tls = 1; u += 8; }
    else if (starts(u, "http://")) { o->tls = 0; u += 7; }
    else return 0;
    unsigned h = 0;
    while (u[h] && u[h] != ':' && u[h] != '/' && u[h] != '?') h++;
    if (!h || h >= sizeof o->host) return 0;
    scopy(o->host, u, h, sizeof o->host);
    u += h; o->port = o->tls ? 443 : 80;
    if (*u == ':') { unsigned p = 0; u++; while (*u >= '0' && *u <= '9') p = p * 10 + (unsigned)(*u++ - '0'); if (!p || p > 65535) return 0; o->port = (unsigned short)p; }
    o->path = *u ? u : "/"; o->path_len = slen(o->path);
    return 1;
}

/* A link target against the current page: absolute, root-relative (/x), or relative (x); "" when it is not a page. */
static void resolve(const char *href, unsigned n, char *out) {
    out[0] = 0;
    if (!n || href[0] == '#' || starts(href, "mailto:") || starts(href, "javascript:")) return;
    if (starts(href, "http://") || starts(href, "https://")) { scopy(out, href, n, URL_MAX); return; }
    struct url b; if (!parse_url(cur, &b)) return;
    unsigned o = 0;
    const char *pre = b.tls ? "https://" : "http://";
    if (n > 2 && href[0] == '/' && href[1] == '/') {   /* //host/path keeps the page's scheme */
        for (const char *p = pre; p[0] != '/' && o < URL_MAX - 1; p++) out[o++] = *p;
        for (unsigned i = 0; i < n && o < URL_MAX - 1; i++) out[o++] = href[i];
        out[o] = 0; return;
    }
    for (const char *p = pre; *p; p++) out[o++] = *p;
    for (const char *p = b.host; *p; p++) out[o++] = *p;
    if (b.port != (b.tls ? 443 : 80)) { out[o++] = ':'; char t[6]; unsigned k = 0, v = b.port; do t[k++] = (char)('0' + v % 10); while (v /= 10); while (k) out[o++] = t[--k]; }
    if (href[0] == '/') { /* root relative */ }
    else {   /* relative: the directory of the current path */
        unsigned e = b.path_len; while (e && b.path[e - 1] != '/') e--;
        for (unsigned i = 0; i < e && o < URL_MAX - 2; i++) out[o++] = b.path[i];
        if (!e) out[o++] = '/';
    }
    for (unsigned i = 0; i < n && o < URL_MAX - 1; i++) out[o++] = href[i];
    out[o] = 0;
}

/* GET url with up to 3 redirects. Returns the raw reply length in raw (headers included) and leaves cur at the final
   URL, or -1 after printing why. */
/* 6.9.0: a YouTube address names a video (watch?v=, youtu.be/, shorts/): its 11 character id into vid, 1; else 0. */
int yt_page(const char *vid, char *out, unsigned max, int *status);   /* ask.c */
static int yt_id(const char *u, char vid[12]) {
    if (starts(u, "https://")) u += 8; else if (starts(u, "http://")) u += 7; else return 0;
    if (starts(u, "www.")) u += 4; else if (starts(u, "m.")) u += 2;
    const char *p = 0;
    if (starts(u, "youtu.be/")) p = u + 9;
    else if (starts(u, "youtube.com/shorts/")) p = u + 19;
    else if (starts(u, "youtube.com/watch?")) {
        for (const char *q = u + 18; *q; q++) if ((q[-1] == '?' || q[-1] == '&') && q[0] == 'v' && q[1] == '=') { p = q + 2; break; }
    }
    if (!p) return 0;
    for (int i = 0; i < 11; i++) {
        char c = p[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_')) return 0;
        vid[i] = c;
    }
    vid[11] = 0;
    return !((p[11] >= 'a' && p[11] <= 'z') || (p[11] >= 'A' && p[11] <= 'Z') || (p[11] >= '0' && p[11] <= '9') || p[11] == '-' || p[11] == '_');
}
/* 6.10.0: Play. The page's "Play in Movies" link (&play=1) asks the video service for the clip made to fit what the heap
   can hold, saves it as YT.AVI where an app's files live (the signed-in person's home folder, else JT/), leaves
   PLAYNOW.TXT naming it, and starts Movies, which plays it. Everything here blocks, like a page load. */
int yt_avi(const char *vid, unsigned cap, char *out, int *status);   /* ask.c */
int home_active(void); int home_save(const char *name, const void *data, unsigned len);   /* home.c */
int sd_write(const char *name, const void *data, unsigned len);   /* emmc.c */
int app_dock(const char *name);   /* appwin.c */
static int has(const char *s, const char *w) { for (; *s; s++) if (starts(s, w)) return 1; return 0; }
static int save_here(const char *name, const void *d, unsigned n) { return home_active() ? home_save(name, d, n) == 1 : sd_write(name, d, n) == 1; }
static void yt_play(const char *vid) {
    static const unsigned try[] = { 12u << 20, 10u << 20, 8u << 20, 6u << 20 };
    char *buf = 0; unsigned cap = 0;
    for (unsigned i = 0; i < 4 && !buf; i++) { cap = try[i]; buf = kmalloc(cap + 1); }
    if (!buf) { kputs("browser: out of memory for the video\n"); return; }
    kputs("browser: making the video for Movies\n");
    int status, got = yt_avi(vid, cap, buf, &status);
    if (got <= 0 || status != 200) { kputs("browser: video service "); kputs(got < 0 ? "not reachable\n" : "refused\n"); return; }
    if (!save_here("YT.AVI", buf, (unsigned)got)) { kputs("browser: could not save the video\n"); return; }
    kputs("browser: saved YT.AVI, "); kdec((unsigned)got); kputs(" bytes\n");
    save_here("PLAYNOW.TXT", "YT.AVI\n", 7);
    app_dock("movies");
}
static void take_cookies(const char *raw, unsigned got, const struct url *u) {   /* Set-Cookie lines of one reply into the jar */
    int hb = http_body_start(raw, got); unsigned h = hb < 0 ? got : (unsigned)hb;
    for (unsigned i = 0; i + 11 < h; i++) {
        if ((i && raw[i - 1] != '\n') || !starts(raw + i, "set-cookie:")) continue;
        unsigned a = i + 11; while (a < h && raw[a] == ' ') a++;
        unsigned eq = a; while (eq < h && raw[eq] != '=' && raw[eq] != ';' && raw[eq] != '\r') eq++;
        if (eq >= h || raw[eq] != '=' || eq == a || eq - a >= sizeof jar[0].name) continue;
        unsigned v = eq + 1, ve = v; while (ve < h && raw[ve] != ';' && raw[ve] != '\r' && raw[ve] != '\n') ve++;
        unsigned le = ve; while (le < h && raw[le] != '\r' && raw[le] != '\n') le++;
        int secure = 0, gone = 0;
        for (unsigned k = ve; k < le; k++) if (raw[k] == ';') {
            unsigned t = k + 1; while (t < le && raw[t] == ' ') t++;
            if (starts(raw + t, "secure") && (t + 6 == le || raw[t + 6] == ';' || raw[t + 6] == ' ')) secure = 1;
            if (starts(raw + t, "max-age=0") || starts(raw + t, "max-age=-")) gone = 1;
        }
        if (secure && !u->tls) continue;   /* a page over plain http may not set a Secure cookie */
        unsigned c = 0;
        for (; c < njar; c++) {   /* the same host and name: replaced */
            unsigned m = 0; while (jar[c].host[m] && jar[c].host[m] == u->host[m]) m++;
            unsigned q = 0; while (a + q < eq && jar[c].name[q] == raw[a + q]) q++;
            if (!jar[c].host[m] && !u->host[m] && a + q == eq && !jar[c].name[q]) break;
        }
        if (gone) { if (c < njar) jar[c] = jar[--njar]; continue; }
        if (c == njar) { if (njar == COOKIE_MAX) continue; njar++; }
        scopy(jar[c].host, u->host, slen(u->host), sizeof jar[c].host);
        scopy(jar[c].name, raw + a, eq - a, sizeof jar[c].name);
        scopy(jar[c].value, raw + v, ve - v, sizeof jar[c].value);
        jar[c].secure = secure;
        kputs("browser: cookie "); kputs(jar[c].name); kputs(secure ? " (secure)\n" : "\n");
    }
}
static int fetch_once(const char *url, char *raw, unsigned max);
static int fetch(const char *url, char *raw, unsigned max) { int r = fetch_once(url, raw, max); post_body = 0; post_len = 0; return r; }
static int fetch_once(const char *url, char *raw, unsigned max) {
    char next[URL_MAX], vid[12];
    scopy(cur, url, slen(url), URL_MAX);
    if (yt_id(url, vid)) {   /* the page about the video, from the Mac's video service, behind a header show() can skip */
        static const char head[] = "HTTP/1.0 200 OK\r\n\r\n";
        unsigned h = sizeof head - 1; int status;
        kputs("browser: youtube "); kputs(vid); kputs("\n");
        int got = yt_page(vid, raw + h, max - h, &status);
        if (got <= 0 || status != 200) { kputs("browser: video service "); kputs(got < 0 ? "not reachable\n" : "refused\n"); return -1; }
        for (unsigned i = 0; i < h; i++) raw[i] = head[i];
        raw[h + (unsigned)got] = 0;
        kputs("browser: http 200\n");
        return (int)h + got;
    }
    for (int hop = 0; hop <= 3; hop++) {
        struct url u;
        if (!parse_url(cur, &u)) { kputs("browser: bad url\n"); return -1; }
        static char req[URL_MAX + 3072]; unsigned n = 0;
        const char *body = hop == 0 ? post_body : 0; unsigned blen = body ? post_len : 0;
        #define RQ(str) for (const char *p_ = (str); *p_ && n < sizeof req - 1; p_++) req[n++] = *p_
        RQ(body ? "POST " : "GET ");
        for (unsigned i = 0; i < u.path_len; i++) req[n++] = u.path[i];
        RQ(" HTTP/1.1\r\nHost: "); RQ(u.host);
        RQ("\r\nUser-Agent: JoshuaTree/1.0\r\nAccept-Encoding: gzip\r\nConnection: close\r\n");
        for (unsigned c = 0, first = 1; c < njar; c++) {
            unsigned m = 0; while (jar[c].host[m] && jar[c].host[m] == u.host[m]) m++;
            if (jar[c].host[m] || u.host[m] || (jar[c].secure && !u.tls)) continue;
            RQ(first ? "Cookie: " : "; "); RQ(jar[c].name); RQ("="); RQ(jar[c].value); first = 0;
        }
        if (req[n - 1] != '\n') RQ("\r\n");   /* a Cookie line went in */
        if (body) {
            char d[8]; unsigned k = 0, v = blen; do d[k++] = (char)('0' + v % 10); while (v /= 10);
            RQ("Content-Type: application/x-www-form-urlencoded\r\nContent-Length: ");
            while (k) req[n++] = d[--k];
            RQ("\r\n");
        }
        RQ("\r\n");
        for (unsigned i = 0; i < blen && n < sizeof req - 1; i++) req[n++] = body[i];
        #undef RQ
        /* Each stage prints one line when it passes, so a photo of the screen says where a failed fetch stopped. */
        unsigned ip; int got;
        if (!http_resolve_host(u.host, &ip)) { kputs(net_last_error() == NET_ERR_DNS_TIMEOUT ? "browser: dns timeout\n" : "browser: no such host\n"); return -1; }
        kputs("browser: dns ok\n");
        if (u.tls) {
            if (tls_connect(ip, u.port, u.host) < 0) { kputs(tls_last_error() == TLS_ERR_ENTROPY ? "browser: hardware randomness unavailable\n" : "browser: connect timeout\n"); return -1; }
            kputs("browser: tcp ok\n");
            if (tls_handshake() < 0) {
                int e = tls_last_error();
                kputs("browser: ");
                if (e == TLS_ERR_TIMEOUT) kputs("tls timeout\n");
                else if (e == TLS_ERR_CONNECT) kputs("tls connection closed\n");
                else { kputs("tls error "); kdec((unsigned)e); kputs(e == 62 ? " (not trusted)\n" : e == 56 ? " (wrong host name)\n" : "\n"); }
                return -1;
            }
            kputs("browser: tls ok\n");
            got = tls_exchange(req, n, raw, max - 1);
            if (got < 0 && tls_last_error() > 0) { kputs("browser: tls error "); kdec((unsigned)tls_last_error()); kputs("\n"); return -1; }
        } else got = tcp_get_timeout(ip, u.port, req, n, raw, max - 1, HTTP_TICKS);
        if (got <= 0) {
            int e = net_last_error();
            kputs(e == NET_ERR_CONNECT_TIMEOUT ? "browser: connect timeout\n" : e == NET_ERR_REPLY_TIMEOUT ? "browser: reply timeout\n" : got == 0 ? "browser: empty reply\n" : "browser: no reply\n");
            return -1;
        }
        if (!u.tls) kputs("browser: tcp ok\n");
        raw[got] = 0;
        if ((unsigned)got >= max - 1) kputs("browser: page too big, showing the first 4 MiB\n");
        take_cookies(raw, (unsigned)got, &u);
        int status = http_status_of(raw, (unsigned)got);
        kputs("browser: http "); kdec((unsigned)status); kputs(status >= 400 ? " (the server refused)\n" : status < 200 ? " (not a page)\n" : "\n");
        if (status >= 300 && status < 400) {
            int b = http_body_start(raw, (unsigned)got); if (b < 0) b = got;
            unsigned i = 0, found = 0;
            while ((int)i < b) {   /* the Location header, at a line start */
                if ((i == 0 || raw[i - 1] == '\n') && starts(raw + i, "location:")) {
                    i += 9; while (raw[i] == ' ') i++;
                    unsigned e = i; while ((int)e < b && raw[e] != '\r' && raw[e] != '\n') e++;
                    resolve(raw + i, e - i, next); found = next[0] != 0; break;
                }
                i++;
            }
            if (!found) { kputs("browser: redirect without a location\n"); return -1; }
            if (hop == 3) { kputs("browser: too many redirects\n"); return -1; }
            scopy(cur, next, slen(next), URL_MAX);
            continue;
        }
        return got;
    }
    return -1;
}

/* HTML to text. Tags go; block tags become line breaks, paragraphs and headings get a blank line before them; script
   and style bodies are skipped; the title is kept; common entities and &#NN; are decoded; runs of white space collapse
   to one space; a list item starts "- "; an <a href> ends in its link number, "text [3]". */
static unsigned put(char *t, unsigned o, char c) { if (o < TEXT_MAX - 1) t[o++] = c; return o; }
static unsigned entity(const char *s, char *c) {   /* bytes consumed after '&', 0 when it is not one we know */
    static const struct { const char *name; char ch; } ents[] = { {"amp;", '&'}, {"lt;", '<'}, {"gt;", '>'}, {"quot;", '"'}, {"apos;", '\''}, {"nbsp;", ' '} };
    for (unsigned i = 0; i < sizeof ents / sizeof ents[0]; i++) if (starts(s, ents[i].name)) { *c = ents[i].ch; return slen(ents[i].name); }
    if (s[0] == '#') {   /* &#NN; or &#xHH;: ASCII comes through, anything wider prints as '?' */
        unsigned v = 0, i = 1, hex = (s[1] | 32) == 'x'; if (hex) i++;
        unsigned d = i;
        for (; i < d + 7; i++) {
            char h = s[i];
            if (h >= '0' && h <= '9') v = v * (hex ? 16 : 10) + (unsigned)(h - '0');
            else if (hex && (h | 32) >= 'a' && (h | 32) <= 'f') v = v * 16 + (unsigned)((h | 32) - 'a' + 10);
            else break;
        }
        if (i == d || s[i] != ';') return 0;
        *c = v >= 32 && v < 127 ? (char)v : v == 160 ? ' ' : '?';
        return i + 1;
    }
    return 0;
}
static int is_tag(const char *tag, unsigned tn, const char *w) {   /* the tag name alone: "p" is not "pre" */
    unsigned k = slen(w);
    return tn >= k && starts(tag, w) && (tn == k || tag[k] == ' ' || tag[k] == '\n' || tag[k] == '\t' || tag[k] == '/');
}
static int attr(const char *tag, unsigned tn, const char *name, unsigned *s, unsigned *z) {   /* name= in a tag: value at tag[*s..*z) */
    unsigned k0 = slen(name);
    for (unsigned k = 1; k + k0 <= tn; k++) if (starts(tag + k, name) && (tag[k - 1] == ' ' || tag[k - 1] == '\n' || tag[k - 1] == '\t')) {
        unsigned a = k + k0, b; char q = a < tn ? tag[a] : 0;
        if (q == '"' || q == '\'') { a++; b = a; while (b < tn && tag[b] != q) b++; }
        else { b = a; while (b < tn && tag[b] != ' ' && tag[b] != '\n' && tag[b] != '\t') b++; if (b == tn && b > a && tag[b - 1] == '/') b--; }
        *s = a; *z = b; return 1;
    }
    return 0;
}
static int is_val(const char *tag, unsigned s, unsigned z, const char *w) { return z - s == slen(w) && starts(tag + s, w); }
static unsigned mark(unsigned o, int link) {   /* " [N]" after a link's text */
    o = put(text, o, ' '); o = put(text, o, '['); char d[4]; unsigned k = 0, v = (unsigned)link; do d[k++] = (char)('0' + v % 10); while (v /= 10); while (k) o = put(text, o, d[--k]);
    return put(text, o, ']');
}
static int pseudo_link(const char *kind, unsigned k) {   /* field:K or submit:F as the next link; its number, or 0 when the list is full */
    if (nlinks >= LINK_MAX) return 0;
    unsigned o = 0; for (const char *p = kind; *p; p++) links[nlinks][o++] = *p;
    links[nlinks][o++] = (char)('0' + k); links[nlinks][o] = 0;
    return (int)++nlinks;
}
/* 6.25.0: what the page hides stays hidden. A pass over every <style> block collects the simple selectors (tag, .class,
   #id, tag.class, tag#id) whose rules say display:none; @media and other @-blocks, and selectors with spaces, combinators
   or pseudo-classes, are left out. An element with one of them, with the hidden attribute, or with an inline
   display:none is skipped whole, nested elements and all: the menus, drawers and cookie banners a page keeps out of view. */
#define HIDE_MAX 32
static char hide_sel[HIDE_MAX][32]; static unsigned nhide;
static int is_ws(char c) { return c == ' ' || c == '\n' || c == '\t' || c == '\r'; }
static int says_none(const char *d, unsigned n) {   /* "display" ":" "none" in a declaration block */
    for (unsigned i = 0; i + 7 <= n; i++) if (starts(d + i, "display")) {
        unsigned k = i + 7; while (k < n && is_ws(d[k])) k++;
        if (k < n && d[k] == ':') { k++; while (k < n && is_ws(d[k])) k++; if (k + 4 <= n && starts(d + k, "none")) return 1; }
    }
    return 0;
}
static void css_rules(const char *c, unsigned n) {
    for (unsigned i = 0; i < n;) {
        while (i < n && is_ws(c[i])) i++;
        if (i + 1 < n && c[i] == '/' && c[i + 1] == '*') { i += 2; while (i + 1 < n && !(c[i] == '*' && c[i + 1] == '/')) i++; i += 2; continue; }
        unsigned sel = i; while (i < n && c[i] != '{') i++;
        if (i >= n) return;
        unsigned sel_end = i++, depth = 1, body = i;
        while (i < n && depth) { if (c[i] == '{') depth++; else if (c[i] == '}') depth--; i++; }
        if (c[sel] == '@' || !says_none(c + body, i - 1 - body)) continue;   /* @media and friends, or a rule that hides nothing */
        for (unsigned a = sel; a < sel_end;) {   /* each selector of a comma list */
            while (a < sel_end && is_ws(c[a])) a++;
            unsigned b = a; while (b < sel_end && c[b] != ',') b++;
            unsigned e = b; while (e > a && is_ws(c[e - 1])) e--;
            int simple = e > a && e - a < 32;
            for (unsigned k = a; k < e && simple; k++) if (is_ws(c[k]) || c[k] == '>' || c[k] == '+' || c[k] == '~' || c[k] == '[' || c[k] == ':' || c[k] == '*') simple = 0;
            if (simple && nhide < HIDE_MAX) { for (unsigned k = a; k < e; k++) hide_sel[nhide][k - a] = c[k]; hide_sel[nhide++][e - a] = 0; }
            a = b + 1;
        }
    }
}
static int word_in(const char *list, unsigned n, const char *w, unsigned wn) {   /* w is one of the space-separated words */
    for (unsigned i = 0; i + wn <= n; i++) {
        if (i && !is_ws(list[i - 1])) continue;
        unsigned k = 0; while (k < wn && list[i + k] == w[k]) k++;
        if (k == wn && (i + wn == n || is_ws(list[i + wn]))) return 1;
    }
    return 0;
}
static int hidden_tag(const char *tag, unsigned tn, unsigned nl) {   /* tag[0..nl) is its name */
    unsigned s, z, cs = 0, cz = 0, is = 0, iz = 0;
    for (unsigned k = nl; k + 6 <= tn; k++)   /* the bare hidden attribute */
        if (is_ws(tag[k - 1]) && starts(tag + k, "hidden") && (k + 6 == tn || is_ws(tag[k + 6]) || tag[k + 6] == '=' || tag[k + 6] == '/')) return 1;
    if (attr(tag, tn, "style=", &s, &z) && says_none(tag + s, z - s)) return 1;
    int has_c = attr(tag, tn, "class=", &cs, &cz), has_i = attr(tag, tn, "id=", &is, &iz);
    for (unsigned r = 0; r < nhide; r++) {
        const char *sel = hide_sel[r]; unsigned sl = slen(sel), t = 0;
        while (t < sl && sel[t] != '.' && sel[t] != '#') t++;   /* the tag part */
        if (t) { if (t != nl) continue; unsigned m = 0; while (m < t && (tag[m] | 32) == (sel[m] | 32)) m++; if (m < t) continue; }
        if (t == sl) return 1;   /* a bare tag name */
        const char *w = sel + t + 1; unsigned wn = sl - t - 1;
        if (sel[t] == '.' && has_c && word_in(tag + cs, cz - cs, w, wn)) return 1;
        if (sel[t] == '#' && has_i && iz - is == wn && starts(tag + is, w)) return 1;
    }
    return 0;
}

static void to_text(const char *h, unsigned n) {
    unsigned o = 0, i = 0, skip = 0, space = 1;
    char htag[16]; unsigned hn = 0, hdepth = 0;   /* inside a hidden element: its name, and how deep its own tag is nested */
    nhide = 0;
    for (unsigned k = 0; k + 7 < n; k++) if (h[k] == '<' && starts(h + k + 1, "style")) {   /* every <style>'s rules, first */
        unsigned a = k + 6; while (a < n && h[a] != '>') a++;
        unsigned b = ++a; while (b + 7 < n && !(h[b] == '<' && h[b + 1] == '/' && starts(h + b + 2, "style"))) b++;
        css_rules(h + a, b - a); k = b;
    }
    int in_title = 0, link = 0; unsigned tl = 0;
    nlinks = 0; nimg = 0; nforms = nfields = 0; title[0] = 0;
    int in_form = -1;
    while (i < n) {
        char c = h[i];
        if (skip) {   /* inside script or style: only the literal closing tag ends it; a '<' in code is not a tag */
            if (c == '<' && h[i + 1] == '/' && starts(h + i + 2, skip == 1 ? "script" : "style")) { skip = 0; while (i < n && h[i] != '>') i++; }
            i++; continue;
        }
        if (c == '<') {
            unsigned e = i + 1; while (e < n && h[e] != '>') e++;
            const char *tag = h + i + 1; unsigned tn = e - i - 1;
            if (tn >= 3 && tag[0] == '!' && tag[1] == '-' && tag[2] == '-') {   /* a comment: to --> */
                e = i + 4; while (e + 2 < n && !(h[e] == '-' && h[e + 1] == '-' && h[e + 2] == '>')) e++;
                i = e + 3; continue;
            }
            int close = tag[0] == '/'; if (close) { tag++; tn--; }
            unsigned nl = 0; while (nl < tn && !is_ws(tag[nl]) && tag[nl] != '/' && tag[nl] != '>') nl++;
            if (hdepth) {   /* inside a hidden element: only its own tag's nesting counts */
                if (nl == hn) { unsigned m = 0; while (m < nl && (tag[m] | 32) == htag[m]) m++; if (m == nl) hdepth += close ? -1u : 1u; }
                if (!close && (is_tag(tag, tn, "script") || is_tag(tag, tn, "style"))) skip = is_tag(tag, tn, "script") ? 1 : 2;
                i = e + 1; continue;
            }
            if (!close && nl && nl < sizeof htag && tag[0] != '!' && hidden_tag(tag, tn, nl)) {
                static const char *const lone[] = { "img", "input", "br", "hr", "meta", "link", "source", "wbr" };   /* no closing tag to wait for */
                int is_lone = tag[tn - 1] == '/';
                for (unsigned v = 0; v < sizeof lone / sizeof lone[0] && !is_lone; v++) if (is_tag(tag, tn, lone[v])) is_lone = 1;
                if (!is_lone) { for (unsigned m = 0; m < nl; m++) htag[m] = (char)(tag[m] | 32); hn = nl; hdepth = 1; }
                i = e + 1; continue;
            }
            if (!close && (is_tag(tag, tn, "a") || is_tag(tag, tn, "span") || is_tag(tag, tn, "button")) && o && text[o - 1] != ' ' && text[o - 1] != '\n') { o = put(text, o, ' '); space = 1; }   /* nav bars space their links with CSS we do not read */
            if (!close && (is_tag(tag, tn, "script") || is_tag(tag, tn, "style"))) skip = is_tag(tag, tn, "script") ? 1 : 2;
            else {
                int heading = tn >= 2 && (tag[0] | 32) == 'h' && tag[1] >= '1' && tag[1] <= '6' && (tn == 2 || tag[2] == ' ' || tag[2] == '\n');
                if (is_tag(tag, tn, "title")) in_title = !close;
                else if (is_tag(tag, tn, "a")) {
                    if (!close) {
                        link = 0;
                        for (unsigned k = 1; k + 5 < tn; k++) if (starts(tag + k, "href=") && (tag[k - 1] == ' ' || tag[k - 1] == '\n' || tag[k - 1] == '\t')) {
                            unsigned s = k + 5; char q = tag[s]; unsigned z;
                            if (q == '"' || q == '\'') { s++; z = s; while (z < tn && tag[z] != q) z++; } else { z = s; while (z < tn && tag[z] != ' ' && tag[z] != '>') z++; }
                            if (nlinks < LINK_MAX) { resolve(tag + s, z - s, links[nlinks]); if (links[nlinks][0]) link = (int)++nlinks; }
                            break;
                        }
                    } else if (link) {   /* "text [3]" */
                        o = put(text, o, ' '); o = put(text, o, '['); char d[4]; unsigned k = 0, v = (unsigned)link; do d[k++] = (char)('0' + v % 10); while (v /= 10); while (k) o = put(text, o, d[--k]);
                        o = put(text, o, ']'); link = 0; space = 0;
                    }
                }
                else if (is_tag(tag, tn, "form")) {
                    if (close) in_form = -1;
                    else if (nforms < FORM_MAX) {
                        unsigned s, z;
                        if (attr(tag, tn, "action=", &s, &z) && z > s) resolve(tag + s, z - s, forms[nforms].action); else scopy(forms[nforms].action, cur, slen(cur), URL_MAX);
                        forms[nforms].post = attr(tag, tn, "method=", &s, &z) && is_val(tag, s, z, "post");
                        in_form = (int)nforms++;
                    }
                }
                else if (!close && in_form >= 0 && is_tag(tag, tn, "input")) {
                    unsigned s, z, ts = 0, tz = 0;
                    int typed = attr(tag, tn, "type=", &ts, &tz);
                    int submit = typed && (is_val(tag, ts, tz, "submit") || is_val(tag, ts, tz, "image"));
                    int hidden = typed && is_val(tag, ts, tz, "hidden"), pw = typed && is_val(tag, ts, tz, "password");
                    int texty = !typed || pw || is_val(tag, ts, tz, "text") || is_val(tag, ts, tz, "search") || is_val(tag, ts, tz, "email") || is_val(tag, ts, tz, "url") || is_val(tag, ts, tz, "tel");
                    if (submit) {
                        int l = pseudo_link("submit:", (unsigned)in_form);
                        if (o && text[o - 1] != ' ' && text[o - 1] != '\n') o = put(text, o, ' ');
                        if (attr(tag, tn, "value=", &s, &z) && z > s) for (unsigned i = s; i < z; i++) o = put(text, o, tag[i]);
                        else for (const char *p = "Submit"; *p; p++) o = put(text, o, *p);
                        if (l) o = mark(o, l);
                        space = 0;
                    } else if ((hidden || texty) && nfields < FIELD_MAX && attr(tag, tn, "name=", &s, &z) && z > s) {
                        unsigned k = nfields++;
                        fields[k].form = (unsigned)in_form; fields[k].kind = (unsigned char)(hidden ? 2 : pw ? 1 : 0);
                        scopy(fields[k].name, tag + s, z - s, sizeof fields[k].name);
                        fields[k].value[0] = 0;
                        if (attr(tag, tn, "value=", &s, &z)) scopy(fields[k].value, tag + s, z - s, sizeof fields[k].value);
                        if (!hidden) {   /* "name: value [N]", a password as stars */
                            int l = pseudo_link("field:", k);
                            if (o && text[o - 1] != ' ' && text[o - 1] != '\n') o = put(text, o, ' ');
                            for (const char *p = fields[k].name; *p; p++) o = put(text, o, *p);
                            o = put(text, o, ':'); o = put(text, o, ' ');
                            if (!fields[k].value[0]) for (int i = 0; i < 8; i++) o = put(text, o, '_');
                            for (const char *p = fields[k].value; *p; p++) o = put(text, o, pw ? '*' : *p);
                            if (l) o = mark(o, l);
                            space = 0;
                        }
                    }
                }
                else if (in_form >= 0 && is_tag(tag, tn, "button")) {   /* a submit button unless type=button or reset; its text, then [N] */
                    unsigned s, z;
                    if (!close) link = attr(tag, tn, "type=", &s, &z) && (is_val(tag, s, z, "button") || is_val(tag, s, z, "reset")) ? 0 : pseudo_link("submit:", (unsigned)in_form);
                    else if (link) { o = mark(o, link); link = 0; space = 0; }
                }
                else if (!close && is_tag(tag, tn, "img")) {   /* src="..." on a line of its own */
                    for (unsigned k = 1; k + 4 < tn && nimg < IMG_MAX; k++) if (starts(tag + k, "src=") && (tag[k - 1] == ' ' || tag[k - 1] == '\n' || tag[k - 1] == '\t')) {
                        unsigned s = k + 4; char q = tag[s]; unsigned z;
                        if (q == '"' || q == '\'') { s++; z = s; while (z < tn && tag[z] != q) z++; } else { z = s; while (z < tn && tag[z] != ' ' && tag[z] != '\n' && tag[z] != '\t') z++; if (z == tn && z > s && tag[z - 1] == '/') z--; }   /* <img src=x/> */
                        resolve(tag + s, z - s, img_url[nimg]);
                        if (img_url[nimg][0]) {
                            if (o && text[o - 1] != '\n') o = put(text, o, '\n');
                            o = put(text, o, BR_IMG); o = put(text, o, (char)(0x10 + nimg++)); o = put(text, o, '\n'); space = 1;
                        }
                        break;
                    }
                }
                else if (is_tag(tag, tn, "b") || is_tag(tag, tn, "strong")) { if (!in_title) o = put(text, o, close ? BR_PLAIN : BR_BOLD); }
                else if (heading || is_tag(tag, tn, "p") || is_tag(tag, tn, "br") || is_tag(tag, tn, "div") || is_tag(tag, tn, "li") || is_tag(tag, tn, "tr")
                         || is_tag(tag, tn, "ul") || is_tag(tag, tn, "ol") || is_tag(tag, tn, "table") || is_tag(tag, tn, "blockquote") || is_tag(tag, tn, "pre") || is_tag(tag, tn, "hr")) {
                    if (o && text[o - 1] != '\n') o = put(text, o, '\n');
                    if (!close && (heading || is_tag(tag, tn, "p")) && o && text[o - 1] == '\n' && !(o > 1 && text[o - 2] == '\n')) o = put(text, o, '\n');   /* a blank line before */
                    if (!close && is_tag(tag, tn, "li")) { o = put(text, o, '-'); o = put(text, o, ' '); }
                    if (!close && heading) o = put(text, o, tag[1] <= '2' ? BR_BIG : BR_SMALL);   /* 6.13.0: the window draws it as a heading */
                    space = 1;
                }
            }
            i = e + 1; continue;
        }
        if (hdepth) { i++; continue; }   /* a hidden element's own text */
        if (c == '&') { char d; unsigned k = entity(h + i + 1, &d); if (k) { c = d; i += k; } }
        i++;
        if ((unsigned char)c >= 0x80) {   /* 6.8.0: one UTF-8 character as one byte, its common punctuation in ASCII, anything else '?' */
            unsigned b = (unsigned char)c, cp, len = b >= 0xF0 ? 3 : b >= 0xE0 ? 2 : 1;
            if (b < 0xC0) continue;   /* a stray continuation byte */
            cp = b & (0x3Fu >> len);
            while (len-- && i < n && ((unsigned char)h[i] & 0xC0) == 0x80) cp = cp << 6 | ((unsigned char)h[i++] & 0x3F);
            c = cp == 0x2018 || cp == 0x2019 || cp == 0x2032 ? '\'' : cp == 0x201C || cp == 0x201D || cp == 0x2033 ? '"'
              : cp == 0x2010 || cp == 0x2011 || cp == 0x2013 || cp == 0x2014 || cp == 0x2212 ? '-' : cp == 0x2022 || cp == 0xB7 ? '*'
              : cp == 0x2026 ? '.' : cp == 0xA0 || cp == 0x2009 || cp == 0x202F ? ' ' : '?';   /* ponytail: one byte out, so an ellipsis is one dot */
        }
        int ws = c == ' ' || c == '\n' || c == '\r' || c == '\t';
        if (in_title) { if (ws) c = ' '; if (tl < 60 && !(ws && (!tl || title[tl - 1] == ' '))) { title[tl++] = c; title[tl] = 0; } continue; }
        if (ws) { if (!space) { o = put(text, o, ' '); space = 1; } continue; }
        o = put(text, o, (c >= 32 && c < 127) ? c : '?'); space = 0;
    }
    while (o && (text[o - 1] == ' ' || text[o - 1] == '\n')) o--;
    while (tl && title[tl - 1] == ' ') title[--tl] = 0;
    tlen = o; nline = 0;
    for (i = 0; i < o; i++) if (text[i] == '\n') nline++;
    if (o) nline++;
}

static unsigned line_start(unsigned k) {   /* the offset of line k */
    unsigned i = 0; for (; k && i < tlen; i++) if (text[i] == '\n') k--;
    return i;
}
/* Prints lines [from, from+count) and the status line, and leaves the view at from. */
static void show_lines(unsigned from, unsigned count) {
    if (!cur[0]) { kputs("browser: no page, browse URL first\n"); return; }
    if (from >= nline) from = nline ? nline - 1 : 0;
    view = from;
    unsigned i = line_start(from), k = from;
    for (; k < from + count && i <= tlen && k < nline; k++) {
        unsigned e = i; while (e < tlen && text[e] != '\n') e++;
        static char plain[TEXT_MAX]; unsigned m = 0;   /* the line without its style marks */
        for (unsigned j = i; j < e; j++) if ((unsigned char)text[j] >= 0x20) plain[m++] = text[j];
        if (!m) kputs("\n"); else say_wrapped("", plain, m);
        i = e + 1;
    }
    char st[URL_MAX + 40]; unsigned o = 0;
    for (const char *p = cur; *p; p++) st[o++] = *p;
    for (const char *p = "  line "; *p; p++) st[o++] = *p;
    unsigned v = from + 1; char d[10]; unsigned z = 0; do d[z++] = (char)('0' + v % 10); while (v /= 10); while (z) st[o++] = d[--z];
    for (const char *p = " of "; *p; p++) st[o++] = *p;
    v = nline; do d[z++] = (char)('0' + v % 10); while (v /= 10); while (z) st[o++] = d[--z];
    say_wrapped("browser: ", st, o);
}

/* 6.12.0: real pages. HTTP/1.1 replies may come chunked and gzipped (the request asks for gzip: most pages shrink four
   to ten times). Keep-alive waits: net.c holds one connection, so there is nothing to keep it alive for yet. */
static int header_has(const char *raw, unsigned h, const char *name, const char *val) {   /* name: ...val... in raw[0..h) */
    for (unsigned i = 0; i < h; i++) {
        if ((i && raw[i - 1] != '\n') || !starts(raw + i, name)) continue;
        for (unsigned e = i + slen(name); e < h && raw[e] != '\r' && raw[e] != '\n'; e++) if (starts(raw + e, val)) return 1;
    }
    return 0;
}
/* Transfer-Encoding: chunked, undone in place: the hex size lines go and the chunks close up. The new length, or -1 when
   a size line is not hex or a chunk runs past what arrived. */
static int dechunk(char *b, unsigned n) {
    unsigned r = 0, w = 0;
    for (;;) {
        unsigned size = 0, digits = 0;
        for (; r < n; r++, digits++) {
            char c = (char)(b[r] | 32);
            unsigned d = c >= '0' && c <= '9' ? (unsigned)(c - '0') : c >= 'a' && c <= 'f' ? (unsigned)(c - 'a' + 10) : 16;
            if (d == 16) break;
            if (size >> 27) return -1;
            size = size * 16 + d;
        }
        if (!digits) return -1;
        while (r < n && b[r] != '\n') r++;   /* chunk extensions, then the line end */
        if (r++ >= n) return -1;
        if (!size) return (int)w;             /* the last chunk; trailers are not read */
        if (size > n - r) return -1;
        for (unsigned i = 0; i < size; i++) b[w++] = b[r++];
        if (r < n && b[r] == '\r') r++;
        if (r < n && b[r] == '\n') r++;
    }
}
/* Content-Encoding: gzip (RFC 1952): past the header, the deflate stream inflates into a buffer the size the trailer
   names. The page and its length in *n, or 0 with the reason printed. ponytail: the CRC-32 is not checked; inflate_raw
   already refuses a stream that does not come out at exactly that size. */
static char *gunzip(const unsigned char *g, unsigned *n) {
    unsigned len = *n, p = 10, size;
    char *out;
    if (len < 18 || g[0] != 0x1f || g[1] != 0x8b || g[2] != 8) goto bad;
    if (g[3] & 4) p += 2 + (g[10] | (unsigned)g[11] << 8);                 /* FEXTRA */
    if (g[3] & 8) { while (p < len && g[p]) p++; p++; }                     /* FNAME */
    if (g[3] & 16) { while (p < len && g[p]) p++; p++; }                    /* FCOMMENT */
    if (g[3] & 2) p += 2;                                                   /* FHCRC */
    if (p + 8 > len) goto bad;
    size = g[len - 4] | (unsigned)g[len - 3] << 8 | (unsigned)g[len - 2] << 16 | (unsigned)g[len - 1] << 24;
    if (size > PAGE_MAX) { kputs("browser: page too big\n"); return 0; }
    if (!(out = kmalloc(size + 1))) { kputs("browser: out of memory\n"); return 0; }
    if (inflate_raw(g + p, len - 8 - p, (unsigned char *)out, size) != 0) goto bad;
    out[size] = 0; *n = size;
    return out;
bad:
    kputs("browser: gzip reply is damaged\n");
    return 0;
}
/* The page out of a raw reply: past the headers, chunks joined, gzip undone. 0 when it could not be read. */
static char *page_body(char *raw, unsigned got, unsigned *n) {
    int b = http_body_start(raw, got); if (b < 0) b = 0;
    char *body = raw + b; int len = (int)got - b;
    if (b && header_has(raw, (unsigned)b, "transfer-encoding:", "chunked") && (len = dechunk(body, (unsigned)len)) < 0) {
        kputs("browser: chunked reply cut short\n"); return 0;
    }
    body[len] = 0; *n = (unsigned)len;
    if (b && header_has(raw, (unsigned)b, "content-encoding:", "gzip")) body = gunzip((const unsigned char *)body, n);
    if (body) { kputs("browser: body "); kdec(*n); kputs(" bytes\n"); }
    return body;
}

/* 6.14.0: the page's images, after its text: each fetched like a page (quietly), decoded (PNG or baseline JPEG) and scaled
   to fit IMG_W x IMG_H. One line each on the log: "browser: image N WxH" or "browser: image N failed". */
static void sink(const char *s) { (void)s; }
static int jpeg_size(const unsigned char *d, unsigned n, unsigned *w, unsigned *h) {   /* the frame's size off its SOF marker */
    for (unsigned i = 2; i + 9 < n;) {
        if (d[i] != 0xFF) return 0;
        unsigned m = d[i + 1], len = (unsigned)d[i + 2] << 8 | d[i + 3];
        if (m >= 0xC0 && m <= 0xC3) { *h = (unsigned)d[i + 5] << 8 | d[i + 6]; *w = (unsigned)d[i + 7] << 8 | d[i + 8]; return *w && *h; }
        i += 2 + len;
    }
    return 0;
}
static void fit(unsigned sw, unsigned sh, unsigned *dw, unsigned *dh) {   /* scaled down to fit, aspect kept, never up */
    *dw = sw; *dh = sh;
    if (*dw > IMG_W) { *dh = *dh * IMG_W / *dw; *dw = IMG_W; }
    if (*dh > IMG_H) { *dw = *dw * IMG_H / *dh; *dh = IMG_H; }
    if (!*dw) *dw = 1;
    if (!*dh) *dh = 1;
}
static int decode_image(unsigned k, const unsigned char *d, unsigned n) {   /* 1 with img_px[k] filled */
    unsigned sw, sh, dw, dh, ch;
    if (n > 3 && d[0] == 0xFF && d[1] == 0xD8) {
        if (!jpeg_size(d, n, &sw, &sh)) return 0;
        fit(sw, sh, &dw, &dh);
        if (jpeg_decode_scaled32(d, n, img_px[k], dw, dh, &sw, &sh) != 0) return 0;
    } else {
        unsigned char *px;
        if (png_decode(d, n, &px, &sw, &sh, &ch) != 0) return 0;
        fit(sw, sh, &dw, &dh);
        for (unsigned y = 0; y < dh; y++) for (unsigned x = 0; x < dw; x++) {   /* nearest pixel; alpha over the page's off-white */
            const unsigned char *q = px + ((unsigned long)(y * sh / dh) * sw + x * sw / dw) * ch;
            unsigned r = q[0], g = ch >= 3 ? q[1] : q[0], b = ch >= 3 ? q[2] : q[0], a = ch == 4 ? q[3] : ch == 2 ? q[1] : 255;
            r = (r * a + 0xfa * (255 - a)) / 255; g = (g * a + 0xf8 * (255 - a)) / 255; b = (b * a + 0xf4 * (255 - a)) / 255;
            img_px[k][y * dw + x] = r << 16 | g << 8 | b;
        }
    }
    img_w[k] = dw; img_h[k] = dh;
    return 1;
}
unsigned int ticks(void);   /* ip.c, 100 a second */
#define IMG_TICKS 500   /* every image a page has gets 5 s in all: the window blocks while they load, and a slow host must not hold it */
static void load_images(char *raw) {
    char page[URL_MAX]; scopy(page, cur, slen(cur), URL_MAX);
    unsigned t0 = ticks();
    for (unsigned k = 0; k < nimg; k++) {
        if (ticks() - t0 > IMG_TICKS) { img_w[k] = img_h[k] = 0; kputs("browser: image "); kdec(k + 1); kputs(" skipped, too slow\n"); continue; }
        unsigned long mark = heap_mark();
        void (*was)(const char *) = cmd_out; cmd_out = sink;
        unsigned n = 0; char *body = 0;
        int got = fetch(img_url[k], raw, RAW_MAX);
        if (got > 0 && http_status_of(raw, (unsigned)got) == 200) body = page_body(raw, (unsigned)got, &n);
        cmd_out = was;
        img_w[k] = img_h[k] = 0;
        int ok = body && decode_image(k, (const unsigned char *)body, n);
        heap_release(mark);
        kputs("browser: image "); kdec(k + 1);
        if (ok) { kputs(" "); kdec(img_w[k]); kputs("x"); kdec(img_h[k]); kputs("\n"); } else kputs(" failed\n");
    }
    scopy(cur, page, slen(page), URL_MAX);
}

/* 6.17.0: downloads. A reply that is not text (an image, a zip, a PDF) is saved where an app's files live, not shown,
   and `fetch URL` saves any reply. The name is the address's last part as an 8.3 name, DOWNLOAD.BIN when nothing is left. */
static int want_file;   /* the next show() saves its reply whatever it is */
/* 6.23.1: a download goes in a DOWNLOAD folder (~/DOWNLOAD, or JT/DOWNLOAD with no accounts) and never over a file
   that is there. Saved beside the system's own files, a page could have written JT/WIFI.TXT (the Pi would join the
   page's network at its next start), USERS.TXT, SUDOERS.TXT or a keychain by naming its link after one. A name that is
   taken becomes NAME~1 to NAME~9; then it is refused. 1 with the name used in saved, -3 all taken, else 0 or less. */
int home_expand(const char *in, char *out, unsigned cap); int home_op(int op, const char *path, const void *data, unsigned len);
int home_walk(char *path, int keep, char **leaf); void home_back(void);
int fat_read_at(const char *name, unsigned int off, void *buf, unsigned int len);
static int dl_exists(const char *path) {
    char walk[96], *leaf; unsigned n = slen(path);
    if (n >= sizeof walk) return 1;
    for (unsigned i = 0; i <= n; i++) walk[i] = path[i];
    if (home_walk(walk, 1, &leaf) != 1) return 0;
    char c; int r = fat_read_at(leaf, 0, &c, 1);
    home_back();
    return r >= 0;
}
static int dl_save(const char *name, const void *d, unsigned n, char saved[13]) {
    char dir[96]; unsigned dn;
    if (home_active()) { if (home_expand("~/DOWNLOAD", dir, sizeof dir) <= 0) return 0; }
    else scopy(dir, "JT/DOWNLOAD", 11, sizeof dir);
    home_op(2, dir, 0, 0);   /* the folder, made when missing (an error when it is there) */
    dn = slen(dir);
    unsigned dot = 0; while (name[dot] && name[dot] != '.') dot++;
    for (unsigned k = 0; k < 10; k++) {
        char nm[13]; unsigned o = 0;
        if (!k) { scopy(nm, name, slen(name), sizeof nm); }
        else {   /* the base cut to six, then ~k, then the extension */
            for (unsigned i = 0; i < dot && i < 6; i++) nm[o++] = name[i];
            nm[o++] = '~'; nm[o++] = (char)('0' + k);
            for (unsigned i = dot; name[i] && o < 12; i++) nm[o++] = name[i];
            nm[o] = 0;
        }
        char path[112]; scopy(path, dir, dn, sizeof path); path[dn] = '/'; scopy(path + dn + 1, nm, slen(nm), sizeof path - dn - 1);
        if (dl_exists(path)) continue;
        int r = home_op(0, path, d, n);
        if (r == 1) scopy(saved, nm, slen(nm), 13);
        return r;
    }
    return -3;
}
static void file_name(const char *url, char out[13]) {
    const char *p = url, *seg = url;
    for (; *p && *p != '?' && *p != '#'; p++) if (*p == '/') seg = p + 1;
    const char *dot = 0; for (const char *q = seg; q < p; q++) if (*q == '.') dot = q;
    unsigned o = 0;
    for (const char *q = seg; q < (dot ? dot : p) && o < 8; q++) {
        char c = *q; if (c >= 'a' && c <= 'z') c = (char)(c - 32);
        if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-') out[o++] = c;
    }
    if (!o) for (const char *w = "DOWNLOAD"; *w; w++) out[o++] = *w;
    unsigned e0 = o; out[o++] = '.';
    for (const char *q = dot ? dot + 1 : p; q < p && o < e0 + 4; q++) {
        char c = *q; if (c >= 'a' && c <= 'z') c = (char)(c - 32);
        if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) out[o++] = c;
    }
    if (o == e0 + 1) for (const char *w = "BIN"; *w; w++) out[o++] = *w;
    out[o] = 0;
}

static void show(const char *url, int push) {
    unsigned long mark = heap_mark();
    char vid[12];
    if (yt_id(url, vid) && has(url, "play=1")) { yt_play(vid); heap_release(mark); return; }   /* 6.10.0: not a page, the clip */
    char *raw = kmalloc(RAW_MAX);
    if (!raw) { kputs("browser: out of memory\n"); heap_release(mark); return; }
    if (!net_get_gateway()) { kputs("browser: no network\n"); heap_release(mark); return; }
    kputs("browser: fetching\n");
    unsigned dns_was = net_dns_wait_ticks, connect_was = net_connect_wait_ticks;
    net_dns_wait_ticks = DNS_TICKS; net_connect_wait_ticks = CONNECT_TICKS;
    char was[URL_MAX]; scopy(was, cur, slen(cur), URL_MAX);
    int got = fetch(url, raw, RAW_MAX);
    net_dns_wait_ticks = dns_was; net_connect_wait_ticks = connect_was;
    unsigned blen = 0;
    char *body = got > 0 ? page_body(raw, (unsigned)got, &blen) : 0;
    int hb = got > 0 ? http_body_start(raw, (unsigned)got) : -1;
    if (body && (want_file || (hb > 0 && header_has(raw, (unsigned)hb, "content-type:", "/") && !header_has(raw, (unsigned)hb, "content-type:", "text/") && !header_has(raw, (unsigned)hb, "content-type:", "html")))) {   /* xhtml is a page too */
        char name[13]; file_name(cur, name);
        int st = http_status_of(raw, (unsigned)got);
        if (st != 200) { kputs("browser: not saved, the server said "); kdec((unsigned)st); kputs("\n"); }
        else {
            char saved[13]; int r = dl_save(name, body, blen, saved);
            if (r == 1) { kputs("browser: saved "); kputs(saved); kputs(", "); kdec(blen); kputs(home_active() ? " bytes in your DOWNLOAD folder\n" : " bytes in JT/DOWNLOAD\n"); }
            else kputs(r == -3 ? "browser: not saved, ten files already have that name\n" : "browser: could not save the file\n");
        }
        scopy(cur, was, slen(was), URL_MAX);   /* the page on screen stays */
        want_file = 0; heap_release(mark); return;
    }
    want_file = 0;
    if (body) {
        to_text(body, blen);
        load_images(raw);
        if (push) {   /* a new page: it follows the one on screen, anything after that is forgotten */
            if (hn && hi + 1 < hn) hn = hi + 1;
            if (hn == HIST_MAX) { for (unsigned k = 1; k < HIST_MAX; k++) scopy(hist[k - 1], hist[k], slen(hist[k]), URL_MAX); hn--; }
            scopy(hist[hn], cur, slen(cur), URL_MAX); hi = hn++;
        }
        loads++;
        say_wrapped("browser: ", title[0] ? title : cur, slen(title[0] ? title : cur));
        show_lines(0, PAGE_LINES);
        kputs("links: "); kdec(nlinks); kputs(nlinks ? ", open N follows one\n" : "\n");
    } else scopy(cur, was, slen(was), URL_MAX);   /* the old page stays */
    heap_release(mark);
}

static unsigned number(const char *q, unsigned n, unsigned s, unsigned *end) {
    unsigned k = 0; while (s < n && q[s] >= '0' && q[s] <= '9') k = k * 10 + (unsigned)(q[s++] - '0');
    *end = s; return k;
}
static int word(const char *q, unsigned n, const char *w) { unsigned k = slen(w); return n == k && starts(q, w); }

/* 6.15.0: a form's named fields as name=value&..., percent-encoded, into out; then GET action?query or POST it. */
static unsigned urlenc(char *out, unsigned o, unsigned max, const char *v) {
    static const char hex[] = "0123456789ABCDEF";
    for (; *v && o + 3 < max; v++) {
        unsigned char c = (unsigned char)*v;
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '.' || c == '_' || c == '~') out[o++] = (char)c;
        else if (c == ' ') out[o++] = '+';
        else { out[o++] = '%'; out[o++] = hex[c >> 4]; out[o++] = hex[c & 15]; }
    }
    return o;
}
static void submit(unsigned f) {
    static char query[1024]; unsigned o = 0;
    for (unsigned k = 0; k < nfields; k++) if (fields[k].form == f) {
        if (o) query[o++] = '&';
        o = urlenc(query, o, sizeof query, fields[k].name);
        if (o < sizeof query - 1) query[o++] = '=';
        o = urlenc(query, o, sizeof query, fields[k].value);
    }
    query[o] = 0;
    char url[URL_MAX]; scopy(url, forms[f].action, slen(forms[f].action), URL_MAX);
    kputs(forms[f].post ? "browser: post " : "browser: get "); kputs(url); kputs("\n");
    if (forms[f].post) { post_body = query; post_len = o; }
    else {   /* the query replaces any the action had */
        unsigned u = 0; while (url[u] && url[u] != '?') u++;
        url[u++] = '?';
        for (unsigned i = 0; i < o && u < URL_MAX - 1; i++) url[u++] = query[i];
        url[u] = 0;
    }
    show(url, 1);
}

static void tab_keep(void) {   /* the current tab's history, into its slot */
    for (unsigned k = 0; k < hn; k++) scopy(tabs[tab_cur].hist[k], hist[k], slen(hist[k]), URL_MAX);
    tabs[tab_cur].hn = hn; tabs[tab_cur].hi = hi;
}
static void tab_take(unsigned t) {   /* tab t in front: its history back, its page fetched again, or the empty page */
    tab_cur = t; hn = tabs[t].hn; hi = tabs[t].hi;
    for (unsigned k = 0; k < hn; k++) scopy(hist[k], tabs[t].hist[k], slen(tabs[t].hist[k]), URL_MAX);
    cur[0] = 0; tlen = 0; nline = 0; view = 0; nlinks = 0; nimg = 0; nforms = nfields = 0; title[0] = 0; text[0] = 0;
    kputs("browser: tab "); kdec(t + 1); kputs(" of "); kdec(ntabs); kputs("\n");
    if (hn) { char url[URL_MAX]; scopy(url, hist[hi], slen(hist[hi]), URL_MAX); show(url, 0); }
    else loads++;
}
static int tab_command(const char *q, unsigned n) {   /* tab new, tab N, tab close, tabs */
    if (word(q, n, "tabs")) {
        tab_keep();
        for (unsigned t = 0; t < ntabs; t++) {
            kputs(t == tab_cur ? "* " : "  "); kdec(t + 1); kputs(" ");
            kputs(tabs[t].hn ? tabs[t].hist[tabs[t].hi] : "(empty)"); kputs("\n");
        }
        return 1;
    }
    if (word(q, n, "tab new")) {
        if (ntabs == TAB_MAX) { kputs("browser: tabs full, close one first\n"); return 1; }
        tab_keep(); tabs[ntabs].hn = tabs[ntabs].hi = 0; ntabs++;
        tab_take(ntabs - 1);
        return 1;
    }
    if (word(q, n, "tab close")) {
        if (ntabs == 1) { hn = hi = 0; tab_keep(); tab_take(0); return 1; }   /* the last tab: emptied, not gone */
        for (unsigned t = tab_cur; t + 1 < ntabs; t++) tabs[t] = tabs[t + 1];
        ntabs--;
        tab_take(tab_cur < ntabs ? tab_cur : ntabs - 1);
        return 1;
    }
    if (n > 4 && starts(q, "tab ")) {
        unsigned e, t = number(q, n, 4, &e);
        if (e != n || !t || t > ntabs) { kputs("browser: no such tab\n"); return 1; }
        if (t - 1 != tab_cur) { tab_keep(); tab_take(t - 1); }
        return 1;
    }
    return 0;
}

/* One command line, after cmd.c trimmed it. 1 when it was a browser command. */
int browse_command(const char *q, unsigned n) {
    if (tab_command(q, n)) return 1;
    if (n > 7 && starts(q, "browse ")) {
        unsigned s = 7; while (s < n && q[s] == ' ') s++;
        if (n - s >= URL_MAX) { kputs("browser: url too long\n"); return 1; }
        char url[URL_MAX]; scopy(url, q + s, n - s, URL_MAX);
        if (!starts(url, "http://") && !starts(url, "https://")) {   /* a bare host: https */
            char full[URL_MAX]; unsigned o = 0;
            for (const char *p = "https://"; *p; p++) full[o++] = *p;
            for (unsigned i = 0; url[i] && o < URL_MAX - 1; i++) full[o++] = url[i];
            full[o] = 0; scopy(url, full, o, URL_MAX);
        }
        show(url, 1);
        return 1;
    }
    if (n > 5 && starts(q, "open ")) {
        unsigned e, k = number(q, n, 5, &e);
        if (e != n || !k || k > nlinks) { kputs("browser: no such link\n"); return 1; }
        if (starts(links[k - 1], "field:")) {
            kputs("browser: field "); kputs(fields[links[k - 1][6] - '0'].name); kputs(", fill "); kdec(k); kputs(" TEXT sets it\n");
            return 1;
        }
        if (starts(links[k - 1], "submit:")) { submit((unsigned)(links[k - 1][7] - '0')); return 1; }
        char url[URL_MAX]; scopy(url, links[k - 1], slen(links[k - 1]), URL_MAX);
        show(url, 1);
        return 1;
    }
    if (n > 6 && starts(q, "fetch ")) {   /* 6.17.0: fetch URL saves the reply as a file */
        unsigned s = 6; while (s < n && q[s] == ' ') s++;
        if (n - s >= URL_MAX) { kputs("browser: url too long\n"); return 1; }
        char url[URL_MAX]; scopy(url, q + s, n - s, URL_MAX);
        want_file = 1; show(url, 0); want_file = 0;
        return 1;
    }
    if (n > 5 && starts(q, "fill ")) {   /* 6.15.0: fill N TEXT, N a field's link number */
        unsigned e, k = number(q, n, 5, &e);
        if (!k || k > nlinks || !starts(links[k - 1], "field:") || (e < n && q[e] != ' ')) { kputs("browser: no such field\n"); return 1; }
        unsigned f = (unsigned)(links[k - 1][6] - '0'), t = e < n ? e + 1 : n;
        scopy(fields[f].value, q + t, n - t, sizeof fields[f].value);
        kputs("browser: field "); kputs(fields[f].name); kputs(" set\n");   /* never the value: it may be a password */
        return 1;
    }
    if (word(q, n, "back") || word(q, n, "forward")) {
        int fwd = q[0] == 'f';
        if (fwd ? hi + 1 >= hn : !hi) { kputs(fwd ? "browser: nothing ahead\n" : "browser: nothing behind\n"); return 1; }
        hi += fwd ? 1 : -1;
        char url[URL_MAX]; scopy(url, hist[hi], slen(hist[hi]), URL_MAX);
        show(url, 0);
        return 1;
    }
    if (word(q, n, "reload")) {
        if (!cur[0]) { kputs("browser: no page, browse URL first\n"); return 1; }
        char url[URL_MAX]; scopy(url, cur, slen(cur), URL_MAX);
        show(url, 0);
        return 1;
    }
    if (word(q, n, "links")) {
        if (!cur[0]) { kputs("browser: no page, browse URL first\n"); return 1; }
        for (unsigned k = 0; k < nlinks; k++) { kputs("["); kdec(k + 1); kputs("] "); say_wrapped("", links[k], slen(links[k])); }
        kputs("links: "); kdec(nlinks); kputs("\n");
        return 1;
    }
    if (word(q, n, "top")) { show_lines(0, PAGE_LINES); return 1; }
    if (word(q, n, "bottom")) { show_lines(nline > PAGE_LINES ? nline - PAGE_LINES : 0, PAGE_LINES); return 1; }
    if (word(q, n, "more") || word(q, n, "down")) {
        if (cur[0] && view + PAGE_LINES >= nline) { kputs("browser: end of page\n"); return 1; }
        show_lines(view + PAGE_LINES, PAGE_LINES); return 1;
    }
    if (word(q, n, "up")) { show_lines(view > PAGE_LINES ? view - PAGE_LINES : 0, PAGE_LINES); return 1; }
    if (n > 5 && starts(q, "find ")) {
        if (!cur[0]) { kputs("browser: no page, browse URL first\n"); return 1; }
        const char *w = q + 5; unsigned wn = n - 5;
        unsigned i = line_start(view + 1), k = view + 1;   /* from the line after the top of the view, round to the top */
        for (unsigned tries = 0; tries <= nline; tries++, k++) {
            if (k >= nline) { k = 0; i = 0; }
            unsigned e = i; while (e < tlen && text[e] != '\n') e++;
            for (unsigned p = i; p + wn <= e; p++) {
                unsigned m = 0; while (m < wn && (text[p + m] | 32) == (w[m] | 32)) m++;
                if (m == wn) { kputs("browser: found\n"); show_lines(k, PAGE_LINES); return 1; }
            }
            i = e + 1;
        }
        kputs("browser: not found\n");
        return 1;
    }
    return 0;
}

/* The Browser window (browser_ui.h) reads the page through these instead of the printed lines. */
const char *browser_text(unsigned *len) { *len = tlen; return text; }   /* the page as lines of text, '\n' between them, "[N]" after a link */
const char *browser_title(void) { return title; }                     /* its <title>, up to 60 characters, "" when it has none */
const char *browser_url(void) { return cur; }                          /* the page on screen, "" before the first */
const unsigned *browser_image(unsigned k, unsigned *w, unsigned *h) { if (k >= nimg || !img_w[k]) return 0; *w = img_w[k]; *h = img_h[k]; return img_px[k]; }   /* 6.14.0 */
unsigned browser_tabs(unsigned *front) { *front = tab_cur + 1; return ntabs; }   /* 6.16.0: how many tabs, and which is in front */
/* 6.18.0: a page made somewhere else (mail.c's message): shown as a page named name, with no history and its images never
   fetched (a remote image in a mail tells its sender it was opened). */
void browser_render(const char *html, unsigned n, const char *name) {
    scopy(cur, name, slen(name), URL_MAX);
    to_text(html, n);
    for (unsigned k = 0; k < IMG_MAX; k++) img_w[k] = img_h[k] = 0;
    loads++;
    say_wrapped("browser: ", title[0] ? title : cur, slen(title[0] ? title : cur));
    show_lines(0, PAGE_LINES);
}
/* 6.19.0: GET url quietly for another part of the kernel (ics.c): its body, chunks joined and gzip undone, in a buffer the
   caller's heap mark covers. The HTTP status, or -1 when nothing came back; the page on screen is left alone. */
int browser_get(const char *url, char **body, unsigned *n) {
    char was[URL_MAX]; scopy(was, cur, slen(cur), URL_MAX);
    *body = 0; *n = 0;
    char *raw = kmalloc(RAW_MAX);
    if (!raw) return -1;
    void (*out)(const char *) = cmd_out; cmd_out = sink;
    int got = fetch(url, raw, RAW_MAX), st = got > 0 ? http_status_of(raw, (unsigned)got) : -1;
    if (got > 0) *body = page_body(raw, (unsigned)got, n);
    cmd_out = out;
    scopy(cur, was, slen(was), URL_MAX);
    return *body ? st : -1;
}
int browser_field_secret(unsigned n) { return n && n <= nlinks && starts(links[n - 1], "field:") && fields[links[n - 1][6] - '0'].kind == 1; }   /* 6.15.0: a password field */
const char *browser_link(unsigned n) { return n && n <= nlinks ? links[n - 1] : 0; }
unsigned browser_loads(void) { return loads; }
