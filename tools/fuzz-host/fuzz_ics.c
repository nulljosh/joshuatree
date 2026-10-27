/* Fuzzes arch/arm64/ics.c's parsers on the host under ASan and UBSan, run by tools/checks/ics-fuzz-check.sh.
   These read bytes from servers anyone can run: an iCalendar feed (`calendar subscribe`, parse), a CalDAV REPORT's
   multistatus (dav_feed, jt_days) and a PROPFIND's (dav_tag, dav_href, is_cal), on a kernel with no memory protection.
   ics.c is included whole, so its static functions are reachable, with the kernel it calls stubbed out below. Each sample
   is truncated at every length, then mutated a few thousand times from a fixed seed; every input is copied into a
   buffer of exactly its size, so a read one byte past the end is a sanitizer report. A hang is caught by alarm(1).
   Seconds, no QEMU. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <setjmp.h>
#include <unistd.h>

/* The kernel ics.c calls, as stubs: no card, no network, no keychain. */
static void out_null(const char *s) { (void)s; }
void (*cmd_out)(const char *s) = out_null;
void cmd_dec(unsigned v) { (void)v; }
unsigned long heap_mark(void) { return 0; }
void heap_release(unsigned long m) { (void)m; }
int browser_get(const char *url, char **body, unsigned *n) { (void)url; *body = 0; *n = 0; return 0; }
unsigned long net_clock_utc(void) { return 1793000000UL; }   /* 2026-10-27 */
int home_active(void) { return 0; }
int home_save(const char *name, const void *data, unsigned len) { (void)name; (void)data; (void)len; return 1; }
int sd_write(const char *name, const void *data, unsigned len) { (void)name; (void)data; (void)len; return 1; }
char current_user[40];
void accounts_wipe(void *p, unsigned n) { memset(p, 0, n); }
int keychain_get(const char *item, char *out, unsigned max) { (void)item; (void)out; (void)max; return 0; }
int keychain_set(const char *item, const char *value) { (void)item; (void)value; return 1; }
void ask_caldav_password(const char *user) { (void)user; }
int home_expand(const char *in, char *out, unsigned cap) { (void)in; (void)out; (void)cap; return 0; }
int home_walk(char *path, int keep, char **leaf) { (void)path; (void)keep; (void)leaf; return 0; }
void home_back(void) {}
int sd_enter_jt(int make) { (void)make; return 0; }
int fat_read_file(const char *name, void *buf, unsigned bufsize) { (void)name; (void)buf; (void)bufsize; return -1; }
void fat_cwd_set(unsigned int cluster) { (void)cluster; }
int fat_chdir(const char *name) { (void)name; return 0; }
unsigned int ticks(void) { return 0; }
void uart_puts(const char *s) { (void)s; }
int con_quiet;
void *kmalloc(unsigned int n) { return malloc(n ? n : 1); }
int http_resolve_host(const char *host, unsigned int *ip) { (void)host; *ip = 0; return 0; }
int http_status_of(const char *raw, unsigned int total) { (void)raw; (void)total; return 0; }
int http_body_start(const char *raw, unsigned int total) { (void)raw; (void)total; return -1; }
int https_fetch_timeout(const char *host, unsigned short port, const void *request, unsigned request_len, char *out, unsigned max, unsigned reply_ticks) {
    (void)host; (void)port; (void)request; (void)request_len; (void)out; (void)max; (void)reply_ticks; return -1;
}

#include "../../arch/arm64/ics.c"

static const char FEED[] =
    "BEGIN:VCALENDAR\r\nVERSION:2.0\r\nBEGIN:VEVENT\r\nUID:a\r\nDTSTART;VALUE=DATE:20261101\r\nSUMMARY:Dentist\\, 10am\r\n"
    " folded on\r\nEND:VEVENT\r\nBEGIN:VEVENT\r\nDTSTART;TZID=America/Vancouver:20261105T170000\r\nSUMMARY:Q&A | team\\n"
    "\r\nEND:VEVENT\r\nBEGIN:VEVENT\r\nDTSTART:2026110\r\nEND:VEVENT\r\nEND:VCALENDAR\r\n";
static const char REPORT[] =
    "<?xml version=\"1.0\"?><d:multistatus xmlns:d=\"DAV:\" xmlns:cal=\"urn:ietf:params:xml:ns:caldav\"><d:response>"
    "<d:href>/cal/jt-20261101.ics</d:href><d:propstat><d:prop><cal:calendar-data>BEGIN:VCALENDAR&#13;\nBEGIN:VEVENT&#13;\n"
    "DTSTART;VALUE=DATE:20261101&#13;\nSUMMARY:Q&amp;A &lt;b&gt;&#13;\nEND:VEVENT&#13;\nEND:VCALENDAR</cal:calendar-data>"
    "</d:prop></d:propstat></d:response><d:response><d:href>/cal/b.ics</d:href><d:propstat><d:prop><cal:calendar-data>"
    "<![CDATA[BEGIN:VEVENT\r\nDTSTART:20261109T170000Z\r\nSUMMARY:Design\r\nEND:VEVENT]]></cal:calendar-data><cal:calendar-data/>"
    "</d:prop></d:propstat></d:response></d:multistatus>";
static const char PROPFIND[] =
    "<?xml version=\"1.0\"?><d:multistatus xmlns:d=\"DAV:\" xmlns:c=\"urn:ietf:params:xml:ns:caldav\"><d:response><d:href>/cal/</d:href>"
    "<d:propstat><d:prop><d:resourcetype><d:collection/></d:resourcetype><d:current-user-principal><d:href>/p/j/</d:href>"
    "</d:current-user-principal><c:calendar-home-set/></d:prop></d:propstat></d:response><d:response><d:href>/cal/work/</d:href>"
    "<d:propstat><d:prop><d:resourcetype><d:collection/><c:calendar/></d:resourcetype><c:supported-calendar-component-set>"
    "<c:comp name=\"VEVENT\"/></c:supported-calendar-component-set><d:displayname>Work</d:displayname></d:prop></d:propstat></d:response></d:multistatus>";

static sigjmp_buf g_jmp;
static long g_cases, g_hangs;
static void on_alarm(int sig) { (void)sig; siglongjmp(g_jmp, 1); }

static void run_all(const char *in, unsigned n) {   /* every parser, each on its own exact-size copy */
    char *b = malloc(n ? n : 1);
    memcpy(b, in, n); nev = 0; parse(b, n, 0);
    memcpy(b, in, n); dav_feed(b, n);
    { unsigned days[DAYS_MAX]; memcpy(b, in, n); jt_days(b, n, days); }
    {
        char href[200]; memcpy(b, in, n);
        dav_href(b, n, "current-user-principal", href, sizeof href);
        dav_href(b, n, "calendar-home-set", href, sizeof href);
        is_cal(b, n);
        for (const char *r = dav_tag(b, n, "response", 0); r; ) {   /* the walk dav_discover does */
            const char *next = dav_tag(b, n, "response", (unsigned)(r - b) + 1);
            unsigned seg = (unsigned)((next ? next : b + n) - r);
            dav_href(r, seg, "response", href, sizeof href);
            dav_tag(r, seg, "displayname", 0);
            is_cal(r, seg);
            r = next;
        }
    }
    for (unsigned i = 0; i < n; i++) day_of(b + i, n - i);
    free(b);
}
static void one(const char *in, unsigned n) {
    g_cases++;
    if (sigsetjmp(g_jmp, 1)) { g_hangs++; fprintf(stderr, "HANG at a %u byte input\n", n); return; }
    alarm(1); run_all(in, n); alarm(0);
}
static unsigned rnd_state = 12345;
static unsigned rnd(void) { rnd_state = rnd_state * 1103515245u + 12345u; return rnd_state >> 8; }
static void fuzz(const char *name, const char *s, unsigned n) {
    long before = g_cases;
    for (unsigned k = 0; k <= n; k++) one(s, k);   /* every truncation */
    static const char nasty[] = "<>&;:/\\\r\n \"'=#x0123456789";
    char *m = malloc(n + 64);
    for (int t = 0; t < 3000; t++) {
        unsigned len = n; memcpy(m, s, n);
        int edits = 1 + (int)(rnd() % 6);
        for (int e = 0; e < edits; e++) {
            unsigned p = len ? rnd() % len : 0, op = rnd() % 4;
            if (op == 0 && len) m[p] = (char)rnd();                                  /* any byte */
            else if (op == 1 && len) m[p] = nasty[rnd() % (sizeof nasty - 1)];       /* a byte the parsers care about */
            else if (op == 2 && len + 1 < n + 64) { memmove(m + p + 1, m + p, len - p); m[p] = nasty[rnd() % (sizeof nasty - 1)]; len++; }
            else if (len) { memmove(m + p, m + p + 1, len - p - 1); len--; }           /* delete */
        }
        one(m, len);
    }
    free(m);
    fprintf(stderr, "ics-fuzz: %s, %ld cases\n", name, g_cases - before);
}
int main(void) {
    signal(SIGALRM, on_alarm);
    fuzz("feed", FEED, sizeof FEED - 1);
    fuzz("report", REPORT, sizeof REPORT - 1);
    fuzz("propfind", PROPFIND, sizeof PROPFIND - 1);
    puts(g_hangs ? "ICS FUZZ: FAIL (hang)" : "ICS FUZZ: PASS");
    return g_hangs != 0;
}
