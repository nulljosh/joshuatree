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
static int save(const char *d, unsigned n) { return home_active() ? home_save("SUBS.TXT", d, n) == 1 : sd_write("SUBS.TXT", d, n) == 1; }

int ics_command(const char *q, unsigned n) {   /* calendar subscribe URL, calendar unsubscribe; 1 when it was one */
    if (n == 20 && starts(q, "calendar unsubscribe")) {
        cmd_out(save("", 0) ? "calendar: unsubscribed, SUBS.TXT emptied\n" : "calendar: could not write SUBS.TXT\n");
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
    unsigned t = today();
    nev = 0;
    unsigned seen = parse(body, bn, t ? month_back(t) : 0);
    void *kmalloc(unsigned int);
    char *out = kmalloc(nev * (11 + TEXT_MAX + 1) + 1); unsigned w = 0;
    if (!out) { cmd_out("calendar: out of memory\n"); heap_release(mark); return 1; }
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
    heap_release(mark);
    return 1;
}
