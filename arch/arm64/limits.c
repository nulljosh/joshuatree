/* Parental controls (12.0), as in Screen Time. An admin limits another account; JT/LIMITS.TXT keeps it, one line each:
       NAME apps=notes,calendar hours=7-20 web=0
   apps: only these open (the names Spotlight shows, any case; Files and Burrow are the Finder, Browser is Blackcomb, Hamurapi
   is Hamurabi). No apps= means every app. hours=F-T: apps open from hour F to before hour T, by the Pi's own clock; a window
   that wraps midnight (20-7) works. While the clock is unknown (no network yet) the hours are not applied, and the UART
   says so. web=0: Blackcomb loads nothing. Admins are never limited, and an admin's line is ignored. The file is untrusted:
   a line for a name that is no account is dropped, a bad value is read as no limit. A change goes to the card first and is
   kept in memory only if the write worked, as sudo.c does. */
#include "limits.h"
#include "accounts.h"
#include "sudo.h"
#include "login.h"
#include "emmc.h"
#include "../../drivers/fat.h"

#define LIM_FILE "LIMITS.TXT"
void uart_puts(const char *s); void uart_putc(char c);
void menubar_notify(const char *message);   /* desk.c: the one-line notice in the menu bar */

struct lim { char name[ACC_NAME_MAX + 1]; char apps[LIM_APPS_MAX + 1]; int from, to, web; };
static struct lim lim[ACC_USERS_MAX];
static unsigned nlim;

static int same(const char *a, const char *b) { while (*a && *a == *b) { a++; b++; } return *a == *b; }
static void copy(char *d, const char *s, unsigned max) { unsigned i = 0; for (; s[i] && i < max; i++) d[i] = s[i]; d[i] = 0; }
static int at(const char *name) { for (unsigned i = 0; i < nlim; i++) if (same(lim[i].name, name)) return (int)i; return -1; }
static char low(char c) { return c >= 'A' && c <= 'Z' ? (char)(c + 32) : c; }
static int num(const char *s, int *v) { int n = 0, k = 0; while (s[k] >= '0' && s[k] <= '9' && k < 3) n = n * 10 + s[k++] - '0'; *v = n; return k; }

/* "7-20" into from and to; 0 when it is not two hours 0-24 with from != to */
static int hours(const char *s, int *from, int *to) {
    int f, t, k = num(s, &f);
    if (!k || s[k] != '-') return 0;
    int k2 = num(s + k + 1, &t);
    if (!k2 || s[k + 1 + k2] || f > 24 || t > 24 || f == t) return 0;
    *from = f % 24; *to = t % 24; return 1;
}
/* one app name, lower case, its alias resolved: Files and Burrow are the Finder, Browser is Blackcomb, Hamurapi Hamurabi */
static void canon(const char *s, unsigned n, char out[24]) {
    unsigned k = 0; for (; k < n && k < 23; k++) out[k] = low(s[k]); out[k] = 0;
    static const char *const alias[][2] = { {"files", "finder"}, {"burrow", "finder"}, {"browser", "blackcomb"}, {"hamurapi", "hamurabi"} };
    for (unsigned a = 0; a < 4; a++) if (same(out, alias[a][0])) copy(out, alias[a][1], 23);
}
static int listed(const char *apps, const char *app) {
    char want[24], got[24]; unsigned n = 0; while (app[n]) n++;
    canon(app, n, want);
    for (const char *p = apps; *p;) {
        unsigned m = 0; while (p[m] && p[m] != ',') m++;
        canon(p, m, got);
        if (m && same(got, want)) return 1;
        p += m; if (*p) p++;
    }
    return 0;
}
static int apps_ok(const char *s) {   /* a,b,c of a-z 0-9 and -, no empty name, at most LIM_APPS_MAX */
    unsigned n = 0;
    for (; s[n]; n++) { char c = low(s[n]); if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == ',')) return 0; if (c == ',' && (!n || s[n + 1] == ',' || !s[n + 1])) return 0; }
    return n && n <= LIM_APPS_MAX;
}

static void parse_line(const char *s, unsigned n) {
    struct lim l = { "", "", -1, -1, 1 };
    unsigned i = 0;
    while (i < n && s[i] != ' ') i++;
    if (!i || i > ACC_NAME_MAX) return;
    for (unsigned k = 0; k < i; k++) l.name[k] = s[k];
    l.name[i] = 0;
    if (!accounts_has(l.name) || sudo_is_admin(l.name) || at(l.name) >= 0 || nlim >= ACC_USERS_MAX) return;
    while (i < n) {
        while (i < n && s[i] == ' ') i++;
        unsigned b = i; while (i < n && s[i] != ' ') i++;
        char w[LIM_APPS_MAX + 16]; unsigned m = i - b < sizeof w - 1 ? i - b : sizeof w - 1;
        for (unsigned k = 0; k < m; k++) w[k] = s[b + k];
        w[m] = 0;
        if (m > 5 && w[0] == 'a' && w[1] == 'p' && w[2] == 'p' && w[3] == 's' && w[4] == '=' && apps_ok(w + 5)) copy(l.apps, w + 5, LIM_APPS_MAX);
        else if (m > 6 && w[0] == 'h' && w[1] == 'o' && w[2] == 'u' && w[3] == 'r' && w[4] == 's' && w[5] == '=') { if (!hours(w + 6, &l.from, &l.to)) l.from = l.to = -1; }
        else if (m == 5 && same(w, "web=0")) l.web = 0;
    }
    lim[nlim++] = l;
}
void limits_load(void) {
    static char buf[ACC_USERS_MAX * (ACC_NAME_MAX + LIM_APPS_MAX + 32)];
    nlim = 0;
    int got = 0;
    if (sd_ready() && sd_enter_jt(0)) { got = fat_read_file(LIM_FILE, buf, sizeof buf); fat_chdir(".."); }
    for (int i = 0; i < got;) {
        int s = i; while (i < got && buf[i] != '\n') i++;
        int e = i; if (i < got) i++;
        if (e > s && buf[e - 1] == '\r') e--;
        parse_line(buf + s, (unsigned)(e - s));
    }
    uart_puts("limits: loaded "); uart_putc((char)('0' + nlim)); uart_putc('\n');
}

static int put(char *o, unsigned k, unsigned cap, const char *s) { while (*s && k + 1 < cap) o[k++] = *s++; o[k] = 0; return (int)k; }
static int put_n(char *o, unsigned k, unsigned cap, int v) { char t[4] = {0}; int m = 0; do { t[m++] = (char)('0' + v % 10); v /= 10; } while (v && m < 3); while (m && k + 1 < cap) o[k++] = t[--m]; o[k] = 0; return (int)k; }
int limits_line(unsigned i, char *o, unsigned cap) {
    if (i >= nlim || cap < 8) return 0;
    const struct lim *l = &lim[i]; unsigned k = (unsigned)put(o, 0, cap, l->name);
    if (l->apps[0]) { k = (unsigned)put(o, k, cap, " apps="); k = (unsigned)put(o, k, cap, l->apps); }
    if (l->from >= 0) { k = (unsigned)put(o, k, cap, " hours="); k = (unsigned)put_n(o, k, cap, l->from); k = (unsigned)put(o, k, cap, "-"); k = (unsigned)put_n(o, k, cap, l->to); }
    if (!l->web) k = (unsigned)put(o, k, cap, " web=0");
    return (int)k;
}
static int store(const struct lim *t, unsigned n) {
    static char buf[ACC_USERS_MAX * (ACC_NAME_MAX + LIM_APPS_MAX + 32)];
    if (!sd_ready()) return ACC_E_NOCARD;
    struct lim keep[ACC_USERS_MAX]; unsigned kept = nlim;
    for (unsigned i = 0; i < nlim; i++) keep[i] = lim[i];
    for (unsigned i = 0; i < n; i++) lim[i] = t[i];
    nlim = n;
    unsigned k = 0;
    for (unsigned i = 0; i < n; i++) {
        if (!lim[i].apps[0] && lim[i].from < 0 && lim[i].web) continue;   /* nothing limited: no line */
        k += (unsigned)limits_line(i, buf + k, sizeof buf - k - 1); buf[k++] = '\n';
    }
    if (!k) buf[k++] = '\n';
    if (sd_write(LIM_FILE, buf, k) != 1) { for (unsigned i = 0; i < kept; i++) lim[i] = keep[i]; nlim = kept; return ACC_E_WRITE; }
    return 0;
}
int limits_set(const char *name, const char *field, const char *value) {
    if (!accounts_has(name)) return ACC_E_NOUSER;
    if (sudo_is_admin(name)) return LIM_E_ADMIN;
    struct lim t[ACC_USERS_MAX]; unsigned n = nlim;
    for (unsigned i = 0; i < nlim; i++) t[i] = lim[i];
    int i = at(name);
    if (i < 0) { if (n >= ACC_USERS_MAX) return ACC_E_FULL; i = (int)n++; copy(t[i].name, name, ACC_NAME_MAX); t[i].apps[0] = 0; t[i].from = t[i].to = -1; t[i].web = 1; }
    struct lim *l = &t[i];
    if (same(field, "clear")) { l->apps[0] = 0; l->from = l->to = -1; l->web = 1; }
    else if (same(field, "apps")) { if (same(value, "all")) l->apps[0] = 0; else if (apps_ok(value)) { unsigned k = 0; for (; value[k] && k < LIM_APPS_MAX; k++) l->apps[k] = low(value[k]); l->apps[k] = 0; } else return LIM_E_VALUE; }
    else if (same(field, "hours")) { if (same(value, "any")) l->from = l->to = -1; else if (!hours(value, &l->from, &l->to)) return LIM_E_VALUE; }
    else if (same(field, "web")) { if (same(value, "off")) l->web = 0; else if (same(value, "on")) l->web = 1; else return LIM_E_VALUE; }
    else return LIM_E_VALUE;
    return store(t, n);
}

static const struct lim *mine(void) {
    if (!accounts_count() || !current_user[0] || sudo_is_admin(current_user)) return 0;
    int i = at(current_user); return i < 0 ? 0 : &lim[i];
}
static void refuse(const char *what, const char *why) {
    static char line[96]; unsigned k = (unsigned)put(line, 0, sizeof line, what); k = (unsigned)put(line, k, sizeof line, why);
    uart_puts("limits: "); uart_puts(line); uart_putc('\n');
    menubar_notify(line);
}
int limits_app_ok(const char *app) {
    const struct lim *l = mine();
    if (!l) return 1;
    if (l->apps[0] && !listed(l->apps, app)) { refuse(app, " is not allowed for this account"); return 0; }
    if (l->from >= 0) {
        int h = limits_hour();
        if (h < 0) { uart_puts("limits: the clock is not set yet, so the hours are not applied\n"); return 1; }
        int in = l->from < l->to ? h >= l->from && h < l->to : h >= l->from || h < l->to;
        if (!in) { refuse(app, " is outside this account's hours"); return 0; }
    }
    return 1;
}
int limits_web_ok(void) {
    const struct lim *l = mine();
    if (l && !l->web) { refuse("The web", " is off for this account"); return 0; }
    return 1;
}
