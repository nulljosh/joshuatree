/* A text browser in the Console. `browse URL` at the ask> row fetches the page (https through tls.c, http through the
   shared stack), follows up to 3 redirects, turns the HTML into readable text and prints it into the Console, which
   already scrolls (Page Up, Page Down, Home, End). Each link gets a number after its text, `[3]`; `open 3` fetches it.
   Every line fits the 53-column rule through ask.c's say_wrapped. Everything is static: one page at a time. */
#include "../../drivers/net.h"
#include "../../drivers/http.h"
#include "tls.h"

void kputs(const char *s);                     /* main.c */
void kdec(unsigned v);
void say_wrapped(const char *prefix, const char *s, unsigned n);   /* ask.c */
unsigned long heap_mark(void);
void heap_release(unsigned long m);
void *kmalloc(unsigned int n);

#define URL_MAX 256
#define LINK_MAX 32
#define RAW_MAX 65536       /* the reply, headers and all */
#define TEXT_MAX 8192       /* the readable text: half the Console log, so a page never pushes itself out */
#define HTTP_TICKS 2000     /* 20 s for a plain-http reply */

static char cur[URL_MAX];                 /* the page on screen, the base for relative links */
static char links[LINK_MAX][URL_MAX];
static unsigned nlinks;

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
static int fetch(const char *url, char *raw, unsigned max) {
    char next[URL_MAX];
    scopy(cur, url, slen(url), URL_MAX);
    for (int hop = 0; hop <= 3; hop++) {
        struct url u;
        if (!parse_url(cur, &u)) { kputs("browser: bad url\n"); return -1; }
        static char req[URL_MAX + 160]; unsigned n = 0;
        for (const char *p = "GET "; *p; p++) req[n++] = *p;
        for (unsigned i = 0; i < u.path_len; i++) req[n++] = u.path[i];
        for (const char *p = " HTTP/1.0\r\nHost: "; *p; p++) req[n++] = *p;
        for (const char *p = u.host; *p; p++) req[n++] = *p;
        for (const char *p = "\r\nUser-Agent: JoshuaTree/1.0\r\nConnection: close\r\n\r\n"; *p; p++) req[n++] = *p;
        int got;
        if (u.tls) got = https_fetch(u.host, u.port, req, n, raw, max - 1);
        else {
            unsigned ip;
            if (!http_resolve_host(u.host, &ip)) { kputs("browser: no such host\n"); return -1; }
            got = tcp_get_timeout(ip, u.port, req, n, raw, max - 1, HTTP_TICKS);
        }
        if (got <= 0) {
            kputs("browser: ");
            if (u.tls && tls_last_error() > 0) { kputs("tls error "); kdec((unsigned)tls_last_error()); kputs(u.tls && tls_last_error() == 62 ? " (not trusted)\n" : "\n"); }
            else if (u.tls && tls_last_error() == TLS_ERR_DNS) kputs("no such host\n");
            else kputs(got == 0 ? "empty reply\n" : "no reply\n");
            return -1;
        }
        raw[got] = 0;
        int status = http_status_of(raw, (unsigned)got);
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
        if (status != 200) { kputs("browser: http "); kdec((unsigned)status); kputs("\n"); }
        return got;
    }
    return -1;
}

/* HTML to text. Tags go; block tags become line breaks; script and style bodies are skipped; the title is kept;
   a few entities are decoded; runs of white space collapse to one space; an <a href> ends in its link number. */
static unsigned put(char *t, unsigned o, char c) { if (o < TEXT_MAX - 1) t[o++] = c; return o; }
static unsigned entity(const char *s, char *c) {   /* bytes consumed after '&', 0 when it is not one we know */
    static const struct { const char *name; char ch; } ents[] = { {"amp;", '&'}, {"lt;", '<'}, {"gt;", '>'}, {"quot;", '"'}, {"apos;", '\''}, {"nbsp;", ' '}, {"#39;", '\''} };
    for (unsigned i = 0; i < sizeof ents / sizeof ents[0]; i++) if (starts(s, ents[i].name)) { *c = ents[i].ch; return slen(ents[i].name); }
    return 0;
}
static unsigned to_text(const char *h, unsigned n, char *t, char *title) {
    unsigned o = 0, i = 0, skip = 0, space = 1;
    int in_title = 0, link = 0; unsigned tl = 0;
    nlinks = 0; title[0] = 0;
    while (i < n) {
        char c = h[i];
        if (c == '<') {
            unsigned e = i + 1; while (e < n && h[e] != '>') e++;
            const char *tag = h + i + 1; unsigned tn = e - i - 1;
            if (tn >= 3 && tag[0] == '!' && tag[1] == '-' && tag[2] == '-') {   /* a comment: to --> */
                e = i + 4; while (e + 2 < n && !(h[e] == '-' && h[e + 1] == '-' && h[e + 2] == '>')) e++;
                i = e + 3; continue;
            }
            int close = tag[0] == '/'; if (close) { tag++; tn--; }
            if (!skip && !close && (starts(tag, "script") || starts(tag, "style"))) skip = starts(tag, "script") ? 1 : 2;
            else if (skip && close && starts(tag, skip == 1 ? "script" : "style")) skip = 0;
            else if (!skip) {
                if (starts(tag, "title")) in_title = !close;
                else if (starts(tag, "a") && (tn == 1 || tag[1] == ' ' || tag[1] == '\n' || tag[1] == '\t')) {
                    if (!close) {
                        link = 0;
                        for (unsigned k = 1; k + 5 < tn; k++) if (starts(tag + k, "href=") && (tag[k - 1] == ' ' || tag[k - 1] == '\n' || tag[k - 1] == '\t')) {
                            unsigned s = k + 5; char q = tag[s]; unsigned z;
                            if (q == '"' || q == '\'') { s++; z = s; while (z < tn && tag[z] != q) z++; } else { z = s; while (z < tn && tag[z] != ' ' && tag[z] != '>') z++; }
                            if (nlinks < LINK_MAX) { resolve(tag + s, z - s, links[nlinks]); if (links[nlinks][0]) link = (int)++nlinks; }
                            break;
                        }
                    } else if (link) {   /* "text[3]" */
                        o = put(t, o, '['); char d[4]; unsigned k = 0, v = (unsigned)link; do d[k++] = (char)('0' + v % 10); while (v /= 10); while (k) o = put(t, o, d[--k]);
                        o = put(t, o, ']'); link = 0; space = 0;
                    }
                }
                else if (starts(tag, "p") && (tn == 1 || tag[1] == ' ') ? 1 : starts(tag, "br") || starts(tag, "div") || starts(tag, "li") || starts(tag, "tr") || starts(tag, "h1") || starts(tag, "h2") || starts(tag, "h3")
                         || starts(tag, "h4") || starts(tag, "h5") || starts(tag, "h6") || starts(tag, "ul") || starts(tag, "ol") || starts(tag, "table") || starts(tag, "blockquote") || starts(tag, "pre") || starts(tag, "hr")) {
                    if (o && t[o - 1] != '\n') o = put(t, o, '\n');
                    if (!close && starts(tag, "li")) { o = put(t, o, '-'); o = put(t, o, ' '); }
                    space = 1;
                }
            }
            i = e + 1; continue;
        }
        if (skip) { i++; continue; }
        if (c == '&') { char d; unsigned k = entity(h + i + 1, &d); if (k) { c = d; i += k; } }
        i++;
        int ws = c == ' ' || c == '\n' || c == '\r' || c == '\t';
        if (in_title) { if (ws) c = ' '; if (tl < 60 && !(ws && (!tl || title[tl - 1] == ' '))) { title[tl++] = c; title[tl] = 0; } continue; }
        if (ws) { if (!space) { o = put(t, o, ' '); space = 1; } continue; }
        o = put(t, o, (c >= 32 && c < 127) ? c : '?'); space = 0;
    }
    while (o && (t[o - 1] == ' ' || t[o - 1] == '\n')) o--;
    while (tl && title[tl - 1] == ' ') title[--tl] = 0;
    return o;
}

static void show(const char *url) {
    unsigned long mark = heap_mark();
    char *raw = kmalloc(RAW_MAX), *text = kmalloc(TEXT_MAX); char title[64];
    if (!raw || !text) { kputs("browser: out of memory\n"); heap_release(mark); return; }
    if (!net_get_gateway()) { kputs("browser: no network\n"); heap_release(mark); return; }
    kputs("browser: fetching\n");
    int got = fetch(url, raw, RAW_MAX);
    if (got > 0) {
        int b = http_body_start(raw, (unsigned)got); if (b < 0) b = 0;
        unsigned n = to_text(raw + b, (unsigned)got - (unsigned)b, text, title);
        say_wrapped("browser: ", title[0] ? title : cur, slen(title[0] ? title : cur));
        say_wrapped("", text, n);
        kputs("links: "); kdec(nlinks); kputs(nlinks ? ", open N follows one\n" : "\n");
    }
    heap_release(mark);
}

/* The ask> line, before it goes to Claude. 1 when it was a browser command. */
int browse_command(const char *q, unsigned n) {
    if (n > 7 && starts(q, "browse ")) {
        unsigned s = 7; while (s < n && q[s] == ' ') s++;
        unsigned e = n; while (e > s && q[e - 1] == ' ') e--;
        if (e - s >= URL_MAX) { kputs("browser: url too long\n"); return 1; }
        char url[URL_MAX]; scopy(url, q + s, e - s, URL_MAX);
        if (!starts(url, "http://") && !starts(url, "https://")) {   /* a bare host: https */
            char full[URL_MAX]; unsigned o = 0;
            for (const char *p = "https://"; *p; p++) full[o++] = *p;
            for (unsigned i = 0; url[i] && o < URL_MAX - 1; i++) full[o++] = url[i];
            full[o] = 0; scopy(url, full, o, URL_MAX);
        }
        show(url);
        return 1;
    }
    if (n > 5 && starts(q, "open ")) {
        unsigned k = 0, s = 5; while (s < n && q[s] >= '0' && q[s] <= '9') k = k * 10 + (unsigned)(q[s++] - '0');
        if (s != n || !k || k > nlinks) { kputs("browser: no such link\n"); return 1; }
        char url[URL_MAX]; scopy(url, links[k - 1], slen(links[k - 1]), URL_MAX);
        show(url);
        return 1;
    }
    return 0;
}
