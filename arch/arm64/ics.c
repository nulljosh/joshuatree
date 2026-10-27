/* Calendar subscriptions (6.19.0): `calendar subscribe URL` reads an iCalendar feed (https://, http://, or webcal://, which
   is https) and writes its events as SUBS.TXT, in EVENTS.TXT's own "YYYY-MM-DD|text" lines, where the Calendar app's files
   live (the home folder, else JT/). Calendar shows them beside the person's own and never saves them back; a date the
   person has an event on keeps theirs. Read only: nothing goes back to the feed. `calendar unsubscribe` empties the file.
   Each VEVENT gives its DTSTART's date (a time and zone are dropped: the day it says is the day shown) and SUMMARY,
   unfolded and unescaped. Events on one day share its line; the 48 soonest from a month ago on are kept, as the app holds
   64 events in all. tools/checks/arm64-ics-check.py serves a feed and reads both the file and the app's log. */
extern void (*cmd_out)(const char *s);   /* cmd.c */
void cmd_dec(unsigned v);
unsigned long heap_mark(void);
void heap_release(unsigned long m);
int browser_get(const char *url, char **body, unsigned *n);   /* browser.c */
unsigned long net_clock_utc(void);                             /* ip.c: 0 until the network set it */
int home_active(void); int home_save(const char *name, const void *data, unsigned len);   /* home.c */
int sd_write(const char *name, const void *data, unsigned len);                            /* emmc.c */

#define SUBS_MAX 48
#define TEXT_MAX 39   /* the app's EVENT_TEXT_MAX less its nul */
static struct { unsigned day; char text[TEXT_MAX + 1]; } ev[SUBS_MAX];   /* day: YYYYMMDD as a number, so it sorts */
static unsigned nev;

static int starts(const char *s, const char *w) { for (; *w; s++, w++) if ((*s | 32) != (*w | 32)) return 0; return 1; }
static unsigned today(void) {   /* YYYYMMDD from the network clock, 0 when there is none */
    unsigned long t = net_clock_utc();
    if (!t) return 0;
    long z = (long)(t / 86400) + 719468, era = z / 146097, doe = z - era * 146097;   /* Howard Hinnant's civil_from_days */
    long yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365, doy = doe - (365 * yoe + yoe / 4 - yoe / 100), mp = (5 * doy + 2) / 153;
    long d = doy - (153 * mp + 2) / 5 + 1, m = mp < 10 ? mp + 3 : mp - 9, y = yoe + era * 400 + (m <= 2);
    return (unsigned)(y * 10000 + m * 100 + d);
}
static unsigned month_back(unsigned d) { unsigned y = d / 10000, m = d / 100 % 100; return m == 1 ? (y - 1) * 10000 + 1200 + d % 100 : d - 100; }
static void add(unsigned day, const char *sum, unsigned sn) {   /* one event: joined to its day's line, or a new day in order */
    unsigned i = 0;
    while (i < nev && ev[i].day < day) i++;
    if (i < nev && ev[i].day == day) {
        unsigned o = 0; while (ev[i].text[o]) o++;
        if (o + 2 < TEXT_MAX) { ev[i].text[o++] = ';'; ev[i].text[o++] = ' '; }
        for (unsigned k = 0; k < sn && o < TEXT_MAX; k++) ev[i].text[o++] = sum[k];
        ev[i].text[o] = 0;
        return;
    }
    if (nev == SUBS_MAX) { if (i == nev) return; nev--; }   /* full: a later day gives way to a sooner one */
    for (unsigned k = nev; k > i; k--) ev[k] = ev[k - 1];
    nev++;
    ev[i].day = day;
    unsigned o = 0; for (unsigned k = 0; k < sn && o < TEXT_MAX; k++) ev[i].text[o++] = sum[k];
    ev[i].text[o] = 0;
}
/* The feed, unfolded line by line: DTSTART and SUMMARY of each VEVENT. The number of events read. */
static unsigned parse(char *f, unsigned n, unsigned from) {
    unsigned w = 0;
    for (unsigned r = 0; r < n; r++) {   /* unfold: CRLF then a space or tab continues the line */
        if (f[r] == '\r') continue;
        if (f[r] == '\n' && r + 1 < n && (f[r + 1] == ' ' || f[r + 1] == '\t')) { r++; continue; }
        f[w++] = f[r];
    }
    unsigned seen = 0, day = 0, in = 0; char sum[TEXT_MAX + 1]; unsigned sn = 0;
    for (unsigned i = 0; i < w;) {
        unsigned e = i; while (e < w && f[e] != '\n') e++;
        const char *l = f + i; unsigned ln = e - i;
        if (ln >= 12 && starts(l, "BEGIN:VEVENT")) { in = 1; day = 0; sn = 0; }
        else if (in && ln >= 10 && starts(l, "END:VEVENT")) {
            in = 0; seen++;
            if (day && day >= from) add(day, sn ? sum : "(no title)", sn ? sn : 10);
        } else if (in && starts(l, "DTSTART")) {
            unsigned c = 7; while (c < ln && l[c] != ':') c++;
            unsigned v = 0, k = 0;
            for (c++; c < ln && k < 8 && l[c] >= '0' && l[c] <= '9'; c++, k++) v = v * 10 + (unsigned)(l[c] - '0');
            if (k == 8 && v / 100 % 100 >= 1 && v / 100 % 100 <= 12 && v % 100 >= 1 && v % 100 <= 31) day = v;
        } else if (in && starts(l, "SUMMARY")) {
            unsigned c = 7; while (c < ln && l[c] != ':') c++;
            sn = 0;
            for (c++; c < ln && sn < TEXT_MAX; c++) {
                char ch = l[c];
                if (ch == '\\' && c + 1 < ln) { c++; ch = l[c] == 'n' || l[c] == 'N' ? ' ' : l[c]; }
                if (ch == '|' || (unsigned char)ch < 0x20) ch = ' ';   /* the file's own separator, and no control bytes */
                sum[sn++] = ch;
            }
        }
        i = e + 1;
    }
    return seen;
}
static int save_as(const char *name, const char *d, unsigned n) { return home_active() ? home_save(name, d, n) == 1 : sd_write(name, d, n) == 1; }
static int save(const char *d, unsigned n) { return save_as("SUBS.TXT", d, n); }

/* A feed's text (or CalDAV's events run together) into SUBS.TXT, with one line saying what was kept. Under a heap mark. */
static void keep(char *body, unsigned bn) {
    unsigned t = today();
    nev = 0;
    unsigned seen = parse(body, bn, t ? month_back(t) : 0);
    void *kmalloc(unsigned int);
    char *out = kmalloc(nev * (11 + TEXT_MAX + 1) + 1); unsigned w = 0;
    if (!out) { cmd_out("calendar: out of memory\n"); return; }
    for (unsigned i = 0; i < nev; i++) {
        unsigned d = ev[i].day;
        char date[11] = { (char)('0' + d / 10000000), (char)('0' + d / 1000000 % 10), (char)('0' + d / 100000 % 10), (char)('0' + d / 10000 % 10), '-',
                          (char)('0' + d / 1000 % 10), (char)('0' + d / 100 % 10), '-', (char)('0' + d / 10 % 10), (char)('0' + d % 10), 0 };
        for (unsigned k = 0; k < 10; k++) out[w++] = date[k];
        out[w++] = '|';
        for (const char *p = ev[i].text; *p; p++) out[w++] = *p;
        out[w++] = '\n';
    }
    if (!save(out, w)) cmd_out("calendar: could not write SUBS.TXT\n");
    else { cmd_out("calendar: "); cmd_dec(seen); cmd_out(seen == 1 ? " event read, " : " events read, "); cmd_dec(nev); cmd_out(nev == 1 ? " day kept in SUBS.TXT\n" : " days kept in SUBS.TXT\n"); }
}

/* ---- 6.24.0: two-way CalDAV ----
   `calendar caldav https://HOST[:PORT]/PATH/ USER`, the calendar collection's own address, asks for an app password with
   sudo's stars (cmd.c's PW_CALDAV). https only: Basic auth over plain http would be the password in the clear. Kept in
   the keychain as "caldav" with an account signed in (memory only with none), wiped when another account is.
   `calendar sync` pushes, then pulls. Push: each of the person's own EVENTS.TXT days goes up as an all-day VEVENT, PUT at
   PATH/jt-YYYYMMDD.ics with the UID jt-YYYYMMDD@joshuatree, so a second sync replaces it instead of doubling it. Pull: one
   REPORT calendar-query for every VEVENT; each calendar-data, its XML escapes undone, is read like a feed into SUBS.TXT.
   6.41.0: deletes sync both ways. CALSYNC.TXT, beside EVENTS.TXT, lists the days both sides had after the last sync. A
   REPORT before the push finds the server's jt- days; a listed day the server no longer has was deleted there and leaves
   EVENTS.TXT, and a listed day EVENTS.TXT no longer has was deleted here and is DELETEd on the server. A day never
   listed is new on whichever side has it. tools/checks/arm64-caldav-check.py runs it against a fixture server. */
#include "tls.h"
#include "../../drivers/http.h"
extern char current_user[];
void accounts_wipe(void *p, unsigned n);
int keychain_get(const char *item, char *out, unsigned max); int keychain_set(const char *item, const char *value);
void ask_caldav_password(const char *user);   /* cmd.c */
int home_expand(const char *in, char *out, unsigned cap); int home_walk(char *path, int keep, char **leaf); void home_back(void);
int sd_enter_jt(int make); int fat_read_file(const char *name, void *buf, unsigned bufsize); void fat_cwd_set(unsigned int cluster); int fat_chdir(const char *name);
static char dav_url[200], dav_user[96], dav_pass[128], dav_owner[40];
static int dav_ready;
static unsigned slen(const char *s) { unsigned n = 0; while (s[n]) n++; return n; }
static int same(const char *a, const char *b) { while (*a && *a == *b) { a++; b++; } return *a == *b; }
static void cp(char *d, const char *s, unsigned n, unsigned max) { if (n >= max) n = max - 1; for (unsigned i = 0; i < n; i++) d[i] = s[i]; d[n] = 0; }
static void dav_forget(void) { accounts_wipe(dav_pass, sizeof dav_pass); dav_ready = 0; dav_owner[0] = 0; }
static int dav_mine(void) {   /* another account or nobody: wiped; then the keychain may hold it */
    if (dav_ready && !same(dav_owner, current_user)) dav_forget();
    if (dav_ready) return 1;
    char v[440]; int k = keychain_get("caldav", v, sizeof v);   /* "url user password" */
    if (k == 1) {
        unsigned i = 0; while (v[i] && v[i] != ' ') i++;
        cp(dav_url, v, i, sizeof dav_url);
        unsigned a = ++i; while (v[i] && v[i] != ' ') i++;
        cp(dav_user, v + a, i - a, sizeof dav_user);
        if (v[i] == ' ') { cp(dav_pass, v + i + 1, slen(v + i + 1), sizeof dav_pass); cp(dav_owner, current_user, slen(current_user), sizeof dav_owner); dav_ready = dav_url[0] && dav_user[0] && dav_pass[0]; }
    }
    accounts_wipe(v, sizeof v);
    return dav_ready;
}
static unsigned b64(const unsigned char *in, unsigned n, char *out) {
    static const char t[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    unsigned o = 0;
    for (unsigned i = 0; i < n; i += 3) {
        unsigned v = (unsigned)in[i] << 16 | (i + 1 < n ? (unsigned)in[i + 1] << 8 : 0) | (i + 2 < n ? in[i + 2] : 0);
        out[o++] = t[v >> 18 & 63]; out[o++] = t[v >> 12 & 63]; out[o++] = i + 1 < n ? t[v >> 6 & 63] : '='; out[o++] = i + 2 < n ? t[v & 63] : '=';
    }
    return o;
}
/* https://HOST[:PORT]/PATH into its parts: 1 when it is one. */
static int dav_parts(const char *url, char *host, unsigned short *port, const char **path) {
    if (!starts(url, "https://")) return 0;
    const char *h = url + 8; unsigned i = 0;
    while (h[i] && h[i] != '/' && h[i] != ':' && i < 127) { host[i] = h[i]; i++; }
    host[i] = 0; *port = 443;
    if (h[i] == ':') { unsigned v = 0; i++; while (h[i] >= '0' && h[i] <= '9' && v < 70000) v = v * 10 + (unsigned)(h[i++] - '0'); if (!v || v > 65535) return 0; *port = (unsigned short)v; }
    *path = h[i] == '/' ? h + i : "/";
    return i > 0;
}
/* One request to the collection's server. The HTTP status (0 when nothing came back); its body in *body. */
static int dav_req(const char *method, const char *path, const char *name, const char *ctype, const char *extra, const char *payload, unsigned pn, char **body, unsigned *bn) {
    char host[128]; unsigned short port; const char *base;
    *body = 0; *bn = 0;
    if (!dav_parts(dav_url, host, &port, &base)) return 0;
    void *kmalloc(unsigned int);
    unsigned cap = 1u << 20;
    char *req = kmalloc(pn + 1024), *out = kmalloc(cap);
    if (!req || !out) return 0;
    unsigned o = 0;
    #define RQ(str) for (const char *p_ = (str); *p_; p_++) req[o++] = *p_
    RQ(method); RQ(" "); RQ(path); if (name) RQ(name);
    RQ(" HTTP/1.0\r\nHost: "); RQ(host); RQ("\r\nAuthorization: Basic ");
    { unsigned char up[230]; unsigned un = 0;
      for (const char *p = dav_user; *p && un < 100; p++) up[un++] = (unsigned char)*p;
      up[un++] = ':'; for (const char *p = dav_pass; *p && un < 228; p++) up[un++] = (unsigned char)*p;
      o += b64(up, un, req + o); accounts_wipe(up, sizeof up); }
    RQ("\r\nUser-Agent: JoshuaTree/1.0\r\nConnection: close\r\n");
    if (extra) RQ(extra);
    if (ctype) { RQ("Content-Type: "); RQ(ctype); RQ("\r\n"); }
    RQ("Content-Length: "); { char d[8]; unsigned z = 0, v = pn; do d[z++] = (char)('0' + v % 10); while (v /= 10); while (z) req[o++] = d[--z]; }
    RQ("\r\n\r\n");
    #undef RQ
    for (unsigned i = 0; i < pn; i++) req[o++] = payload[i];
    int got = https_fetch_timeout(host, port, req, o, out, cap - 1, 1000);
    accounts_wipe(req, o);   /* it carried the password */
    (void)base;
    if (got <= 0) return 0;
    out[got] = 0;
    int b = http_body_start(out, (unsigned)got);
    if (b >= 0) { *body = out + b; *bn = (unsigned)got - (unsigned)b; }
    return http_status_of(out, (unsigned)got);
}
/* Each calendar-data in a multistatus, XML escapes undone, run together into one feed in place. Its length. */
static unsigned dav_feed(char *x, unsigned n) {
    unsigned w = 0;
    for (unsigned i = 0; i + 14 < n; i++) {
        if (!starts(x + i, "calendar-data") || !i) continue;
        unsigned k = i - 1; while (k && x[k] != '<' && x[k] != '/' && x[k] != ' ' && x[k] != '>') k--;
        if (x[k] != '<') continue;                                  /* an opening tag, not a closing one */
        unsigned g = i; while (g < n && x[g] != '>') g++;
        if (g >= n || x[g - 1] == '/') { i = g; continue; }         /* <calendar-data/>, empty */
        unsigned a = g + 1, e = a;
        if (starts(x + a, "<![CDATA[")) { a += 9; e = a; while (e + 2 < n && !(x[e] == ']' && x[e + 1] == ']' && x[e + 2] == '>')) e++; }
        else while (e < n && x[e] != '<') e++;
        for (unsigned r = a; r < e;) {
            if (x[r] == '&') {
                const char *t = x + r; char c = 0; unsigned l = 0;
                if (starts(t, "&lt;")) { c = '<'; l = 4; } else if (starts(t, "&gt;")) { c = '>'; l = 4; } else if (starts(t, "&amp;")) { c = '&'; l = 5; }
                else if (starts(t, "&quot;")) { c = '"'; l = 6; } else if (starts(t, "&apos;")) { c = '\''; l = 6; } else if (starts(t, "&#13;") || starts(t, "&#xd;")) { c = '\r'; l = 5; }
                if (l) { x[w++] = c; r += l; continue; }
            }
            x[w++] = x[r++];
        }
        x[w++] = '\n';
        i = e;
    }
    return w;
}
static int dav_read(const char *name, char *buf, unsigned max) {   /* the person's file, from where Calendar keeps EVENTS.TXT */
    int got = -1;
    if (!sd_enter_jt(0)) return -1;
    if (home_active()) {
        char path[96], *leaf, tilde[24] = "~/"; cp(tilde + 2, name, slen(name), sizeof tilde - 2);
        if (home_expand(tilde, path, sizeof path) > 0 && home_walk(path, 1, &leaf) == 1) { got = fat_read_file(leaf, buf, max); home_back(); }
        fat_cwd_set(0);
        return got;
    }
    got = fat_read_file(name, buf, max);
    fat_chdir("..");
    return got;
}
#define DAYS_MAX 96
static unsigned day_of(const char *l, unsigned ln) {   /* "YYYY-MM-DD|..." as YYYYMMDD, else 0 */
    if (ln <= 11 || l[4] != '-' || l[7] != '-' || l[10] != '|') return 0;
    unsigned v = 0; for (unsigned k = 0; k < 10; k++) if (k != 4 && k != 7) { if (l[k] < '0' || l[k] > '9') return 0; v = v * 10 + (unsigned)(l[k] - '0'); }
    return v;
}
static int has_day(const unsigned *a, unsigned n, unsigned d) { for (unsigned i = 0; i < n; i++) if (a[i] == d) return 1; return 0; }
static unsigned jt_days(const char *x, unsigned n, unsigned *out) {   /* every "jt-YYYYMMDD" in a multistatus: the server's days from the Pi */
    unsigned c = 0;
    for (unsigned i = 0; i + 11 <= n && c < DAYS_MAX; i++) if (x[i] == 'j' && x[i + 1] == 't' && x[i + 2] == '-') {
        unsigned v = 0, k = 0; while (k < 8 && x[i + 3 + k] >= '0' && x[i + 3 + k] <= '9') v = v * 10 + (unsigned)(x[i + 3 + k++] - '0');
        if (k == 8 && (i + 11 == n || x[i + 11] < '0' || x[i + 11] > '9') && !has_day(out, c, v)) out[c++] = v;
    }
    return c;
}
static void dav_sync(void) {
    unsigned long mark = heap_mark();
    char host[128]; unsigned short port; const char *base;
    dav_parts(dav_url, host, &port, &base);
    void *kmalloc(unsigned int);
    char *evs = kmalloc(8192); unsigned sent = 0;
    static unsigned put_ok[DAYS_MAX];   /* 6.41.0: the days the server took this time: only they count as synced */
    int en = evs ? dav_read("EVENTS.TXT", evs, 8191) : -1;
    static const char q[] = "<?xml version=\"1.0\" encoding=\"utf-8\"?><c:calendar-query xmlns:d=\"DAV:\" xmlns:c=\"urn:ietf:params:xml:ns:caldav\">"
                            "<d:prop><c:calendar-data/></d:prop><c:filter><c:comp-filter name=\"VCALENDAR\"><c:comp-filter name=\"VEVENT\"/></c:comp-filter></c:filter></c:calendar-query>";
    char *rb; unsigned rn;
    {   /* 6.41.0: deletes, from the last sync's days (CALSYNC.TXT) against both sides now */
        static unsigned last[DAYS_MAX], there[DAYS_MAX]; unsigned nl = 0, nt = 0, gone_here = 0, gone_there = 0;
        char sb[DAYS_MAX * 9 + 1]; int sn = dav_read("CALSYNC.TXT", sb, sizeof sb - 1);
        for (int i = 0; sn > 0 && i + 8 <= sn && nl < DAYS_MAX; i += 9) { unsigned v = 0; for (int k = 0; k < 8; k++) v = v * 10 + (unsigned)(sb[i + k] - '0'); last[nl++] = v; }
        if (nl) {
            int st = dav_req("REPORT", base, 0, "application/xml; charset=utf-8", "Depth: 1\r\n", q, sizeof q - 1, &rb, &rn);
            if (st == 401 || st == 403) { cmd_out("calendar: the server refused the user or the app password\n"); heap_release(mark); return; }
            if (st != 207 || !rb) nl = 0;   /* no answer: delete nothing either way */
            else nt = jt_days(rb, rn, there);
        }
        unsigned here[DAYS_MAX], nh = 0;
        for (unsigned i = 0; en > 0 && i < (unsigned)en && nh < DAYS_MAX;) { unsigned e = i; while (e < (unsigned)en && evs[e] != '\n') e++; unsigned d = day_of(evs + i, e - i); if (d) here[nh++] = d; i = e + 1; }
        for (unsigned k = 0; k < nl; k++) {
            int on_here = has_day(here, nh, last[k]), on_there = has_day(there, nt, last[k]);
            if (on_here && !on_there) {   /* deleted on the server: out of EVENTS.TXT */
                unsigned w = 0;
                for (unsigned i = 0; i < (unsigned)en;) { unsigned e = i; while (e < (unsigned)en && evs[e] != '\n') e++; unsigned ee = e < (unsigned)en ? e + 1 : e; if (day_of(evs + i, e - i) != last[k]) for (unsigned c = i; c < ee; c++) evs[w++] = evs[c]; i = ee; }
                en = (int)w; gone_here++;
            } else if (!on_here && on_there) {   /* deleted here: DELETE on the server */
                char name[24] = "jt-"; unsigned v = last[k];
                for (int c = 10; c >= 3; c--) { name[c] = (char)('0' + v % 10); v /= 10; }
                cp(name + 11, ".ics", 4, sizeof name - 11);
                char *db; unsigned dn;
                int st = dav_req("DELETE", base, name, 0, 0, "", 0, &db, &dn);
                if (st == 200 || st == 204 || st == 404) gone_there++;
                else { cmd_out("calendar: the server did not delete "); cmd_out(name); cmd_out("\n"); }
            }
        }
        if (gone_here) save_as("EVENTS.TXT", evs, (unsigned)en);
        if (gone_here) { cmd_out("calendar: "); cmd_dec(gone_here); cmd_out(gone_here == 1 ? " day deleted on the server, taken off here\n" : " days deleted on the server, taken off here\n"); }
        if (gone_there) { cmd_out("calendar: "); cmd_dec(gone_there); cmd_out(gone_there == 1 ? " day deleted here, taken off the server\n" : " days deleted here, taken off the server\n"); }
    }
    for (unsigned i = 0; en > 0 && i < (unsigned)en;) {   /* push: every "YYYY-MM-DD|text" line */
        unsigned e = i; while (e < (unsigned)en && evs[e] != '\n') e++;
        if (e - i > 11 && evs[i + 4] == '-' && evs[i + 7] == '-' && evs[i + 10] == '|') {
            char d8[9] = { evs[i], evs[i + 1], evs[i + 2], evs[i + 3], evs[i + 5], evs[i + 6], evs[i + 8], evs[i + 9], 0 };
            char ics[512], name[24]; unsigned o = 0, no = 0;
            for (const char *p = "jt-"; *p; p++) name[no++] = *p;
            for (int k = 0; k < 8; k++) name[no++] = d8[k];
            for (const char *p = ".ics"; *p; p++) name[no++] = *p;
            name[no] = 0;
            #define IC(str) for (const char *p_ = (str); *p_ && o < sizeof ics - 8; p_++) ics[o++] = *p_
            IC("BEGIN:VCALENDAR\r\nVERSION:2.0\r\nPRODID:-//Joshua Tree//Calendar//EN\r\nBEGIN:VEVENT\r\nUID:jt-"); IC(d8);
            IC("@joshuatree\r\nDTSTAMP:"); IC(d8); IC("T000000Z\r\nDTSTART;VALUE=DATE:"); IC(d8); IC("\r\nSUMMARY:");
            for (unsigned k = i + 11; k < e && o < sizeof ics - 64; k++) {   /* iCalendar text escapes */
                char c = evs[k];
                if (c == '\r') continue;
                if (c == ',' || c == ';' || c == '\\') ics[o++] = '\\';
                ics[o++] = c;
            }
            IC("\r\nEND:VEVENT\r\nEND:VCALENDAR\r\n");
            #undef IC
            char *rb; unsigned rn;
            int st = dav_req("PUT", base, name, "text/calendar; charset=utf-8", 0, ics, o, &rb, &rn);
            if (st == 401 || st == 403) { cmd_out("calendar: the server refused the user or the app password\n"); heap_release(mark); return; }
            if (st == 201 || st == 204 || st == 200) { if (sent < DAYS_MAX) put_ok[sent] = day_of(evs + i, e - i); sent++; }
            else { cmd_out("calendar: the server did not take "); cmd_out(name); cmd_out("\n"); }
        }
        i = e + 1;
    }
    {   /* 6.41.0: what both sides now have (a day the server refused is not one), for the next sync's deletes */
        char sb[DAYS_MAX * 9 + 1]; unsigned o = 0;
        for (unsigned k = 0; k < sent && k < DAYS_MAX && o + 9 < sizeof sb; k++) {
            unsigned d = put_ok[k];
            if (!d || has_day(put_ok, k, d)) continue;
            for (int c = 7; c >= 0; c--) { sb[o + (unsigned)c] = (char)('0' + d % 10); d /= 10; } sb[o + 8] = '\n'; o += 9;
        }
        save_as("CALSYNC.TXT", sb, o);
    }
    int st = dav_req("REPORT", base, 0, "application/xml; charset=utf-8", "Depth: 1\r\n", q, sizeof q - 1, &rb, &rn);
    cmd_out("calendar: "); cmd_dec(sent); cmd_out(" of your days sent\n");
    if (st == 401 || st == 403) cmd_out("calendar: the server refused the user or the app password\n");
    else if (st != 207 || !rb) { cmd_out("calendar: the calendar did not answer"); if (st > 0) { cmd_out(", the server said "); cmd_dec((unsigned)st); } cmd_out("\n"); }
    else keep(rb, dav_feed(rb, rn));
    heap_release(mark);
}
/* cmd.c, once the app password is typed. */
void ics_password(const char *pass) {
    cp(dav_pass, pass, slen(pass), sizeof dav_pass);
    cp(dav_owner, current_user, slen(current_user), sizeof dav_owner);
    dav_ready = 1;
    char v[440]; unsigned o = 0;
    for (const char *p = dav_url; *p && o < 200; p++) v[o++] = *p;
    v[o++] = ' '; for (const char *p = dav_user; *p && o < 300; p++) v[o++] = *p;
    v[o++] = ' '; for (const char *p = dav_pass; *p && o < sizeof v - 1; p++) v[o++] = *p;
    v[o] = 0;
    int k = keychain_set("caldav", v);
    accounts_wipe(v, sizeof v);
    cmd_out(k == 1 ? "calendar: CalDAV kept in your keychain; calendar sync sends and fetches\n" : "calendar: CalDAV set until a restart; calendar sync sends and fetches\n");
}

int ics_command(const char *q, unsigned n) {   /* calendar subscribe URL, calendar unsubscribe; 1 when it was one */
    if (n == 20 && starts(q, "calendar unsubscribe")) {
        cmd_out(save("", 0) ? "calendar: unsubscribed, SUBS.TXT emptied\n" : "calendar: could not write SUBS.TXT\n");
        return 1;
    }
    if (n == 13 && starts(q, "calendar sync")) {
        dav_mine();
        if (!dav_ready) cmd_out("calendar: no CalDAV set, use calendar caldav https://.../ USER\n"); else dav_sync();
        return 1;
    }
    if (n > 16 && starts(q, "calendar caldav ")) {
        const char *a = q + 16; unsigned an = n - 16, i = 0;
        while (i < an && a[i] != ' ') i++;
        unsigned u0 = i; while (u0 < an && a[u0] == ' ') u0++;
        char host[128]; unsigned short port; const char *path;
        if (i >= sizeof dav_url || u0 == an || an - u0 >= sizeof dav_user) { cmd_out("calendar: use calendar caldav https://HOST/PATH/ USER\n"); return 1; }
        dav_forget(); cp(dav_url, a, i, sizeof dav_url);
        if (!dav_parts(dav_url, host, &port, &path)) { cmd_out("calendar: CalDAV takes https only, the app password must not go in the clear\n"); dav_url[0] = 0; return 1; }
        cp(dav_user, a + u0, an - u0, sizeof dav_user);
        ask_caldav_password(dav_user);
        return 1;
    }
    if (n < 19 || !starts(q, "calendar subscribe ")) return 0;
    const char *u = q + 19; unsigned un = n - 19;
    while (un && *u == ' ') { u++; un--; }
    char url[256]; unsigned o = 0;
    if (un > 9 && starts(u, "webcal://")) { for (const char *p = "https://"; *p; p++) url[o++] = *p; u += 9; un -= 9; }
    else if (!(starts(u, "https://") || starts(u, "http://"))) { cmd_out("calendar: use calendar subscribe https://... (or webcal://)\n"); return 1; }
    if (o + un >= sizeof url) { cmd_out("calendar: that address is too long\n"); return 1; }
    for (unsigned i = 0; i < un; i++) url[o++] = u[i];
    url[o] = 0;
    unsigned long mark = heap_mark();
    char *body; unsigned bn;
    int st = browser_get(url, &body, &bn);
    if (st != 200 || !body) { cmd_out("calendar: the feed did not load"); if (st > 0) { cmd_out(", the server said "); cmd_dec((unsigned)st); } cmd_out("\n"); heap_release(mark); return 1; }
    if (bn < 15 || !starts(body, "BEGIN:VCALENDAR")) { cmd_out("calendar: that is not an iCalendar feed\n"); heap_release(mark); return 1; }
    keep(body, bn);
    heap_release(mark);
    return 1;
}
