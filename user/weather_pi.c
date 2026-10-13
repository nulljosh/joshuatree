/* weather_pi: Weather for the ARM64 build (Pi and QEMU), a small ring-3 program: the city, the temperature large, the
 * condition with a flat glyph, high and low, and a row of five days.
 *
 * The i386 Weather (user/weather.c) is fed by the kernel's weather_fetch, which the ARM kernel does not have, so this
 * is its own, smaller program with the same data contract: it reads WEATHER.TXT through jt_readfile (the card's
 * JT/WEATHER.TXT, the lines weather.c documents: city, word, have, temp, code, isday and one
 * "d weekday code hi lo rain uv sunrise sunset" per day). With no such file, or one that says have 0, it draws a
 * built-in sample and says so in its status line, "Sample, not a live reading". A file is called "Saved reading": this
 * program never claims a live one. Nothing is fetched here and no key is involved.
 *
 * House style (docs/ARCHITECTURE.md): flat, sans serif, terracotta #b5502c accent, glyphs from discs and capsules, no text in
 * a glyph. Esc or the red dot closes it. One line per start tells tools/checks/arm64-weather-check.py what was drawn.
 */
#include "jtsys.h"
#include "libjt/text.h"
#include "libjt/hires.h"

#define BG     0x00F5F0EB
#define CARD   0x00ECE5DC
#define TEXT   0x00403439
#define MID    0x00645057
#define DIM    0x00857A7C
#define ACCENT 0x00B5502C
#define CLOUD  0x00B9AEA6
#define DAYS   5

static struct jt_window_info win JT_DATA;
static char file[2048] JT_DATA, city[32] JT_DATA, word[24] JT_DATA;
static int have JT_DATA, temp JT_DATA, code JT_DATA, nd JT_DATA;
static int wd[DAYS] JT_DATA, dcode[DAYS] JT_DATA, hi[DAYS] JT_DATA, lo[DAYS] JT_DATA;
static const char *const WEEKDAY[7] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};

static int cat(char *b, int p, const char *s) { while (*s && p < 100) b[p++] = *s++; b[p] = 0; return p; }
static int itoa10(int v, char *b) {
    char t[12]; int tn = 0, n = 0; unsigned u = v < 0 ? (unsigned)(-v) : (unsigned)v;
    if (v < 0) b[n++] = '-';
    do { t[tn++] = (char)('0' + u % 10); u /= 10; } while (u);
    while (tn) b[n++] = t[--tn];
    b[n] = 0;
    return n;
}
static int deg(char *b, int v) { int n = itoa10(v, b); b[n++] = (char)0xB0; b[n] = 0; return n; }

static void rect(int x, int y, int w, int h, unsigned c) { jt_fill(&win, x, y, w, h, c); }   /* hires.h: logical pixels, sharp at scale 2 */
static void disc(int cx, int cy, int r, unsigned c) { jt_disc(&win, cx, cy, r, c); }
static void cap(int x0, int y0, int x1, int y1, int t, unsigned c) {   /* a line with round ends, t px thick */
    int dx = x1 - x0, dy = y1 - y0, n = (dx < 0 ? -dx : dx) > (dy < 0 ? -dy : dy) ? (dx < 0 ? -dx : dx) : (dy < 0 ? -dy : dy);
    for (int i = 0; i <= n; i++) disc(n ? x0 + dx * i / n : x0, n ? y0 + dy * i / n : y0, t / 2 ? t / 2 : 1, c);
}
static void rr(int x, int y, int w, int h, int r, unsigned c) {
    rect(x + r, y, w - 2 * r, h, c); rect(x, y + r, w, h - 2 * r, c);
    disc(x + r, y + r, r, c); disc(x + w - r - 1, y + r, r, c); disc(x + r, y + h - r - 1, r, c); disc(x + w - r - 1, y + h - r - 1, r, c);
}
static int text(const char *s, int x, int y, unsigned fg, int bold) { return jt_text_draw(&win, bold ? JT_FACE_BOLD : JT_FACE_BODY, x, y, fg, s); }
static void center(const char *s, int cx, int y, unsigned fg, int bold) { text(s, cx - jt_text_width(bold ? JT_FACE_BOLD : JT_FACE_BODY, s) / 2, y, fg, bold); }
static void right(const char *s, int xr, int y, unsigned fg, int bold) { text(s, xr - jt_text_width(bold ? JT_FACE_BOLD : JT_FACE_BODY, s), y, fg, bold); }

/* The condition glyphs, from weather.c: u2 is twice the unit (an eighth of the half size). */
#define U(k) ((k) * u2 / 2)
static void sun(int cx, int cy, int u2, int r8) {
    disc(cx, cy, U(r8), ACCENT);
    int a = U(r8) + (u2 > 4 ? U(2) : 3), b = U(r8) + (u2 > 4 ? U(4) : 5), t = u2 > 4 ? u2 / 4 : 1, ad = a * 707 / 1000, bd = b * 707 / 1000;
    cap(cx + a, cy, cx + b, cy, t, ACCENT); cap(cx - a, cy, cx - b, cy, t, ACCENT);
    cap(cx, cy + a, cx, cy + b, t, ACCENT); cap(cx, cy - a, cx, cy - b, t, ACCENT);
    cap(cx + ad, cy + ad, cx + bd, cy + bd, t, ACCENT); cap(cx - ad, cy - ad, cx - bd, cy - bd, t, ACCENT);
    cap(cx + ad, cy - ad, cx + bd, cy - bd, t, ACCENT); cap(cx - ad, cy + ad, cx - bd, cy + bd, t, ACCENT);
}
static void cloud(int cx, int cy, int u2, int grow, unsigned c) {
    disc(cx - U(4), cy + U(1), U(3) + grow, c); disc(cx + U(5), cy + U(2), U(2) + grow, c);
    disc(cx, cy - U(1), U(5) + grow, c); rect(cx - U(4), cy + U(1), U(9), U(3) + grow + 1, c);
}
static void glyph(int c, int cx, int cy, int u2, unsigned bg) {
    int t = u2 > 4 ? u2 / 4 : 1;
    if (c == 0) { sun(cx, cy, u2, 3); return; }
    if (c <= 2) { sun(cx + U(3), cy - U(3), u2, 2); cloud(cx - U(1), cy + U(2), u2, t + 1, bg); cloud(cx - U(1), cy + U(2), u2, 0, CLOUD); return; }
    if (c <= 48) { cloud(cx, cy, u2, 0, CLOUD); return; }
    cloud(cx, cy - U(1), u2, 0, CLOUD);
    int snow = (c > 67 && c <= 77) || (c > 82 && c <= 86);
    for (int i = -1; i <= 1; i++) {
        if (snow) disc(cx + i * U(4), cy + U(i ? 5 : 7), t + 1, DIM);
        else cap(cx + i * U(4) + U(1), cy + U(4), cx + i * U(4) - U(1), cy + U(6), t, MID);
    }
}
static const char *word_for(int c) {
    return c == 0 ? "Clear" : c <= 2 ? "Partly cloudy" : c == 3 ? "Cloudy" : c <= 48 ? "Fog" : c <= 57 ? "Drizzle" : c <= 67 ? "Rain"
         : c <= 77 ? "Snow" : c <= 82 ? "Showers" : c <= 86 ? "Snow showers" : "Thunderstorm";
}

static int num(const char **p) {
    int neg = 0, v = 0;
    while (**p == ' ') (*p)++;
    if (**p == '-') { neg = 1; (*p)++; }
    while (**p >= '0' && **p <= '9') v = v * 10 + *(*p)++ - '0';
    return neg ? -v : v;
}
static int is(const char *l, const char *k) { int n = 0; while (k[n]) { if (l[n] != k[n]) return 0; n++; } return l[n] == ' '; }
static void copy(char *d, int max, const char *s) { int n = 0; while (*s && *s != '\n' && n < max - 1) d[n++] = *s++; d[n] = 0; }

static void sample(void) {   /* shown, and labelled, whenever there is no saved reading */
    static const int s_code[DAYS] = {0, 2, 3, 61, 71}, s_hi[DAYS] = {21, 19, 17, 15, 3}, s_lo[DAYS] = {12, 11, 10, 9, -2};
    unsigned now = 0; int dow = 1;
    if (jt_time(&now) > 0 && now) dow = (int)((now / 86400 + 4) % 7);
    copy(city, sizeof city, "Vancouver");
    temp = 18; code = 0; nd = DAYS;
    for (int i = 0; i < DAYS; i++) { wd[i] = (dow + i) % 7; dcode[i] = s_code[i]; hi[i] = s_hi[i]; lo[i] = s_lo[i]; }
}
static void load(void) {
    int n = jt_readfile("WEATHER.TXT", file, sizeof file - 1);
    file[n < 0 ? 0 : n] = 0;
    nd = 0; have = 0; city[0] = 0; word[0] = 0;
    for (const char *l = file; *l; ) {
        const char *v = l; while (*v && *v != ' ' && *v != '\n') v++;
        if (*v == ' ') v++;
        if (is(l, "city")) copy(city, sizeof city, v);
        else if (is(l, "have")) have = num(&v);
        else if (is(l, "temp")) temp = num(&v);
        else if (is(l, "code")) code = num(&v);
        else if (is(l, "d") && nd < DAYS) { wd[nd] = num(&v); dcode[nd] = num(&v); hi[nd] = num(&v); lo[nd] = num(&v); nd++; }
        while (*l && *l != '\n') l++;
        if (*l) l++;
    }
    if (!have || nd <= 0) { have = 0; sample(); }
    if (!city[0]) copy(city, sizeof city, "Vancouver");
    copy(word, sizeof word, word_for(code));
}

static void say(const char *a, int v1, const char *b, int v2) {
    char l[100]; int n = cat(l, 0, a); n += itoa10(v1, l + n); n = cat(l, n, b); n += itoa10(v2, l + n); l[n++] = '\n'; jt_write(1, l, (unsigned)n);
}

static void draw(void) {
    int vw = (int)win.width, vh = (int)win.height, pad = 20, gap = 10;
    char b[48];
    rect(0, 0, vw, vh, BG);
    text(city, pad, 16, TEXT, 1);
    right(have ? "Saved reading" : "Sample, not a live reading", vw - pad, 16, have ? DIM : MID, 0);
    /* the temperature large, the condition and the glyph beside it */
    deg(b, temp);
    int bx = jt_text_draw(&win, JT_FACE_DISPLAY, pad - 2, 44, TEXT, b) + 20;
    text(word, bx, 54, TEXT, 1);
    { int p = cat(b, 0, "High "); p += deg(b + p, hi[0]); p = cat(b, p, "   Low "); deg(b + p, lo[0]); text(b, bx, 78, MID, 0); }
    glyph(code, vw - pad - 52, 82, 8, BG);
    /* five days in a row of cards along the bottom */
    int ch = vh - 150 - 16 < 100 ? 100 : (vh - 150 - 16 > 140 ? 140 : vh - 150 - 16), cy = vh - 16 - ch, cw = (vw - 2 * pad - (DAYS - 1) * gap) / DAYS;
    text("Next five days", pad, cy - 24, MID, 1);
    for (int i = 0; i < DAYS && i < nd; i++) {
        int x = pad + i * (cw + gap), mx = x + cw / 2;
        rr(x, cy, cw, ch, 6, CARD);
        center(i == 0 ? "Today" : WEEKDAY[wd[i] % 7], mx, cy + 8, TEXT, 1);
        glyph(dcode[i], mx, cy + 8 + 16 + (ch - 8 - 16 - 38) / 2 + 12, 5, CARD);
        deg(b, hi[i]); center(b, mx, cy + ch - 38, TEXT, 1);
        deg(b, lo[i]); center(b, mx, cy + ch - 20, DIM, 0);
    }
}

__attribute__((section(".text.start"), used))
void _start(int argc, char **argv) {
    (void)argc; (void)argv;
    if (jt_window_open_hires(&win) != 0 || !win.pixels) { jt_write(2, "weather: no window\n", 19); jt_exit(1); }
    load();
    draw();
    say("weather: ", have ? 1 : 0, " saved, days ", nd);
    say("weather: temp ", temp, " code ", code);
    struct jt_event ev;
    unsigned flags = JT_POLL_PRESENT;
    for (;;) {
        int r = jt_window_poll(&ev, flags);
        if (r == 1 && jt_window_resized(&ev, &win)) { draw(); flags = JT_POLL_PRESENT; continue; }
        flags = 0;
        if (r == -11 /* -EAGAIN */) { jt_sched_yield(); continue; }
        if (r != 1) break;
        if (ev.kind == JT_EV_CLICK && (ev.a < 0 || ev.b < 0 || ev.a >= (int)win.width || ev.b >= (int)win.height)) break;   /* the red dot */
        if (ev.kind == JT_EV_KEY && ev.a == JT_KEY_ESC) break;
        flags = JT_POLL_PRESENT;
    }
    jt_write(1, "weather: closed\n", 16);
    jt_exit(0);
}
