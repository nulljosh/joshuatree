/* A text browser behind the one command line (cmd.c). `browse URL` fetches the page (https through tls.c, http
   through the shared stack), follows up to 3 redirects, turns the HTML into readable text and prints it in pages of
   PAGE_LINES lines; `more`, `up`, `down`, `top`, `bottom` move through it, `find WORD` jumps to a line, `links` lists
   the numbered links and `open N` follows one. The last 8 pages are history for `back` and `forward`; `reload` fetches
   the page again. Every print ends with one status line: the URL and `line X of Y`. Every line fits the 53-column
   rule through ask.c's say_wrapped; everything goes out through cmd_out, so a Terminal app can own the output later.
   Static buffers: one page at a time, the raw reply capped at 64 KiB, the text at 8 KiB. */
#include "../../drivers/net.h"
#include "../../drivers/http.h"
#include "tls.h"

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
#define RAW_MAX 65536       /* the reply, headers and all */
#define TEXT_MAX 8192       /* the readable text: half the Terminal's log, so a page never pushes itself out */
#define HIST_MAX 8
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
static int fetch(const char *url, char *raw, unsigned max) {
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
        static char req[URL_MAX + 160]; unsigned n = 0;
        for (const char *p = "GET "; *p; p++) req[n++] = *p;
        for (unsigned i = 0; i < u.path_len; i++) req[n++] = u.path[i];
        for (const char *p = " HTTP/1.0\r\nHost: "; *p; p++) req[n++] = *p;
        for (const char *p = u.host; *p; p++) req[n++] = *p;
        for (const char *p = "\r\nUser-Agent: JoshuaTree/1.0\r\nConnection: close\r\n\r\n"; *p; p++) req[n++] = *p;
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
        if ((unsigned)got >= max - 1) kputs("browser: page too big, showing the first 64 KiB\n");
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
static void to_text(const char *h, unsigned n) {
    unsigned o = 0, i = 0, skip = 0, space = 1;
    int in_title = 0, link = 0; unsigned tl = 0;
    nlinks = 0; title[0] = 0;
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
                else if (heading || is_tag(tag, tn, "p") || is_tag(tag, tn, "br") || is_tag(tag, tn, "div") || is_tag(tag, tn, "li") || is_tag(tag, tn, "tr")
                         || is_tag(tag, tn, "ul") || is_tag(tag, tn, "ol") || is_tag(tag, tn, "table") || is_tag(tag, tn, "blockquote") || is_tag(tag, tn, "pre") || is_tag(tag, tn, "hr")) {
                    if (o && text[o - 1] != '\n') o = put(text, o, '\n');
                    if (!close && (heading || is_tag(tag, tn, "p")) && o && text[o - 1] == '\n' && !(o > 1 && text[o - 2] == '\n')) o = put(text, o, '\n');   /* a blank line before */
                    if (!close && is_tag(tag, tn, "li")) { o = put(text, o, '-'); o = put(text, o, ' '); }
                    space = 1;
                }
            }
            i = e + 1; continue;
        }
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
        if (e == i) kputs("\n"); else say_wrapped("", text + i, e - i);
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
    if (got > 0) {
        int b = http_body_start(raw, (unsigned)got); if (b < 0) b = 0;
        to_text(raw + b, (unsigned)got - (unsigned)b);
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

/* One command line, after cmd.c trimmed it. 1 when it was a browser command. */
int browse_command(const char *q, unsigned n) {
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
        char url[URL_MAX]; scopy(url, links[k - 1], slen(links[k - 1]), URL_MAX);
        show(url, 1);
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
const char *browser_link(unsigned n) { return n && n <= nlinks ? links[n - 1] : 0; }
unsigned browser_loads(void) { return loads; }
