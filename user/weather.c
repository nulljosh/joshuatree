/* weather: the current reading and a five-day forecast, as a real ring-3
 * program.
 *
 * The twentieth app to leave the kernel (roadmap 2.0). The network side did
 * not move: the kernel's weather_fetch still owns the location lookup, the
 * Open-Meteo call, the menu bar text, the wind sway, phone home and
 * Samantha's weather tool. After every fetch, good or not, it writes
 * WEATHER.TXT, and this program only reads that file through plain open and
 * read. One "key value" line per field, the five days as "d weekday code hi lo".
 *
 * R draws "Fetching..." and calls SYS_REFRESH; the desktop loop refetches, rewrites
 * the file and bumps jt_sysinfo.data_stamp, which this program polls to reload and
 * redraw. Esc exits 0. The window shows one of three honest faces: live,
 * the last good reading (labelled stale), or a labelled sample. It writes
 * "wxwin=" and "wxrow=" lines that tools/checks/weather-app-check.sh reads.
 *
 * Glyphs: antialiased DejaVu Sans from libjt/text.h, the big temperature in
 * the display face.
 */
#include "jtsys.h"
#include "libjt/text.h"

#define BG     0x00F5F0EB
#define CARD   0x00ECE5DC
#define TEXT   0x00403439
#define MID    0x00645057
#define DIM    0x00857A7C
#define ACCENT 0x00C2772B
#define CLOUD  0x00B9AEA6
#define ERRC   0x009A3B2E
#define DAYS   5

static struct jt_window_info win JT_DATA;
static char file[640] JT_DATA, err[48] JT_DATA, city[40] JT_DATA, word[16] JT_DATA, state[12] JT_DATA;
static int have JT_DATA, temp JT_DATA, code JT_DATA, extra JT_DATA, feels JT_DATA, hum JT_DATA, wind JT_DATA, nd JT_DATA;
static int wd[DAYS] JT_DATA, dcode[DAYS] JT_DATA, hi[DAYS] JT_DATA, lo[DAYS] JT_DATA;
static const char *const WEEKDAY[7] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};

static int slen(const char *s) { int n = 0; while (s[n]) n++; return n; }
static int cat(char *b, int p, const char *s) { while (*s && p < 120) b[p++] = *s++; b[p] = 0; return p; }
static int itoa10(int v, char *b) {
    char t[12]; int tn = 0, n = 0; unsigned u = v < 0 ? (unsigned)(-v) : (unsigned)v;
    if (v < 0) b[n++] = '-';
    do { t[tn++] = (char)('0' + u % 10); u /= 10; } while (u);
    while (tn) b[n++] = t[--tn];
    b[n] = 0;
    return n;
}
static int deg(char *b, int v) { int n = itoa10(v, b); b[n++] = (char)0xB0; b[n] = 0; return n; }

static void pset(int x, int y, unsigned c) {
    if (x >= 0 && y >= 0 && x < (int)win.width && y < (int)win.height) win.pixels[(unsigned)y * win.width + (unsigned)x] = c;
}
static void rect(int x, int y, int w, int h, unsigned c) {
    for (int j = 0; j < h; j++) for (int i = 0; i < w; i++) pset(x + i, y + j, c);
}
static void disc(int cx, int cy, int r, unsigned c) {
    for (int j = -r; j <= r; j++) for (int i = -r; i <= r; i++) if (i * i + j * j <= r * r) pset(cx + i, cy + j, c);
}
static void cap(int x0, int y0, int x1, int y1, int t, unsigned c) { /* a line with round ends, t px thick */
    int dx = x1 - x0, dy = y1 - y0, n = (dx < 0 ? -dx : dx) > (dy < 0 ? -dy : dy) ? (dx < 0 ? -dx : dx) : (dy < 0 ? -dy : dy);
    for (int i = 0; i <= n; i++) disc(n ? x0 + dx * i / n : x0, n ? y0 + dy * i / n : y0, t / 2 ? t / 2 : 1, c);
}
static void card(int x, int y, int w, int h) { rect(x + 6, y, w - 12, h, CARD); rect(x, y + 6, w, h - 12, CARD); disc(x + 6, y + 6, 6, CARD); disc(x + w - 7, y + 6, 6, CARD); disc(x + 6, y + h - 7, 6, CARD); disc(x + w - 7, y + h - 7, 6, CARD); }

/* Antialiased text, line top at y. Bold is the real bold cut. */
static int text(const char *s, int x, int y, unsigned fg, int bold) {
    return jt_text_draw(&win, bold ? JT_FACE_BOLD : JT_FACE_BODY, x, y, fg, s);
}
static int tw(const char *s) { return jt_text_width(JT_FACE_BODY, s); }
static void right(const char *s, int xr, int y, unsigned fg, int bold) { text(s, xr - jt_text_width(bold ? JT_FACE_BOLD : JT_FACE_BODY, s), y, fg, bold); }
static void center(const char *s, int cx, int y, unsigned fg, int bold) { text(s, cx - jt_text_width(bold ? JT_FACE_BOLD : JT_FACE_BODY, s) / 2, y, fg, bold); }

/* Condition glyphs from discs and capsules; u is one eighth of the half size. */
static void sun(int cx, int cy, int u, int r8) {
    disc(cx, cy, r8 * u, ACCENT);
    int a = (r8 + 2) * u, b = (r8 + 4) * u, t = u > 2 ? u / 2 : 1, ad = a * 707 / 1000, bd = b * 707 / 1000;
    cap(cx + a, cy, cx + b, cy, t, ACCENT); cap(cx - a, cy, cx - b, cy, t, ACCENT);
    cap(cx, cy + a, cx, cy + b, t, ACCENT); cap(cx, cy - a, cx, cy - b, t, ACCENT);
    cap(cx + ad, cy + ad, cx + bd, cy + bd, t, ACCENT); cap(cx - ad, cy - ad, cx - bd, cy - bd, t, ACCENT);
    cap(cx + ad, cy - ad, cx + bd, cy - bd, t, ACCENT); cap(cx - ad, cy + ad, cx - bd, cy + bd, t, ACCENT);
}
static void cloud(int cx, int cy, int u, int grow, unsigned c) {
    disc(cx - 4 * u, cy + u, 3 * u + grow, c); disc(cx + 5 * u, cy + 2 * u, 2 * u + grow, c);
    disc(cx, cy - u, 5 * u + grow, c); rect(cx - 4 * u, cy + u, 9 * u, 3 * u + grow + 1, c);
}
static void glyph(int c, int cx, int cy, int u, unsigned bg) {
    int t = u > 2 ? u / 2 : 1;
    if (c == 0) { sun(cx, cy, u, 3); return; }
    if (c <= 2) { sun(cx + 3 * u, cy - 3 * u, u, 2); cloud(cx - u, cy + 2 * u, u, t + 1, bg); cloud(cx - u, cy + 2 * u, u, 0, CLOUD); return; }
    if (c == 3) { cloud(cx, cy, u, 0, CLOUD); return; }
    if (c <= 48) { cloud(cx, cy - 3 * u, u, 0, CLOUD); cap(cx - 6 * u, cy + 4 * u, cx + 6 * u, cy + 4 * u, t, DIM); cap(cx - 4 * u, cy + 7 * u, cx + 4 * u, cy + 7 * u, t, DIM); return; }
    cloud(cx, cy - 2 * u, u, 0, CLOUD);
    int snow = (c > 67 && c <= 77) || (c > 82 && c <= 86);
    if (c >= 87) { cap(cx + u, cy + 3 * u, cx - 2 * u, cy + 6 * u, t, ACCENT); cap(cx - 2 * u, cy + 6 * u, cx + u, cy + 6 * u, t, ACCENT); cap(cx + u, cy + 6 * u, cx - u, cy + 9 * u, t, ACCENT); }
    else for (int i = -1; i <= 1; i++) {
        if (snow) disc(cx + i * 4 * u, cy + (i ? 5 : 7) * u, t + 1, DIM);
        else cap(cx + i * 4 * u + u, cy + 4 * u, cx + i * 4 * u - u, cy + 7 * u, t, MID);
    }
}

/* WEATHER.TXT -> the fields above. A missing or empty file leaves state "none". */
static int num(const char **p) {
    int neg = 0, v = 0;
    while (**p == ' ') (*p)++;
    if (**p == '-') { neg = 1; (*p)++; }
    while (**p >= '0' && **p <= '9') v = v * 10 + *(*p)++ - '0';
    return neg ? -v : v;
}
static void copy(char *d, int max, const char *s) { int n = 0; while (*s && *s != '\n' && n < max - 1) d[n++] = *s++; d[n] = 0; }
static int is(const char *l, const char *k) { int n = slen(k); for (int i = 0; i < n; i++) if (l[i] != k[i]) return 0; return l[n] == ' '; }
static void load(void) {
    nd = 0; err[0] = 0;
    int fd = jt_open("WEATHER.TXT", JT_O_RDONLY), n = fd < 0 ? 0 : jt_read(fd, file, sizeof file - 1);
    if (fd >= 0) jt_close(fd);
    file[n < 0 ? 0 : n] = 0;
    state[0] = 'n'; state[1] = 'o'; state[2] = 'n'; state[3] = 'e'; state[4] = 0;
    for (const char *l = file; *l; ) {
        const char *v = l; while (*v && *v != ' ' && *v != '\n') v++;
        if (*v == ' ') v++;
        if (is(l, "state")) copy(state, sizeof state, v);
        else if (is(l, "err")) copy(err, sizeof err, v);
        else if (is(l, "city")) copy(city, sizeof city, v);
        else if (is(l, "word")) copy(word, sizeof word, v);
        else if (is(l, "have")) have = num(&v);
        else if (is(l, "temp")) temp = num(&v);
        else if (is(l, "code")) code = num(&v);
        else if (is(l, "extra")) extra = num(&v);
        else if (is(l, "feels")) feels = num(&v);
        else if (is(l, "hum")) hum = num(&v);
        else if (is(l, "wind")) wind = num(&v);
        else if (is(l, "d") && nd < DAYS) { wd[nd] = num(&v); dcode[nd] = num(&v); hi[nd] = num(&v); lo[nd] = num(&v); nd++; }
        while (*l && *l != '\n') l++;
        if (*l) l++;
    }
}

static int fetching JT_DATA;
static void say(const char *a, const char *b, const char *c) { char l[200]; int n = cat(l, 0, a); n = cat(l, n, b); n = cat(l, n, c); l[n++] = '\n'; jt_write(1, l, (unsigned)n); }

static void draw(void) {
    int live = state[0] == 'o' && state[1] == 'k' && have, stale = !live && have, real = live || stale;
    static const int s_code[DAYS] = {0, 2, 3, 61, 2}, s_hi[DAYS] = {21, 19, 17, 15, 18}, s_lo[DAYS] = {12, 11, 10, 9, 10}, s_wd[DAYS] = {1, 2, 3, 4, 5};
    int t = real ? temp : 18, c = real ? code : 0, hx = real ? extra : 1, n = real ? nd : DAYS;
    int f = real ? feels : 17, h = real ? hum : 55, w = real ? wind : 9;
    const int *dc = real ? dcode : s_code, *dh = real ? hi : s_hi, *dl = real ? lo : s_lo, *dw = real ? wd : s_wd;
    int vw = (int)win.width, vh = (int)win.height, cw = vw - 72 > 760 ? 760 : vw - 72, x0 = (vw - cw) / 2, x1 = x0 + cw, y = 22;
    char b[128], q[8];
    rect(0, 0, vw, vh, BG);

    text(city[0] ? city : real ? "Your location" : "Sample location", x0, y, TEXT, 1);
    text(live ? "Current conditions, live" : stale ? "Last good reading, may be out of date" : "Sample data, not a live reading", x0, y + 20, live ? DIM : MID, 0);
    if (fetching) right("Fetching...", x1, y + 1, MID, 1);
    else if (!live) {
        const char *head = state[0] == 'o' && state[1] == 'f' ? "Offline" : state[0] == 't' ? "Timed out" : state[0] == 'b' ? "Bad response" : state[0] == 'f' ? "Request failed" : "Not fetched yet";
        int p = cat(b, 0, head);
        if (err[0]) { p = cat(b, p, " ("); p = cat(b, p, err); cat(b, p, ")"); }
        right(b, x1, y + 1, ERRC, 1);
        right("Press R to retry", x1, y + 21, MID, 0);
    } else right("Press R to refresh", x1, y + 3, DIM, 0);

    int hy = y + 50;
    deg(b, t);
    int bx = jt_text_draw(&win, JT_FACE_DISPLAY, x0 - 2, hy, TEXT, b) + 22;
    text(real ? word : "Clear", bx, hy + 8, TEXT, 1);
    if (n > 0) { int p = cat(b, 0, "High "); p += deg(b + p, dh[0]); p = cat(b, p, "   Low "); deg(b + p, dl[0]); text(b, bx, hy + 31, MID, 0); }
    glyph(c, x1 - 52, hy + 24, 5, BG);

    int fy = hy + 72, fh = 50, gap = 12, fw = (cw - 2 * gap) / 3;
    for (int i = 0; i < 3; i++) {
        int fx = x0 + i * (fw + gap);
        card(fx, fy, fw, fh);
        text(i == 0 ? "Feels like" : i == 1 ? "Humidity" : "Wind", fx + 16, fy + 8, DIM, 0);
        if (!hx) { text("Not reported", fx + 16, fy + 28, MID, 0); continue; }
        if (i == 0) deg(b, f); else { int p = itoa10(i == 1 ? h : w, b); cat(b, p, i == 1 ? "%" : " km/h"); }
        text(b, fx + 16, fy + 27, TEXT, 1);
    }

    int ry = fy + fh + 16, cy0 = ry + 18, ch = vh - cy0 - 14, drawn = 0;
    if (ch > 118) ch = 118;
    text(live ? "5-day forecast" : stale ? "5-day forecast, last good reading" : "5-day forecast, sample data", x0, ry, MID, 1);
    if (n <= 0) { card(x0, cy0, cw, ch); text("No forecast in the last reply", x0 + 16, cy0 + ch / 2 - 8, MID, 0); }
    else {
        int dwid = (cw - (DAYS - 1) * gap) / DAYS;
        for (int i = 0; i < n && i < DAYS; i++, drawn++) {
            int dx = x0 + i * (dwid + gap), mx = dx + dwid / 2;
            card(dx, cy0, dwid, ch);
            center(i == 0 && real ? "Today" : WEEKDAY[dw[i] % 7], mx, cy0 + 8, TEXT, 1);
            glyph(dc[i], mx, cy0 + ch / 2 - 4, 2, CARD);
            char lw[8]; deg(q, dh[i]); deg(lw, dl[i]); int sx = mx - (jt_text_width(JT_FACE_BOLD, q) + 8 + tw(lw)) / 2;
            sx = text(q, sx, cy0 + ch - 24, TEXT, 1) + 8;
            text(lw, sx, cy0 + ch - 24, DIM, 0);
        }
    }
    { /* what the window really showed, for tools/checks/weather-app-check.sh */
        static int last JT_DATA = -1;
        int sig = (state[0] * 8 + (live ? 0 : stale ? 1 : 2) * 2 + fetching) * 8 + drawn + (hx ? 100000 : 0);
        if (sig != last) {
            last = sig;
            say("wxwin=", fetching ? "fetching" : state, live ? " live" : stale ? " stale" : " sample");
            int p = cat(b, 0, "wxrow="); b[p++] = (char)('0' + drawn); b[p] = 0;
            for (int i = 0; i < drawn; i++) { p = cat(b, p, i ? "," : " "); p = cat(b, p, WEEKDAY[dw[i] % 7]); }
            say(b, hx ? " facts=yes" : " facts=no", "");
            { int p2 = cat(b, 0, "wxshow="); p2 += itoa10(t, b + p2); p2 = cat(b, p2, " "); p2 = cat(b, p2, real ? word : "Clear"); say(b, "", ""); } /* the reading it drew */
        }
    }
}

__attribute__((section(".text.start"), used))
void _start(int argc, char **argv) {
    (void)argc; (void)argv;
    if (jt_window_open(&win) != 0 || !win.pixels) { jt_write(2, "weather: no window\n", 19); jt_exit(1); }
    load();
    draw();
    struct jt_sysinfo si;
    unsigned stamp = 0;
    if (jt_sysinfo(&si) > 0) { stamp = si.data_stamp; if (!si.wx_state) { jt_refresh(JT_REFRESH_WEATHER, 0); } }
    struct jt_event ev;
    unsigned flags = JT_POLL_PRESENT;
    for (;;) {
        int r = jt_window_poll(&ev, flags);
        flags = 0;
        if (r == -11 /* -EAGAIN */) {
            if (jt_sysinfo(&si) > 0 && si.data_stamp != stamp) { stamp = si.data_stamp; fetching = 0; load(); draw(); flags = JT_POLL_PRESENT; }
            jt_sched_yield(); continue;
        }
        if (r != 1) break;
        if (ev.kind == JT_EV_CLICK && (ev.a < 0 || ev.b < 0 || ev.a >= (int)win.width || ev.b >= (int)win.height)) break; /* chrome X or dock */
        if (ev.kind == JT_EV_KEY) {
            if (ev.a == '`') { jt_write(1, "weather: crashing on purpose\n", 29); *(volatile int *)0 = 1; } /* deliberate crash, as in every ring-3 app */
            if (ev.a == JT_KEY_ESC) break;
            if (ev.a == 'r' || ev.a == 'R') {
                fetching = 1; draw();
                jt_window_poll(&ev, JT_POLL_PRESENT); /* put "Fetching..." on screen before the kernel blocks on the network */
                jt_refresh(JT_REFRESH_WEATHER, 0);
                continue;
            }
        }
        flags = JT_POLL_PRESENT;
    }
    jt_exit(0);
}
