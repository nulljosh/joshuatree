/* weather: now, the next 24 hours, seven days and a few details, as a real
 * ring-3 program.
 *
 * The twentieth app to leave the kernel (roadmap 2.0). The network side did
 * not move: the kernel's weather_fetch still owns the location lookup, the
 * Open-Meteo call, the menu bar text, the wind sway, phone home and
 * Samantha's weather tool. After every fetch, good or not, it writes
 * WEATHER.TXT, and this program only reads that file through jt_readfile.
 * One "key value" line per field: the scalars, "h*" lines for the 24 hours
 * (hs start hour, ht temperature, hc code, hp chance of rain, hd is_day) and
 * "d weekday code hi lo rain uv10 sunrise sunset" for each of seven days.
 *
 * 2.10.0 layout (house style: flat, sans serif only, terracotta accent):
 *   overview  city, the big temperature, a condition glyph drawn from discs and
 *             capsules, high and low, feels like, humidity, wind
 *   hourly    the next 24 hours as a strip, Left and Right scroll it
 *   7 days    a row per day with a high/low bar on a shared scale; Up and
 *             Down pick the day the details describe
 *   details   sunrise, sunset, UV, chance of rain for the picked day, and
 *             pressure and visibility right now
 * Wide windows use two columns; narrow ones stack the sections and scroll.
 * Tab and the arrow keys move between the three sections (a terracotta ring
 * marks the focused one once a key has been used). R refetches through
 * SYS_REFRESH, Esc exits 0. The window shows one of three honest faces: live,
 * the last good reading (labelled stale), or a labelled sample (every section
 * title says so). Lines "wxwin=", "wxrow=", "wxshow=" and "wxsec=" tell
 * tools/checks what it drew.
 *
 * Glyphs: antialiased DejaVu Sans from libjt/text.h, the big temperature in
 * the display face. The degree sign is the Latin-1 0xB0 the font has.
 */
#include "jtsys.h"
#include "libjt/text.h"

#define BG     0x00F5F0EB
#define CARD   0x00ECE5DC
#define SEL    0x00E1D6C9
#define TRACK  0x00D9CDBF
#define TEXT   0x00403439
#define MID    0x00645057
#define DIM    0x00857A7C
#define ACCENT 0x00B5502C
#define CLOUD  0x00B9AEA6
#define MOON   0x00645057
#define ERRC   0x009A3B2E
#define DAYS   7
#define HOURS  24

enum { S_TOP, S_HOUR, S_WEEK, S_DET };

static struct jt_window_info win JT_DATA;
static char file[2048] JT_DATA, err[48] JT_DATA, city[40] JT_DATA, word[16] JT_DATA, state[12] JT_DATA;
static int have JT_DATA, temp JT_DATA, code JT_DATA, extra JT_DATA, feels JT_DATA, hum JT_DATA, wind JT_DATA, nd JT_DATA;
static int press JT_DATA, vis10 JT_DATA, isday JT_DATA, nh JT_DATA, hstart JT_DATA;
static int wd[DAYS] JT_DATA, dcode[DAYS] JT_DATA, hi[DAYS] JT_DATA, lo[DAYS] JT_DATA, dpop[DAYS] JT_DATA, duv[DAYS] JT_DATA, drise[DAYS] JT_DATA, dset[DAYS] JT_DATA;
static int ht[HOURS] JT_DATA, hc[HOURS] JT_DATA, hp[HOURS] JT_DATA, hdy[HOURS] JT_DATA;
static int sec JT_DATA = S_WEEK, kbd JT_DATA, day JT_DATA, hoff JT_DATA, sy JT_DATA, fetching JT_DATA;
static int L[4][4] JT_DATA;          /* x, y, w, h of each section, before the scroll offset */
static int content JT_DATA, wide JT_DATA, tcols JT_DATA, trh JT_DATA, wrh JT_DATA;
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
static int imin(int a, int b) { return a < b ? a : b; }
static int imax(int a, int b) { return a > b ? a : b; }
static int clamp(int v, int a, int b) { return v < a ? a : v > b ? b : v; }

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
static void rr(int x, int y, int w, int h, int r, unsigned c) { /* rounded rectangle */
    rect(x + r, y, w - 2 * r, h, c); rect(x, y + r, w, h - 2 * r, c);
    disc(x + r, y + r, r, c); disc(x + w - r - 1, y + r, r, c); disc(x + r, y + h - r - 1, r, c); disc(x + w - r - 1, y + h - r - 1, r, c);
}
/* A section card; focused (after a key press) it gets a terracotta ring around it. */
static void card(int x, int y, int w, int h, int ring) {
    if (ring) rr(x - 2, y - 2, w + 4, h + 4, 8, ACCENT);
    rr(x, y, w, h, 6, CARD);
}

/* Antialiased text, line top at y. Bold is the real bold cut. */
static int text(const char *s, int x, int y, unsigned fg, int bold) {
    return jt_text_draw(&win, bold ? JT_FACE_BOLD : JT_FACE_BODY, x, y, fg, s);
}
static int tw(const char *s) { return jt_text_width(JT_FACE_BODY, s); }
static void right(const char *s, int xr, int y, unsigned fg, int bold) { text(s, xr - jt_text_width(bold ? JT_FACE_BOLD : JT_FACE_BODY, s), y, fg, bold); }
static void center(const char *s, int cx, int y, unsigned fg, int bold) { text(s, cx - jt_text_width(bold ? JT_FACE_BOLD : JT_FACE_BODY, s) / 2, y, fg, bold); }

/* Condition glyphs from discs and capsules. u2 is twice the unit (one unit is an eighth of the
   half size), so 3 is a unit and a half for the week rows. A night sky swaps the sun for a
   crescent. bg is the colour under the glyph, which the crescent and the cloud's halo are cut
   with. */
#define U(k) ((k) * u2 / 2)
static void sun(int cx, int cy, int u2, int r8) {
    disc(cx, cy, U(r8), ACCENT);
    int a = U(r8) + (u2 > 4 ? U(2) : 3), b = U(r8) + (u2 > 4 ? U(4) : 5), t = u2 > 4 ? u2 / 4 : 1, ad = a * 707 / 1000, bd = b * 707 / 1000;
    cap(cx + a, cy, cx + b, cy, t, ACCENT); cap(cx - a, cy, cx - b, cy, t, ACCENT);
    cap(cx, cy + a, cx, cy + b, t, ACCENT); cap(cx, cy - a, cx, cy - b, t, ACCENT);
    cap(cx + ad, cy + ad, cx + bd, cy + bd, t, ACCENT); cap(cx - ad, cy - ad, cx - bd, cy - bd, t, ACCENT);
    cap(cx + ad, cy - ad, cx + bd, cy - bd, t, ACCENT); cap(cx - ad, cy + ad, cx - bd, cy + bd, t, ACCENT);
}
static void moon(int cx, int cy, int u2, int r8, unsigned bg) {
    int r = U(r8) + U(1);
    disc(cx, cy, r, MOON); disc(cx + r * 5 / 12, cy - r * 3 / 10, r * 85 / 100, bg);
}
static void cloud(int cx, int cy, int u2, int grow, unsigned c) {
    disc(cx - U(4), cy + U(1), U(3) + grow, c); disc(cx + U(5), cy + U(2), U(2) + grow, c);
    disc(cx, cy - U(1), U(5) + grow, c); rect(cx - U(4), cy + U(1), U(9), U(3) + grow + 1, c);
}
static void glyph(int c, int cx, int cy, int u2, unsigned bg, int night) {
    int t = u2 > 4 ? u2 / 4 : 1;
    if (c == 0) { if (night) moon(cx, cy, u2, 3, bg); else sun(cx, cy, u2, 3); return; }
    if (c <= 2) {
        if (night) moon(cx + U(3), cy - U(3), u2, 2, bg); else sun(cx + U(3), cy - U(3), u2, 2);
        cloud(cx - U(1), cy + U(2), u2, t + 1, bg); cloud(cx - U(1), cy + U(2), u2, 0, CLOUD); return;
    }
    if (c == 3) { cloud(cx, cy, u2, 0, CLOUD); return; }
    if (c <= 48) { cloud(cx, cy - U(3), u2, 0, CLOUD); cap(cx - U(6), cy + U(4), cx + U(6), cy + U(4), t, DIM); cap(cx - U(4), cy + U(7), cx + U(4), cy + U(7), t, DIM); return; }
    cloud(cx, cy - U(1), u2, 0, CLOUD);
    int snow = (c > 67 && c <= 77) || (c > 82 && c <= 86);
    if (c >= 87) { int bt = t < 2 ? 2 : t; cap(cx + U(2), cy + U(3), cx - U(1), cy + U(5), bt, ACCENT); cap(cx - U(1), cy + U(5), cx + U(2), cy + U(5), bt, ACCENT); cap(cx + U(2), cy + U(5), cx - U(1), cy + U(8), bt, ACCENT); }
    else for (int i = -1; i <= 1; i++) {
        if (snow) disc(cx + i * U(4), cy + U(i ? 5 : 7), t + 1, DIM);
        else cap(cx + i * U(4) + U(1), cy + U(4), cx + i * U(4) - U(1), cy + U(6), t, MID);
    }
}

/* Clock and number strings. */
static void hour_label(char *b, int h24) { /* "3 PM" */
    h24 = ((h24 % 24) + 24) % 24;
    int p = itoa10(h24 % 12 ? h24 % 12 : 12, b);
    cat(b, p, h24 < 12 ? " AM" : " PM");
}
static void clock_label(char *b, int mins) { /* "7:18 AM", "--" when unknown */
    if (mins < 0) { cat(b, 0, "--"); return; }
    int h = mins / 60, m = mins % 60, p = itoa10(h % 12 ? h % 12 : 12, b);
    b[p++] = ':'; b[p++] = (char)('0' + m / 10); b[p++] = (char)('0' + m % 10); b[p] = 0;
    cat(b, p, h < 12 ? " AM" : " PM");
}
static void pct_label(char *b, int v) { if (v < 0) { cat(b, 0, "--"); return; } int p = itoa10(v, b); b[p++] = '%'; b[p] = 0; }
static const char *uv_word(int uv10) { return uv10 < 30 ? "Low" : uv10 < 60 ? "Moderate" : uv10 < 80 ? "High" : uv10 < 110 ? "Very high" : "Extreme"; }

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
static int hours_line(const char *v, int *a) { int n = 0; while (n < HOURS && *v && *v != '\n') a[n++] = num(&v); return n; }

/* Shown, and labelled sample, whenever the kernel has no reading at all. */
static void fill_sample(void) {
    static const int s_code[DAYS] = {0, 2, 3, 61, 2, 1, 3}, s_hi[DAYS] = {21, 19, 17, 15, 18, 20, 19}, s_lo[DAYS] = {12, 11, 10, 9, 10, 11, 11};
    static const int s_pop[DAYS] = {0, 10, 20, 70, 20, 5, 15}, s_uv[DAYS] = {40, 30, 20, 10, 30, 50, 40};
    static const int s_ht[HOURS] = {14, 15, 17, 18, 19, 19, 18, 17, 15, 14, 13, 12, 12, 11, 11, 10, 10, 10, 11, 12, 13, 15, 16, 17};
    static const int s_hc[HOURS] = {0, 0, 1, 2, 2, 3, 3, 3, 61, 61, 3, 3, 2, 2, 1, 0, 0, 0, 0, 1, 1, 2, 2, 3};
    static const int s_hp[HOURS] = {0, 0, 5, 10, 10, 15, 20, 30, 60, 70, 40, 20, 10, 5, 0, 0, 0, 0, 0, 0, 5, 5, 10, 10};
    temp = 18; code = 0; extra = 1; feels = 17; hum = 55; wind = 9; press = 1015; vis10 = 160; isday = 1;
    word[0] = 'C'; word[1] = 'l'; word[2] = 'e'; word[3] = 'a'; word[4] = 'r'; word[5] = 0;
    nd = DAYS; nh = HOURS; hstart = 9;
    for (int i = 0; i < DAYS; i++) { wd[i] = (i + 1) % 7; dcode[i] = s_code[i]; hi[i] = s_hi[i]; lo[i] = s_lo[i]; dpop[i] = s_pop[i]; duv[i] = s_uv[i]; drise[i] = 7 * 60 + 18 + i; dset[i] = 18 * 60 + 40 - 2 * i; }
    for (int i = 0; i < HOURS; i++) { ht[i] = s_ht[i]; hc[i] = s_hc[i]; hp[i] = s_hp[i]; hdy[i] = i < 10 || i >= 22; }
}

static void load(void) {
    nd = 0; nh = 0; err[0] = 0;
    for (int i = 0; i < HOURS; i++) { hp[i] = -1; hdy[i] = 1; }
    /* jt_read moves 255 bytes a call; a long city or error line pushed the forecast past it. Take the whole file. */
    int n = jt_readfile("WEATHER.TXT", file, sizeof file - 1);
    file[n < 0 ? 0 : n] = 0;
    state[0] = 'n'; state[1] = 'o'; state[2] = 'n'; state[3] = 'e'; state[4] = 0;
    press = 0; vis10 = -1; isday = 1;
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
        else if (is(l, "press")) press = num(&v);
        else if (is(l, "vis")) vis10 = num(&v);
        else if (is(l, "isday")) isday = num(&v);
        else if (is(l, "hs")) hstart = num(&v);
        else if (is(l, "ht")) nh = hours_line(v, ht);
        else if (is(l, "hc")) hours_line(v, hc);
        else if (is(l, "hp")) hours_line(v, hp);
        else if (is(l, "hd")) hours_line(v, hdy);
        else if (is(l, "d") && nd < DAYS) {
            wd[nd] = num(&v); dcode[nd] = num(&v); hi[nd] = num(&v); lo[nd] = num(&v);
            dpop[nd] = num(&v); duv[nd] = num(&v); drise[nd] = num(&v); dset[nd] = num(&v); nd++;
        }
        while (*l && *l != '\n') l++;
        if (*l) l++;
    }
    if (!have) fill_sample();
    day = clamp(day, 0, imax(nd - 1, 0));
    hoff = clamp(hoff, 0, imax(nh - 1, 0));
}

static void say(const char *a, const char *b, const char *c) { char l[200]; int n = cat(l, 0, a); n = cat(l, n, b); n = cat(l, n, c); l[n++] = '\n'; jt_write(1, l, (unsigned)n); }

/* Section rectangles for the current window size. Wide windows put the overview and the hourly
   strip on the left, the week and the details on the right; narrow ones stack all four and scroll. */
static void layout(void) {
    int vw = (int)win.width, vh = (int)win.height, gap = 14;
    wide = vw >= 700;
    int cw = wide ? imin(vw - 40, 980) : vw - 32, x0 = (vw - cw) / 2, extra_h = imax(vh - 345, 0);
    wrh = 24 + imin(extra_h / 10, 8);
    trh = 41 + imin(extra_h / 6, 14);
    tcols = cw >= 360 ? 3 : 2;
    int trows = 6 / tcols, detail_h = 22 + trows * trh + (trows - 1) * 8, week_h = 30 + DAYS * wrh + 4;
    if (wide) {
        int lw = (cw - gap) * 47 / 100, rw = cw - gap - lw, rx = x0 + lw + gap, top_h = 154;
        L[S_TOP][0] = x0; L[S_TOP][1] = 12; L[S_TOP][2] = lw; L[S_TOP][3] = top_h;
        L[S_HOUR][0] = x0; L[S_HOUR][1] = 12 + top_h + 10; L[S_HOUR][2] = lw; L[S_HOUR][3] = clamp(vh - L[S_HOUR][1] - 12, 120, 190);
        L[S_WEEK][0] = rx; L[S_WEEK][1] = 12; L[S_WEEK][2] = rw; L[S_WEEK][3] = week_h;
        L[S_DET][0] = rx; L[S_DET][1] = 12 + week_h + 8; L[S_DET][2] = rw; L[S_DET][3] = detail_h;
        content = imax(L[S_HOUR][1] + L[S_HOUR][3], L[S_DET][1] + L[S_DET][3]) + 8;
    } else {
        int y = 12, h[4];
        h[0] = 154; h[1] = 134; h[2] = week_h; h[3] = detail_h;
        for (int i = 0; i < 4; i++) { L[i][0] = x0; L[i][1] = y; L[i][2] = cw; L[i][3] = h[i]; y += h[i] + 10; }
        content = y + 2;
    }
    sy = clamp(sy, 0, imax(content - vh, 0));
}
/* Scroll so the focused section is on screen (a no-op when everything fits). */
static void reveal(void) {
    int vh = (int)win.height, y0 = L[sec][1], y1 = y0 + L[sec][3];
    if (y0 - sy < 8) sy = y0 - 8;
    if (y1 - sy > vh - 8) sy = y1 - vh + 8;
    sy = clamp(sy, 0, imax(content - vh, 0));
}

static void draw_top(int x, int y, int w, int live, int stale, int real) {
    char b[128];
    text(city[0] ? city : real ? "Your location" : "Sample location", x, y, TEXT, 1);
    text(live ? "Current conditions, live" : stale ? "Last good reading, may be out of date" : "Sample data, not a live reading", x, y + 18, live ? DIM : MID, 0);
    if (fetching) right("Fetching...", x + w, y, MID, 1);
    else if (!live) {
        const char *head = state[0] == 'o' && state[1] == 'f' ? "Offline" : state[0] == 't' ? "Timed out" : state[0] == 'b' ? "Bad response" : state[0] == 'f' ? "Request failed" : "Not fetched yet";
        int p = cat(b, 0, head);
        if (err[0] && tw(b) + tw(err) < w - 200) { p = cat(b, p, " ("); p = cat(b, p, err); cat(b, p, ")"); }
        right(b, x + w, y, ERRC, 1);
        right("Press R to retry", x + w, y + 18, MID, 0);
    } else right("Press R to refresh", x + w, y + 18, DIM, 0);

    int hy = y + 36;
    deg(b, temp);
    int bx = jt_text_draw(&win, JT_FACE_DISPLAY, x - 2, hy, TEXT, b) + 20;
    text(word, bx, hy + 8, TEXT, 1);
    if (nd > 0) { int p = cat(b, 0, "High "); p += deg(b + p, hi[0]); p = cat(b, p, "   Low "); deg(b + p, lo[0]); text(b, bx, hy + 31, MID, 0); }
    glyph(code, x + w - 46, hy + 30, 8, BG, !isday);

    int fy = hy + 76, fh = 42, gap = 10, fw = (w - 2 * gap) / 3;
    for (int i = 0; i < 3; i++) {
        int fx = x + i * (fw + gap);
        card(fx, fy, fw, fh, 0);
        text(i == 0 ? "Feels like" : i == 1 ? "Humidity" : "Wind", fx + 12, fy + 5, DIM, 0);
        if (!extra) { text("Not reported", fx + 12, fy + 22, MID, 0); continue; }
        if (i == 0) deg(b, feels); else { int p = itoa10(i == 1 ? hum : wind, b); cat(b, p, i == 1 ? "%" : " km/h"); }
        text(b, fx + 12, fy + 21, TEXT, 1);
    }
}

static int vis_hours(int w) { return clamp((w - 24) / 54, 1, imax(nh, 1)); }

static void draw_hourly(int x, int y, int w, int h, int live, int stale) {
    char b[24];
    card(x, y, w, h, kbd && sec == S_HOUR);
    text(live ? "Next 24 hours" : stale ? "Next 24 hours, last good reading" : "Next 24 hours, sample data", x + 12, y + 8, MID, 1);
    if (nh <= 0) { text("No hourly forecast in the last reply", x + 12, y + h / 2 - 6, MID, 0); return; }
    int ix = x + 12, iw = w - 24, vis = vis_hours(w), cell = iw / vis;
    hoff = clamp(hoff, 0, imax(nh - vis, 0));
    int body = 16 + 6 + 36 + 6 + 16 + 2 + 14, top = y + 28 + (h - 28 - 16 - body) / 2;
    for (int j = 0; j < vis; j++) {
        int i = hoff + j, cx = ix + j * cell + cell / 2;
        if (i == 0) center("Now", cx, top, TEXT, 1); else { hour_label(b, hstart + i); center(b, cx, top, MID, 0); }
        glyph(hc[i], cx, top + 16 + 6 + 18, 4, CARD, !hdy[i]);
        deg(b, ht[i]); center(b, cx, top + 16 + 6 + 36 + 6, TEXT, 1);
        if (hp[i] >= 10) { pct_label(b, hp[i]); center(b, cx, top + 16 + 6 + 36 + 6 + 18, ACCENT, 0); }
    }
    if (nh > vis) { /* where in the 24 hours the strip is */
        int ty = y + h - 12, tl = iw * vis / nh, tx = ix + (iw - tl) * hoff / imax(nh - vis, 1);
        cap(ix, ty, ix + iw, ty, 4, TRACK); cap(tx, ty, tx + tl, ty, 4, MID);
    }
}

static void draw_week(int x, int y, int w, int live, int stale) {
    char b[24], c[24];
    card(x, y, w, L[S_WEEK][3], kbd && sec == S_WEEK);
    text(live ? "7-day forecast" : stale ? "7-day forecast, last good reading" : "7-day forecast, sample data", x + 12, y + 8, MID, 1);
    if (nd <= 0) { text("No forecast in the last reply", x + 12, y + 40, MID, 0); return; }
    int ix = x + 12, iw = w - 24, gmin = lo[0], gmax = hi[0];
    for (int i = 1; i < nd; i++) { gmin = imin(gmin, lo[i]); gmax = imax(gmax, hi[i]); }
    if (gmax <= gmin) gmax = gmin + 1;
    int bx0 = ix + 182, bx1 = ix + iw - 50;
    for (int i = 0; i < nd; i++) {
        int ry = y + 30 + i * wrh, ty = ry + (wrh - 16) / 2 - 1, my = ry + wrh / 2;
        if (i == day) rr(x + 5, ry, w - 10, wrh, 6, SEL);
        text(i == 0 && (live || stale) ? "Today" : WEEKDAY[wd[i] % 7], ix + 4, ty, TEXT, 1);
        glyph(dcode[i], ix + 78, my, 3, i == day ? SEL : CARD, 0);
        if (dpop[i] >= 10) { pct_label(b, dpop[i]); right(b, ix + 134, ty, ACCENT, 0); }
        deg(b, lo[i]); right(b, ix + 172, ty, DIM, 0);
        int a = bx0 + (lo[i] - gmin) * (bx1 - bx0) / (gmax - gmin), z = bx0 + (hi[i] - gmin) * (bx1 - bx0) / (gmax - gmin);
        cap(bx0, my, bx1, my, 6, TRACK);
        cap(a, my, z, my, 6, ACCENT);
        deg(c, hi[i]); right(c, ix + iw - 4, ty, TEXT, 1);
    }
}

static void tile(int i, int x, int y, int w, int h) {
    char b[40]; const char *label = "";
    int ok = 1; b[0] = 0;
    switch (i) {
    case 0: label = "Sunrise"; clock_label(b, drise[day]); ok = drise[day] >= 0; break;
    case 1: label = "Sunset"; clock_label(b, dset[day]); ok = dset[day] >= 0; break;
    case 2: label = "UV index"; ok = duv[day] >= 0;
        if (ok) { int p = itoa10((duv[day] + 5) / 10, b); b[p++] = ' '; b[p] = 0; cat(b, p, uv_word(duv[day])); } break;
    case 3: label = "Chance of rain"; pct_label(b, dpop[day]); ok = dpop[day] >= 0; break;
    case 4: label = "Pressure now"; ok = press > 0; if (ok) { int p = itoa10(press, b); cat(b, p, " hPa"); } break;
    default: label = "Visibility now"; ok = vis10 >= 0;
        if (ok) { int p = vis10 >= 100 ? itoa10((vis10 + 5) / 10, b) : itoa10(vis10 / 10, b); if (vis10 < 100) { b[p++] = '.'; b[p++] = (char)('0' + vis10 % 10); b[p] = 0; } cat(b, p, " km"); } break;
    }
    card(x, y, w, h, kbd && sec == S_DET);
    text(label, x + 12, y + 5, DIM, 0);
    text(ok ? b : "Not reported", x + 12, y + 21 + (h - 41) / 2, ok ? TEXT : MID, ok);
}
static void draw_details(int x, int y, int w, int live) {
    char b[56]; int p = cat(b, 0, "Details, ");
    p = cat(b, p, day == 0 ? "today" : WEEKDAY[wd[day] % 7]);
    if (!have) cat(b, p, ", sample data"); else if (!live) cat(b, p, ", last good reading");
    text(b, x + 2, y, MID, 1);
    int gap = 10, cw = (w - (tcols - 1) * gap) / tcols;
    for (int i = 0; i < 6; i++) tile(i, x + (i % tcols) * (cw + gap), y + 22 + (i / tcols) * (trh + 8), cw, trh);
}

static void draw(void) {
    int live = state[0] == 'o' && state[1] == 'k' && have, stale = !live && have, real = live || stale, drawn;
    int vw = (int)win.width, vh = (int)win.height;
    char b[128];
    layout();
    rect(0, 0, vw, vh, BG);
    draw_top(L[S_TOP][0], L[S_TOP][1] - sy, L[S_TOP][2], live, stale, real);
    draw_hourly(L[S_HOUR][0], L[S_HOUR][1] - sy, L[S_HOUR][2], L[S_HOUR][3], live, stale);
    draw_week(L[S_WEEK][0], L[S_WEEK][1] - sy, L[S_WEEK][2], live, stale);
    draw_details(L[S_DET][0], L[S_DET][1] - sy, L[S_DET][2], live);
    drawn = nd;
    if (content > vh) { /* a short window scrolls: a thin bar at the right edge says where */
        int th = (vh - 16) * vh / content, ty = 8 + (vh - 16 - th) * sy / imax(content - vh, 1);
        cap(vw - 6, ty, vw - 6, ty + th, 4, TRACK);
    }
    { /* what the window really showed, for tools/checks/weather-app-check.sh */
        static int last JT_DATA = -1;
        int sig = (state[0] * 8 + (live ? 0 : stale ? 1 : 2) * 2 + fetching) * 8 + drawn + (extra ? 100000 : 0) + nh * 1000000 + vw * 7 + vh * 13;
        if (sig != last) {
            last = sig;
            say("wxwin=", fetching ? "fetching" : state, live ? " live" : stale ? " stale" : " sample");
            int p = cat(b, 0, "wxrow="); b[p++] = (char)('0' + drawn); b[p] = 0;
            for (int i = 0; i < drawn; i++) { p = cat(b, p, i ? "," : " "); p = cat(b, p, WEEKDAY[wd[i] % 7]); }
            say(b, extra ? " facts=yes" : " facts=no", "");
            { int p2 = cat(b, 0, "wxhours="); p2 += itoa10(nh, b + p2); say(b, "", ""); }
            { int p2 = cat(b, 0, "wxshow="); p2 += itoa10(temp, b + p2); p2 = cat(b, p2, " "); p2 = cat(b, p2, word); say(b, "", ""); } /* the reading it drew */
            { int p2 = cat(b, 0, "wxdim="); p2 += itoa10(vw, b + p2); p2 = cat(b, p2, "x"); p2 += itoa10(vh, b + p2); say(b, wide ? " wide" : " narrow", ""); }
        }
    }
}

static void note_focus(void) { /* one line per focus move, for tools/checks */
    char b[64]; int p = cat(b, 0, "wxsec=");
    p = cat(b, p, sec == S_HOUR ? "hourly" : sec == S_WEEK ? "week" : "details");
    p = cat(b, p, " day="); p += itoa10(day, b + p); p = cat(b, p, " hoff="); itoa10(hoff, b + p);
    say(b, "", "");
}

/* Returns 1 when the key was one of ours (the window redraws). */
static int key(int k) {
    int vis = vis_hours(L[S_HOUR][2]);
    if (k == '\t') sec = sec == S_HOUR ? S_WEEK : sec == S_WEEK ? S_DET : S_HOUR;
    else if (k == JT_KEY_DOWN) {
        if (sec == S_HOUR) sec = S_WEEK;
        else if (sec == S_WEEK) { if (day < nd - 1) day++; else sec = S_DET; }
    } else if (k == JT_KEY_UP) {
        if (sec == S_DET) sec = S_WEEK;
        else if (sec == S_WEEK) { if (day > 0) day--; else sec = S_HOUR; }
    } else if (k == JT_KEY_LEFT) {
        if (sec == S_HOUR) hoff = imax(hoff - 1, 0); else sec = S_HOUR;
    } else if (k == JT_KEY_RIGHT) {
        if (sec == S_HOUR) { if (hoff < nh - vis) hoff++; else sec = S_WEEK; }
    } else if (k == JT_KEY_HOME && sec == S_HOUR) hoff = 0;
    else if (k == JT_KEY_END && sec == S_HOUR) hoff = imax(nh - vis, 0);
    else return 0;
    kbd = 1;
    layout(); reveal();
    return 1;
}

static void click(int mx, int my) {
    int yy = my + sy;
    for (int s = S_HOUR; s <= S_DET; s++) {
        if (mx < L[s][0] || mx >= L[s][0] + L[s][2] || yy < L[s][1] || yy >= L[s][1] + L[s][3]) continue;
        sec = s; kbd = 1;
        if (s == S_WEEK && yy >= L[S_WEEK][1] + 30) { int r = (yy - L[S_WEEK][1] - 30) / wrh; if (r < nd) day = r; }
        return;
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
        if (r == 1 && jt_window_resized(&ev, &win)) { draw(); flags = JT_POLL_PRESENT; continue; } /* JT_EV_RESIZE: remapped, repaint at the new size */
        flags = 0;
        if (r == -11 /* -EAGAIN */) {
            if (jt_sysinfo(&si) > 0 && si.data_stamp != stamp) { stamp = si.data_stamp; fetching = 0; load(); draw(); flags = JT_POLL_PRESENT; }
            jt_sched_yield(); continue;
        }
        if (r != 1) break;
        if (ev.kind == JT_EV_CLICK && (ev.a < 0 || ev.b < 0 || ev.a >= (int)win.width || ev.b >= (int)win.height)) break; /* chrome X or dock */
        if (ev.kind == JT_EV_CLICK) { click(ev.a, ev.b); draw(); note_focus(); }
        if (ev.kind == JT_EV_WHEEL) {
            if (sec == S_HOUR && content <= (int)win.height) hoff = clamp(hoff - ev.a, 0, imax(nh - vis_hours(L[S_HOUR][2]), 0));
            else sy = clamp(sy - ev.a * 24, 0, imax(content - (int)win.height, 0));
            draw();
        }
        if (ev.kind == JT_EV_KEY) {
            if (ev.a == '`') { jt_write(1, "weather: crashing on purpose\n", 29); *(volatile int *)0 = 1; } /* deliberate crash, as in every ring-3 app */
            if (ev.a == JT_KEY_ESC) break;
            if (ev.a == 'r' || ev.a == 'R') {
                fetching = 1; draw();
                jt_window_poll(&ev, JT_POLL_PRESENT); /* put "Fetching..." on screen before the kernel blocks on the network */
                jt_refresh(JT_REFRESH_WEATHER, 0);
                continue;
            }
            if (key(ev.a)) { draw(); note_focus(); }
        }
        flags = JT_POLL_PRESENT;
    }
    jt_exit(0);
}
