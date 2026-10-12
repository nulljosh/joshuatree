/* Nimble on the Pi: one question to Nimble's public answer API, one short answer back, and up to three sources. Nimble
   (https://nimble.jaybulb.com, Joshua's answer engine) takes `GET /api/answer?q=...` and replies with JSON: a text answer
   with a `body` and `sources` (name, url), or a card (big, unit, sub, src, url), a unit conversion or a sum. When its own
   free AI quota is resting it answers from Wikipedia, so this file needs no fallback of its own.

   Three callers share it and nothing else asks: the Terminal's `nimble QUESTION` (cmd.c), Samantha's `nimble` plugin
   (ask.c's plugin table) and the Nimble window (nimble_ui.h). The window reads the result through the accessors at the
   bottom. A request is one GET through the same TLS and TCP client the Browser uses (https through tls.c, plain http
   through the shared stack for the host checks), with DNS, connect and reply budgets in ticks (100 a second).

   The reply is network data and treated as hostile: the raw reply is capped at 32 KiB (a longer one is refused, never
   parsed), the JSON is walked by structure with no recursion and no allocation (strings are length-checked, a value
   that does not fit is cut, an unterminated one ends the walk), the answer is plain ASCII of at most 1000 characters,
   and a source is kept only if its URL starts http:// or https:// and fits the Browser's address bar. Every line the
   Terminal prints goes through say_wrapped's 53-column rule. The base URL is compiled in (NIMBLE_BASE, the Makefile
   sets it only for arm64-nimble-check.py, which points it at a stub on the host). */
#include "../../drivers/net.h"
#include "../../drivers/http.h"
#include "tls.h"

#ifndef NIMBLE_BASE
#define NIMBLE_BASE "https://nimble.jaybulb.com"
#endif

extern void (*cmd_out)(const char *s);         /* cmd.c */
#define kputs cmd_out
void say_wrapped(const char *prefix, const char *s, unsigned n);   /* ask.c */
unsigned long heap_mark(void);
void heap_release(unsigned long m);
void *kmalloc(unsigned int n);

#define Q_MAX 120           /* the question the Terminal and the window take */
#define ANS_MAX 1000        /* the answer, in characters */
#define SRC_MAX 3
#define TITLE_MAX 60
#define URL_MAX 200         /* the Browser's address bar holds 200 */
#define RAW_MAX 32768       /* the whole reply, headers and all */
#define PATH_MAX_LEN 400
/* Budgets in ticks (100 a second). The answer itself can take a few seconds (an AI model behind it), so the reply gets
   20 s; DNS and the TCP connect keep the Browser's short ones. */
#define DNS_TICKS 300
#define CONNECT_TICKS 400
#define REPLY_TICKS 2000

#define NIM_IDLE 0
#define NIM_OK 1
#define NIM_NONET 2
#define NIM_ERR 3

static int nstate;
static char nans[ANS_MAX + 1];
static unsigned nans_n, nsrc, nasks;
static struct nsrc { char title[TITLE_MAX]; char url[URL_MAX]; } nsrcs[SRC_MAX];
static char nerr[64];

static unsigned slen(const char *s) { unsigned n = 0; while (s[n]) n++; return n; }
static int starts(const char *s, const char *w) { for (; *w; s++, w++) if ((*s | 32) != (*w | 32)) return 0; return 1; }
static int same(const char *a, const char *b) { while (*a && *a == *b) { a++; b++; } return *a == *b; }
static void scopy(char *d, const char *s, unsigned max) { unsigned n = 0; while (s[n] && n + 1 < max) { d[n] = s[n]; n++; } d[n] = 0; }
static void fail(int state, const char *why) { nstate = state; scopy(nerr, why, sizeof nerr); nans[0] = 0; nans_n = 0; nsrc = 0; }

/* ---- The JSON walk. A cursor over the raw body, bounded by its length. ---- */
struct jp { const char *s; unsigned n, i; };
#define J_DONE 1    /* the string's closing quote was seen */
#define J_CUT 2     /* it did not fit and was cut */
#define J_WIDE 4    /* it held a character outside ASCII */
static void jws(struct jp *p) { while (p->i < p->n && (p->s[p->i] == ' ' || p->s[p->i] == '\n' || p->s[p->i] == '\r' || p->s[p->i] == '\t')) p->i++; }
static void jput(char *out, unsigned max, unsigned *o, const char *w, unsigned *flags) {   /* append w, or note that it was cut */
    for (; *w; w++) { if (*o + 1 < max) out[(*o)++] = *w; else *flags |= J_CUT; }
}
static void jcp(char *out, unsigned max, unsigned *o, unsigned cp, unsigned *flags) {   /* one code point as ASCII: the few typographic ones read as their plain forms */
    char t[2] = {0, 0};
    if (cp == '\n') t[0] = '\n';
    else if (cp < 32) t[0] = ' ';
    else if (cp < 127) t[0] = (char)cp;
    else {
        *flags |= J_WIDE;
        if (cp == 0xB0) { jput(out, max, o, "deg", flags); return; }
        t[0] = cp == 0x2018 || cp == 0x2019 ? '\'' : cp == 0x201C || cp == 0x201D ? '"' : cp == 0x2013 || cp == 0x2014 ? '-' : cp == 0xA0 ? ' ' : '?';
    }
    jput(out, max, o, t, flags);
}
/* The string at p->i (a quote) into out, ASCII, at most max-1 characters. Returns J_* flags; J_DONE only if it ended. */
static unsigned jstr(struct jp *p, char *out, unsigned max) {
    unsigned o = 0, flags = 0;
    p->i++;
    while (p->i < p->n) {
        unsigned char c = (unsigned char)p->s[p->i++];
        if (c == '"') { out[o] = 0; return flags | J_DONE; }
        unsigned cp = c;
        if (c == '\\') {
            if (p->i >= p->n) break;
            c = (unsigned char)p->s[p->i++]; cp = c;
            if (c == 'n') cp = '\n';
            else if (c == 't' || c == 'r' || c == 'b' || c == 'f') cp = ' ';
            else if (c == 'u') {
                if (p->n - p->i < 4) { p->i = p->n; break; }
                cp = 0;
                for (int k = 0; k < 4; k++) {
                    char h = p->s[p->i++]; unsigned v = h >= '0' && h <= '9' ? (unsigned)(h - '0') : (h | 32) >= 'a' && (h | 32) <= 'f' ? (unsigned)((h | 32) - 'a' + 10) : 0;
                    cp = cp * 16 + v;
                }
                if (cp >= 0xD800 && cp < 0xE000) { if (cp >= 0xDC00) continue; cp = '?'; flags |= J_WIDE; }   /* a surrogate pair: one '?' for the pair */
            }
        } else if (c >= 0xC0) {   /* UTF-8: the lead byte says how many follow */
            unsigned extra = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : 1;
            cp = c & (0x3F >> extra);
            while (extra-- && p->i < p->n && ((unsigned char)p->s[p->i] & 0xC0) == 0x80) cp = (cp << 6) | ((unsigned char)p->s[p->i++] & 0x3F);
        } else if (c >= 0x80) cp = '?';
        jcp(out, max, &o, cp, &flags);
    }
    out[o] = 0;
    return flags;
}
static int jskip(struct jp *p) {   /* one value of any kind, nested ones included, without recursion. 0 if the input ended inside it */
    jws(p);
    if (p->i >= p->n) return 0;
    char c = p->s[p->i];
    if (c == '"' || c == '{' || c == '[') {
        unsigned depth = 0;
        while (p->i < p->n) {
            c = p->s[p->i++];
            if (c == '"') {
                for (;;) { if (p->i >= p->n) return 0; c = p->s[p->i++]; if (c == '\\') p->i++; else if (c == '"') break; }
                if (!depth) return 1;
            } else if (c == '{' || c == '[') depth++;
            else if ((c == '}' || c == ']') && depth && --depth == 0) return 1;
        }
        return 0;
    }
    while (p->i < p->n && p->s[p->i] != ',' && p->s[p->i] != '}' && p->s[p->i] != ']') p->i++;
    return 1;
}
static unsigned jfield(struct jp *p, char *dst, unsigned max) {   /* a string value into dst; any other kind of value is skipped and dst stays empty */
    jws(p);
    if (p->i < p->n && p->s[p->i] == '"') return jstr(p, dst, max);
    dst[0] = 0; jskip(p);
    return 0;
}
static void jnum(struct jp *p, char *dst, unsigned max) {   /* a number or a string value as text */
    jws(p);
    if (p->i < p->n && p->s[p->i] == '"') { jstr(p, dst, max); return; }
    unsigned o = 0;
    while (p->i < p->n && p->s[p->i] != ',' && p->s[p->i] != '}' && p->s[p->i] != ']' && p->s[p->i] > ' ') { if (o + 1 < max) dst[o++] = p->s[p->i]; p->i++; }
    dst[o] = 0;
}
static int web_url(const char *u, unsigned flags) {   /* a source worth keeping: whole, ASCII, http or https */
    if ((flags & (J_CUT | J_WIDE)) || !(flags & J_DONE)) return 0;
    if (!starts(u, "http://") && !starts(u, "https://")) return 0;
    for (; *u; u++) if (*u <= ' ' || *u > '~') return 0;
    return 1;
}
/* An array of {name|title, url} objects into list (at most SRC_MAX): "sources" uses name, "refs" uses title. */
static void jsources(struct jp *p, const char *namekey, struct nsrc *list, unsigned *count) {
    jws(p);
    if (p->i >= p->n || p->s[p->i] != '[') { jskip(p); return; }
    p->i++;
    for (;;) {
        jws(p);
        if (p->i >= p->n) return;
        char c = p->s[p->i];
        if (c == ']') { p->i++; return; }
        if (c == ',') { p->i++; continue; }
        if (c != '{') { if (!jskip(p)) return; continue; }
        p->i++;
        char title[TITLE_MAX], url[URL_MAX]; unsigned uf = 0;
        title[0] = url[0] = 0;
        for (;;) {
            jws(p);
            if (p->i >= p->n) return;
            c = p->s[p->i];
            if (c == '}') { p->i++; break; }
            if (c == ',') { p->i++; continue; }
            if (c != '"') return;
            char key[16]; jstr(p, key, sizeof key);
            jws(p);
            if (p->i >= p->n || p->s[p->i] != ':') return;
            p->i++;
            if (same(key, namekey)) jfield(p, title, sizeof title);
            else if (same(key, "url")) uf = jfield(p, url, sizeof url);
            else if (!jskip(p)) return;
        }
        if (*count < SRC_MAX && web_url(url, uf)) { scopy(list[*count].title, title[0] ? title : url, TITLE_MAX); scopy(list[*count].url, url, URL_MAX); (*count)++; }
    }
}

/* The reply's JSON into the result. 0 when it was an answer, -1 with nerr set when it was not. */
static struct { char kind[16], body[ANS_MAX + 1], big[40], unit[16], sub[160], src[32], url[URL_MAX], from[32], to[32], fu[24], tu[24], value[32], error[64]; } f;
static struct nsrc srcs_a[SRC_MAX], srcs_r[SRC_MAX];
static int parse(const char *s, unsigned n) {
    struct jp p = { s, n, 0 };
    unsigned na = 0, nr = 0, uf = 0;
    f.kind[0] = f.body[0] = f.big[0] = f.unit[0] = f.sub[0] = f.src[0] = f.url[0] = f.from[0] = f.to[0] = f.fu[0] = f.tu[0] = f.value[0] = f.error[0] = 0;
    jws(&p);
    if (p.i >= p.n || s[p.i] != '{') { fail(NIM_ERR, "the reply was not an answer"); return -1; }
    p.i++;
    for (;;) {
        jws(&p);
        if (p.i >= p.n || s[p.i] == '}') break;
        if (s[p.i] == ',') { p.i++; continue; }
        if (s[p.i] != '"') { fail(NIM_ERR, "the reply was not an answer"); return -1; }
        char key[16]; jstr(&p, key, sizeof key);
        jws(&p);
        if (p.i >= p.n || s[p.i] != ':') break;
        p.i++;
        if (same(key, "kind")) jfield(&p, f.kind, sizeof f.kind);
        else if (same(key, "body")) jfield(&p, f.body, sizeof f.body);
        else if (same(key, "big")) jnum(&p, f.big, sizeof f.big);
        else if (same(key, "unit")) jfield(&p, f.unit, sizeof f.unit);
        else if (same(key, "sub")) jfield(&p, f.sub, sizeof f.sub);
        else if (same(key, "src")) jfield(&p, f.src, sizeof f.src);
        else if (same(key, "url")) uf = jfield(&p, f.url, sizeof f.url);
        else if (same(key, "from")) jnum(&p, f.from, sizeof f.from);
        else if (same(key, "to")) jnum(&p, f.to, sizeof f.to);
        else if (same(key, "fromUnit")) jfield(&p, f.fu, sizeof f.fu);
        else if (same(key, "toUnit")) jfield(&p, f.tu, sizeof f.tu);
        else if (same(key, "value")) jnum(&p, f.value, sizeof f.value);
        else if (same(key, "error")) jfield(&p, f.error, sizeof f.error);
        else if (same(key, "sources")) jsources(&p, "name", srcs_a, &na);
        else if (same(key, "refs")) jsources(&p, "title", srcs_r, &nr);
        else if (!jskip(&p)) break;
    }
    if (f.error[0]) { scopy(nerr, f.error, sizeof nerr); nstate = NIM_ERR; nans[0] = 0; nans_n = 0; nsrc = 0; return -1; }
    /* The answer text, by kind: a text answer is its body, a card its big number and note, a conversion both sides, a sum its value. */
    char *a = nans; unsigned o = 0;
    const char *parts[8]; unsigned np = 0;
    if (f.body[0]) parts[np++] = f.body;
    else if (f.big[0]) { parts[np++] = f.big; if (f.unit[0]) { parts[np++] = " "; parts[np++] = f.unit; } if (f.sub[0]) { parts[np++] = ". "; parts[np++] = f.sub; } }
    else if (f.from[0] && f.to[0]) { parts[np++] = f.from; parts[np++] = " "; parts[np++] = f.fu; parts[np++] = " = "; parts[np++] = f.to; parts[np++] = " "; parts[np++] = f.tu; }
    else if (f.value[0]) parts[np++] = f.value;
    else parts[np++] = "Nimble has no answer for that.";
    for (unsigned k = 0; k < np; k++) for (const char *c = parts[k]; *c && o < ANS_MAX; c++) a[o++] = *c;
    while (o && (a[o - 1] == ' ' || a[o - 1] == '\n')) o--;
    a[o] = 0; nans_n = o;
    nsrc = 0;
    struct nsrc *use = na ? srcs_a : srcs_r; unsigned nu = na ? na : nr;   /* the cited sources first, else the passages it read */
    for (unsigned k = 0; k < nu && nsrc < SRC_MAX; k++, nsrc++) { scopy(nsrcs[nsrc].title, use[k].title, TITLE_MAX); scopy(nsrcs[nsrc].url, use[k].url, URL_MAX); }
    if (!nsrc && f.url[0] && web_url(f.url, uf)) {   /* a card cites its one source */
        scopy(nsrcs[0].title, f.src[0] ? f.src : f.url, TITLE_MAX); scopy(nsrcs[0].url, f.url, URL_MAX); nsrc = 1;
    }
    nstate = NIM_OK;
    return 0;
}

/* ---- The request. ---- */
static const char *hex = "0123456789ABCDEF";
static unsigned put_str(char *o, unsigned n, const char *s) { while (*s) o[n++] = *s++; return n; }
/* The reply for question q, raw, into raw (RAW_MAX). Its length, or -1 with the reason in nerr. */
static int fetch(const char *q, unsigned qn, char *raw) {
    const char *base = NIMBLE_BASE;
    int tls = starts(base, "https://");
    base += tls ? 8 : 7;
    char host[96]; unsigned h = 0, port = tls ? 443 : 80, custom = 0;
    while (base[h] && base[h] != ':' && base[h] != '/' && h < sizeof host - 1) { host[h] = base[h]; h++; }
    host[h] = 0;
    if (base[h] == ':') { port = 0; for (const char *c = base + h + 1; *c >= '0' && *c <= '9'; c++) port = port * 10 + (unsigned)(*c - '0'); custom = 1; }
    static char req[PATH_MAX_LEN + 300];
    unsigned n = put_str(req, 0, "GET /api/answer?q=");
    for (unsigned i = 0; i < qn && n < PATH_MAX_LEN - 4; i++) {   /* a space is +, letters and digits stay, the rest is %XX; stops whole at the budget */
        unsigned char c = (unsigned char)q[i];
        if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '-' || c == '.' || c == '_' || c == '~') req[n++] = (char)c;
        else if (c == ' ') req[n++] = '+';
        else { req[n++] = '%'; req[n++] = hex[c >> 4]; req[n++] = hex[c & 15]; }
    }
    n = put_str(req, n, " HTTP/1.0\r\nHost: "); n = put_str(req, n, host);
    if (custom) { req[n++] = ':'; char d[6]; unsigned k = 0, v = port; do d[k++] = (char)('0' + v % 10); while (v /= 10); while (k) req[n++] = d[--k]; }
    n = put_str(req, n, "\r\nUser-Agent: JoshuaTree/1.0\r\nAccept: application/json\r\nConnection: close\r\n\r\n");
    unsigned ip; int got;
    if (!http_resolve_host(host, &ip)) { scopy(nerr, net_last_error() == NET_ERR_DNS_TIMEOUT ? "dns timeout" : "no such host", sizeof nerr); return -1; }
    if (tls) {
        if (tls_connect(ip, (unsigned short)port, host) < 0) { scopy(nerr, tls_last_error() == TLS_ERR_ENTROPY ? "hardware randomness unavailable" : "connect timeout", sizeof nerr); return -1; }
        if (tls_handshake() < 0) {
            int e = tls_last_error();
            scopy(nerr, e == TLS_ERR_TIMEOUT ? "tls timeout" : e == TLS_ERR_CONNECT ? "tls connection closed" : e == 62 ? "tls error 62 (not trusted)" : e == 56 ? "tls error 56 (wrong host name)" : "tls error", sizeof nerr);
            return -1;
        }
        got = tls_exchange_timeout(req, n, raw, RAW_MAX, REPLY_TICKS);
        if (got < 0 && tls_last_error() > 0) { scopy(nerr, "tls error", sizeof nerr); return -1; }
    } else got = tcp_get_timeout(ip, (unsigned short)port, req, n, raw, RAW_MAX, REPLY_TICKS);
    if (got <= 0) {
        int e = net_last_error();
        scopy(nerr, e == NET_ERR_CONNECT_TIMEOUT ? "connect timeout" : e == NET_ERR_REPLY_TIMEOUT ? "reply timeout" : got == 0 ? "empty reply" : "no reply", sizeof nerr);
        return -1;
    }
    return got;
}

/* Asks Nimble. 1 when there is an answer (nimble_answer, nimble_source), 0 when there is not (nimble_state and nimble_error say why). */
int nimble_ask(const char *q, unsigned n) {
    nasks++;
    nans[0] = 0; nans_n = 0; nsrc = 0; nerr[0] = 0;
    if (n > Q_MAX) n = Q_MAX;
    while (n && q[n - 1] == ' ') n--;
    if (!n) { fail(NIM_ERR, "ask a question first"); return 0; }
    if (!net_get_gateway()) { fail(NIM_NONET, "no network"); return 0; }
    unsigned long mark = heap_mark();
    char *raw = kmalloc(RAW_MAX + 1);
    if (!raw) { heap_release(mark); fail(NIM_ERR, "out of memory"); return 0; }
    unsigned dns_was = net_dns_wait_ticks, connect_was = net_connect_wait_ticks;
    net_dns_wait_ticks = DNS_TICKS; net_connect_wait_ticks = CONNECT_TICKS;
    int got = fetch(q, n, raw);
    net_dns_wait_ticks = dns_was; net_connect_wait_ticks = connect_was;
    int ok = 0;
    if (got < 0) { char why[sizeof nerr]; scopy(why, nerr, sizeof why); fail(NIM_ERR, why); }
    else if ((unsigned)got >= RAW_MAX) fail(NIM_ERR, "the reply was too big");
    else {
        raw[got] = 0;
        int status = http_status_of(raw, (unsigned)got), b = http_body_start(raw, (unsigned)got);
        if (b < 0) b = got;
        if (status == 200) ok = parse(raw + b, (unsigned)(got - b)) == 0;
        else {   /* the API's own reason, if it sent one: {"error": "..."} */
            char api[sizeof nerr], why[sizeof nerr] = "the service answered ";
            unsigned o = slen(why), v = status > 0 ? (unsigned)status : 0, k = 0; char d[6];
            api[0] = 0;
            if (status >= 400 && parse(raw + b, (unsigned)(got - b)) != 0) scopy(api, f.error, sizeof api);
            do d[k++] = (char)('0' + v % 10); while (v /= 10); while (k && o + 1 < sizeof why) why[o++] = d[--k];
            why[o] = 0;
            if (api[0]) { for (const char *c = ": "; *c && o + 1 < sizeof why; c++) why[o++] = *c; for (const char *c = api; *c && o + 1 < sizeof why; c++) why[o++] = *c; why[o] = 0; }
            if (status < 100) scopy(why, "the reply was not an answer", sizeof why);
            fail(NIM_ERR, why);
        }
    }
    heap_release(mark);
    return ok;
}

/* ---- What the callers read. ---- */
int nimble_state(void) { return nstate; }
const char *nimble_error(void) { return nerr; }
const char *nimble_answer(unsigned *len) { *len = nans_n; return nans; }
unsigned nimble_sources(void) { return nsrc; }
const char *nimble_source_title(unsigned i) { return i < nsrc ? nsrcs[i].title : ""; }
const char *nimble_source_url(unsigned i) { return i < nsrc ? nsrcs[i].url : ""; }
unsigned nimble_asks(void) { return nasks; }   /* the window sees a new answer by this changing */

/* Ask and print: the answer wrapped to the Terminal, then the sources as `[N] title` and their address. Samantha's plugin
   runs this too, and says_wrapped is what her result turn is made of. */
void nimble_say(const char *q, unsigned n) {
    kputs("nimble: asking\n");
    if (!nimble_ask(q, n)) { say_wrapped("nimble: ", nerr, slen(nerr)); return; }
    say_wrapped("", nans, nans_n);
    for (unsigned k = 0; k < nsrc; k++) {
        char tag[8] = "[0] "; tag[1] = (char)('1' + k);
        say_wrapped(tag, nsrcs[k].title, slen(nsrcs[k].title));
        say_wrapped("    ", nsrcs[k].url, slen(nsrcs[k].url));
    }
}
/* The Terminal's `nimble QUESTION`. 1 when q was that command. */
int nimble_command(const char *q, unsigned n) {
    if (n < 6 || !starts(q, "nimble") || (n > 6 && q[6] != ' ')) return 0;
    unsigned s = 6; while (s < n && q[s] == ' ') s++;
    if (s == n) { kputs("usage: nimble QUESTION\n"); return 1; }
    nimble_say(q + s, n - s);
    return 1;
}
