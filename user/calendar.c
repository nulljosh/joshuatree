/* calendar: Day, Week, Month and Year views over EVENTS.TXT, as a real ring-3 program.
 *
 * The seventeenth app to leave the kernel (roadmap 2.0), done the way
 * user/reminders.c was. Same job kernel/calendar.h did in ring 0: the four
 * views, 1-4 to pick one, left/right (up/down, a/d) to step by the view's own
 * unit, [ and ] to move the selected day, t for today, Enter to edit the
 * selected day's one event, Esc to close. Built with no kernel include path,
 * linked flat, loaded off the VFS by exec_user, and it reaches the machine
 * only through int 0x80: SYS_WINDOW_OPEN for a framebuffer, SYS_WINDOW_POLL
 * for input and the present, SYS_TIME for today, and the ordinary file calls
 * (open, read, write, close) for EVENTS.TXT. No new syscall.
 *
 * Today comes from SYS_TIME's seconds since the epoch, so the date math the
 * kernel used to do off the CMOS registers now happens here: days since 1970
 * to a civil date (Howard Hinnant's algorithm, integer only), then Zeller's
 * congruence for the weekday, exactly as calendar.h had it. Both sit above
 * the CALENDAR_MATH_ONLY guard so tools/checks/check-calendar.sh can pull
 * them into a host build and sweep every day 1900-2099 against libc.
 *
 * File format, unchanged: one line per date, "YYYY-MM-DD|text". One event
 * per date; saving a day rewrites its line in place, then the whole file.
 * Samantha's calendar_today tool in kernel/chat.h reads the same file fresh
 * on every call, so the two never disagree.
 *
 * A flat binary has no .bss, so the event list lives in the zeroed pages
 * past _user_end (exec_user zeroes the whole image window before loading).
 *
 * Layout: every y is written as T + offset with T = -32, the same numbers
 * calendar.h used inside a dock window, so tools/checks/calviews-check.py
 * and apptop-check.py probe the same pixels they always did. Glyphs are the
 * kernel's 8x16 VGA fallback font; the Year view halves it to 4x8.
 * tools/checks/ring3calendar-check.py drives all of it.
 */

/* ---- pure date math, shared with tools/checks/check-calendar.sh ---- */
#define CAL_DATE_LEN 10 /* "YYYY-MM-DD", not counting the nul */

static int cal_is_leap(int y){ return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0; }

static int cal_days_in_month(int y, int m){
    static const int days[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    return (m == 2 && cal_is_leap(y)) ? 29 : days[m - 1];
}

/* 0 = Sunday .. 6 = Saturday. y is the full year (2026, not 26). Zeller's
   congruence, Gregorian form, January/February counted as months 13/14
   of the previous year. */
static int cal_dow(int y, int m, int q){
    if (m < 3) { m += 12; y--; }
    int K = y % 100, J = y / 100;
    int h = (q + 13 * (m + 1) / 5 + K + K / 4 + J / 4 + 5 * J) % 7;
    return (h + 6) % 7;
}

/* Zero-padded YYYY-MM-DD, the key EVENTS.TXT and the event dot both use.
   out must hold CAL_DATE_LEN+1 bytes. */
static void cal_date_str(int y, int m, int d, char *out){
    out[0] = '0' + (y / 1000) % 10;
    out[1] = '0' + (y / 100) % 10;
    out[2] = '0' + (y / 10) % 10;
    out[3] = '0' + y % 10;
    out[4] = '-';
    out[5] = '0' + (m / 10) % 10;
    out[6] = '0' + m % 10;
    out[7] = '-';
    out[8] = '0' + (d / 10) % 10;
    out[9] = '0' + d % 10;
    out[10] = 0;
}

/* Seconds since 1970-01-01 (what SYS_TIME returns) to a civil date. */
static void cal_ymd_from_epoch(unsigned t, int *y, int *m, int *d){
    long z = (long)(t / 86400u) + 719468;
    long era = z / 146097;
    long doe = z - era * 146097;
    long yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    long yy = yoe + era * 400;
    long doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    long mp = (5 * doy + 2) / 153;
    *d = (int)(doy - (153 * mp + 2) / 5 + 1);
    *m = (int)(mp < 10 ? mp + 3 : mp - 9);
    *y = (int)(yy + (*m <= 2));
}

#ifndef CALENDAR_MATH_ONLY

#include "jtsys.h"
#include "../drivers/vgafont.h"

#define T (-32) /* calendar.h's gui_app_dy() inside a dock window: same offsets, same pixels */

#define BG     0x00FAF8F6 /* GUI_BG */
#define ACCENT 0x00A0553F
#define INK    0x001C1C1E
#define DIM    0x00A39C92
#define HINT   0x00807468
#define TITLE  0x0085144B
#define SEL    0x00EDE6DC
#define RULE   0x00DDD9D3
#define PILL   0x00ECE6DE
#define WHITE  0x00FFFFFF

#define VIEW_DAY   0
#define VIEW_WEEK  1
#define VIEW_MONTH 2
#define VIEW_YEAR  3

#define EVENTS_MAX     64
#define EVENT_TEXT_MAX 40

struct event { char date[CAL_DATE_LEN + 2]; char text[EVENT_TEXT_MAX]; };
extern char _user_end[];

static const char *MONTHS[12] = {"January", "February", "March", "April", "May", "June",
                                 "July", "August", "September", "October", "November", "December"};
static const char *WD[7] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
static const char *WD_FULL[7] = {"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};
static const char *VIEW_NAMES[4] = {"Day", "Week", "Month", "Year"};

static struct jt_window_info win JT_DATA = {0, 0, 0, 0};
static struct event *events JT_DATA = 0;
static int count JT_DATA = 0;
static int ty JT_DATA = 2026, tm JT_DATA = 1, td JT_DATA = 1;   /* today */
static int vy JT_DATA = 2026, vm JT_DATA = 1, sel_d JT_DATA = 1; /* the selected day */
static int view JT_DATA = VIEW_MONTH;
static int editing JT_DATA = 0;
static char entry[EVENT_TEXT_MAX] JT_DATA = {0};
static int entry_len JT_DATA = 0;

/* ---- drawing, the 8x16 VGA font and a few shapes ---- */
static void rect(int x, int y, int w, int h, unsigned c) {
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > (int)win.width)  w = (int)win.width - x;
    if (y + h > (int)win.height) h = (int)win.height - y;
    for (int yy = 0; yy < h; yy++) {
        unsigned *row = win.pixels + (unsigned)(y + yy) * win.width + (unsigned)x;
        for (int xx = 0; xx < w; xx++) row[xx] = c;
    }
}
static void put(int x, int y, unsigned c) {
    if (x < 0 || x >= (int)win.width || y < 0 || y >= (int)win.height) return;
    win.pixels[(unsigned)y * win.width + (unsigned)x] = c;
}
static void glyph(unsigned char ch, int x, int y, unsigned fg) {
    if (ch < VGAFONT_FIRST || ch > VGAFONT_LAST) ch = '?';
    const unsigned char *g = vgafont_glyphs + (ch - VGAFONT_FIRST) * 16;
    for (int r = 0; r < 16; r++)
        for (int c = 0; c < 8; c++)
            if (g[r] & (0x80 >> c)) put(x + c, y + r, fg);
}
static void text(const char *s, int x, int y, unsigned fg) {
    for (; *s; s++, x += 8) glyph((unsigned char)*s, x, y, fg);
}
static int text_w(const char *s) { int n = 0; while (*s++) n++; return n * 8; }
/* The Year view's digits: the same glyph at half size (4x8), each output
   pixel the OR of a 2x2 block so thin strokes survive. */
static void glyph_half(unsigned char ch, int x, int y, unsigned fg) {
    if (ch < VGAFONT_FIRST || ch > VGAFONT_LAST) ch = '?';
    const unsigned char *g = vgafont_glyphs + (ch - VGAFONT_FIRST) * 16;
    for (int r = 0; r < 8; r++) {
        unsigned char bits = g[2 * r] | g[2 * r + 1];
        for (int c = 0; c < 4; c++)
            if (bits & (0xC0 >> (2 * c))) put(x + c, y + r, fg);
    }
}
static void text_half(const char *s, int x, int y, unsigned fg) {
    for (; *s; s++, x += 4) glyph_half((unsigned char)*s, x, y, fg);
}
static void fill_circle(int cx, int cy, int r, unsigned c) {
    for (int dy = -r; dy <= r; dy++)
        for (int dx = -r; dx <= r; dx++)
            if (dx * dx + dy * dy <= r * r) put(cx + dx, cy + dy, c);
}
/* A pill from centre (x0,cy) to centre (x1,cy), radius r. */
static void capsule(int x0, int x1, int cy, int r, unsigned c) {
    rect(x0, cy - r, x1 - x0 + 1, 2 * r + 1, c);
    fill_circle(x0, cy, r, c);
    fill_circle(x1, cy, r, c);
}
/* Word-wrapped text in the 8x16 font, 18px lines, clipped to w by h. */
static void wrapped(const char *s, int x, int y, int w, int h, unsigned fg) {
    int cols = w / 8, lines = h / 18;
    if (cols < 1 || lines < 1) return;
    for (int line = 0; *s && line < lines; line++) {
        int len = 0; /* characters of s that go on this line: whole words while they fit */
        for (;;) {
            int wl = 0; while (s[len + wl] && s[len + wl] != ' ') wl++;
            if (len && len + 1 + wl > cols) break;
            if (!len && wl > cols) wl = cols;
            len = len ? len + 1 + wl : wl;
            if (!s[len]) break;
        }
        for (int i = 0; i < len; i++) glyph((unsigned char)s[i], x + i * 8, y + line * 18, fg);
        s += len;
        while (*s == ' ') s++;
    }
}

/* ---- small string builders, no libc ---- */
static int cal_put(char *out, int p, const char *s){ while (*s) out[p++] = *s++; out[p] = 0; return p; }
static int cal_put_num(char *out, int p, int v){
    char t[12]; int n = 0;
    if (v == 0) t[n++] = '0';
    while (v > 0) { t[n++] = (char)('0' + v % 10); v /= 10; }
    while (n) out[p++] = t[--n];
    out[p] = 0; return p;
}
static int cal_put_mon3(char *out, int p, int m){
    const char *s = MONTHS[m - 1];
    for (int i = 0; i < 3 && s[i]; i++) out[p++] = s[i];
    out[p] = 0; return p;
}
static void say(const char *pfx, const char *s) { /* one line, one write: what the check reads */
    char line[80]; int l = 0;
    while (*pfx && l < 70) line[l++] = *pfx++;
    while (*s && l < 78) line[l++] = *s++;
    line[l++] = '\n';
    jt_write(1, line, (unsigned)l);
}
static void say_num(const char *pfx, int v) {
    char num[12]; cal_put_num(num, 0, v);
    say(pfx, num);
}

/* ---- EVENTS.TXT ---- */
static int str_eq(const char *a, const char *b){ while (*a && *a == *b) { a++; b++; } return *a == *b; }

static int find(const char *ds){
    for (int i = 0; i < count; i++) if (str_eq(events[i].date, ds)) return i;
    return -1;
}

static void take_line(const char *line, int n){
    if (n < CAL_DATE_LEN || count >= EVENTS_MAX) return;
    struct event *e = &events[count];
    int i = 0, j = 0;
    while (i < n && line[i] != '|' && j < CAL_DATE_LEN) e->date[j++] = line[i++];
    e->date[j] = 0;
    if (i < n && line[i] == '|') i++;
    j = 0;
    while (i < n && j < EVENT_TEXT_MAX - 1) e->text[j++] = line[i++];
    e->text[j] = 0;
    count++;
}

static void load(void) {
    count = 0;
    int fd = jt_open("EVENTS.TXT", JT_O_RDONLY);
    if (fd < 0) return;
    char chunk[128], line[CAL_DATE_LEN + EVENT_TEXT_MAX + 4];
    int p = 0, n;
    while ((n = jt_read(fd, chunk, sizeof chunk)) > 0) {
        for (int i = 0; i < n; i++) {
            char ch = chunk[i];
            if (ch == '\r') continue;
            if (ch == '\n') { take_line(line, p); p = 0; continue; }
            if (p < (int)sizeof line) line[p++] = ch;
        }
    }
    if (p) take_line(line, p); /* last line had no newline */
    jt_close(fd);
}

static int save(void) {
    int fd = jt_open("EVENTS.TXT", JT_O_WRONLY | JT_O_CREAT | JT_O_TRUNC);
    if (fd < 0) { jt_write(1, "calendar: save failed\n", 22); return 0; }
    char line[CAL_DATE_LEN + EVENT_TEXT_MAX + 4];
    for (int i = 0; i < count; i++) {
        int l = cal_put(line, 0, events[i].date);
        line[l++] = '|';
        l = cal_put(line, l, events[i].text);
        line[l++] = '\n';
        jt_write(fd, line, (unsigned)l);
    }
    if (jt_close(fd) < 0) { jt_write(1, "calendar: save failed\n", 22); return 0; }
    say_num("calendar: saved ", count);
    return 1;
}

/* One event per date: overwrite the existing line for ds in place. */
static void set_event(const char *ds, const char *txt){
    int idx = find(ds);
    if (idx < 0) {
        if (count >= EVENTS_MAX) return;
        idx = count++;
        cal_put(events[idx].date, 0, ds);
    }
    int c = 0;
    for (; txt[c] && c < EVENT_TEXT_MAX - 1; c++) events[idx].text[c] = txt[c];
    events[idx].text[c] = 0;
    save();
    say("calendar: event ", ds);
}

static int has_event(int y, int m, int d){
    char ds[CAL_DATE_LEN + 1]; cal_date_str(y, m, d, ds);
    return find(ds) >= 0;
}

/* ---- date stepping, rolling month and year ends ---- */
static void add_days(int *y, int *m, int *d, int delta){
    *d += delta;
    while (*d < 1) { if (--*m < 1) { *m = 12; --*y; } *d += cal_days_in_month(*y, *m); }
    while (*d > cal_days_in_month(*y, *m)) { *d -= cal_days_in_month(*y, *m); if (++*m > 12) { *m = 1; ++*y; } }
}
static void add_months(int *y, int *m, int *d, int delta){
    int k = (*y) * 12 + (*m - 1) + delta;
    *y = k / 12; *m = k % 12 + 1;
    if (*d > cal_days_in_month(*y, *m)) *d = cal_days_in_month(*y, *m);
}

static void read_today(void){
    unsigned now = 0; jt_time(&now);
    cal_ymd_from_epoch(now, &ty, &tm, &td);
}

/* ---- the views, calendar.h's own layout ---- */
static void draw_header(void){
    int seg_w = 64, seg_h = 22, x = 20, y = T + 48;
    capsule(x + seg_h / 2, x + seg_w * 4 - seg_h / 2, y + seg_h / 2, seg_h / 2, PILL);
    for (int i = 0; i < 4; i++) {
        int sx = x + i * seg_w, on = i == view;
        if (on) capsule(sx + seg_h / 2, sx + seg_w - seg_h / 2, y + seg_h / 2, seg_h / 2 - 2, ACCENT);
        text(VIEW_NAMES[i], sx + (seg_w - text_w(VIEW_NAMES[i])) / 2, y + 3, on ? WHITE : HINT);
    }
    const char *hint = "1-4 view   left/right step   [ ] day   enter edits   t today";
    int hint_x = (int)win.width - 20 - text_w(hint);
    if (hint_x >= x + seg_w * 4 + 12) text(hint, hint_x, y + 3, HINT);
}

static void draw_title(const char *title){
    text(title, ((int)win.width - text_w(title)) / 2, T + 92, TITLE);
}

static void draw_month(void){
    int first = cal_dow(vy, vm, 1), n = cal_days_in_month(vy, vm);
    int rows = (first + n + 6) / 7;
    int cell_w = ((int)win.width - 16) / 7;
    if (cell_w > 72) cell_w = 72;
    int grid_w = 7 * cell_w;
    int x0 = ((int)win.width - grid_w) / 2;
    int y0 = T + 150;
    int cell_h = ((int)win.height - y0 - 6) / rows;
    if (cell_h > 44) cell_h = 44;
    if (cell_h < 32) cell_h = 32;

    char title[24]; int p = cal_put(title, 0, MONTHS[vm - 1]);
    p = cal_put(title, p, " "); cal_put_num(title, p, vy);
    draw_title(title);

    for (int c = 0; c < 7; c++)
        text(WD[c], x0 + c * cell_w + (cell_w - text_w(WD[c])) / 2, T + 122, (c == 0 || c == 6) ? DIM : HINT);
    rect(x0, T + 142, grid_w, 1, RULE);

    for (int d = 1; d <= n; d++) {
        int idx = first + d - 1, row = idx / 7, col = idx % 7;
        int cx = x0 + col * cell_w + cell_w / 2, cy = y0 + row * cell_h + cell_h / 2;
        int today = (vy == ty && vm == tm && d == td);
        char num[4]; cal_put_num(num, 0, d);
        if (d == sel_d) rect(x0 + col * cell_w + 2, y0 + row * cell_h + 2, cell_w - 4, cell_h - 4, SEL);
        if (today) fill_circle(cx, cy, 15, ACCENT);
        text(num, cx - text_w(num) / 2, cy - 8, today ? WHITE : ((col == 0 || col == 6) ? DIM : INK));
        if (has_event(vy, vm, d)) fill_circle(cx, cy + 14, 2, today ? WHITE : ACCENT);
    }
}

static void draw_week(void){
    int sy = vy, sm = vm, sd = sel_d;
    add_days(&sy, &sm, &sd, -cal_dow(sy, sm, sd));
    int ey = sy, em = sm, ed = sd;
    add_days(&ey, &em, &ed, 6);

    char title[40]; int p = cal_put_mon3(title, 0, sm);
    p = cal_put(title, p, " "); p = cal_put_num(title, p, sd);
    if (sy != ey) { p = cal_put(title, p, ", "); p = cal_put_num(title, p, sy); }
    p = cal_put(title, p, " - ");
    if (em != sm) { p = cal_put_mon3(title, p, em); p = cal_put(title, p, " "); }
    p = cal_put_num(title, p, ed); p = cal_put(title, p, ", "); cal_put_num(title, p, ey);
    draw_title(title);

    int col_w = ((int)win.width - 40) / 7, x0 = ((int)win.width - col_w * 7) / 2;
    int top = T + 118, bottom = (int)win.height - 8;
    rect(x0, T + 142, col_w * 7, 1, RULE);
    int y = sy, m = sm, d = sd;
    for (int c = 0; c < 7; c++) {
        int cx = x0 + c * col_w;
        int sel = (y == vy && m == vm && d == sel_d);
        int today = (y == ty && m == tm && d == td);
        if (sel) rect(cx + 2, T + 146, col_w - 4, bottom - (T + 146), SEL);
        if (c) rect(cx, T + 146, 1, bottom - (T + 146), PILL);
        text(WD[c], cx + (col_w - text_w(WD[c])) / 2, top + 4, (c == 0 || c == 6) ? DIM : HINT);
        char num[4]; cal_put_num(num, 0, d);
        int ncx = cx + col_w / 2, ncy = T + 166;
        if (today) fill_circle(ncx, ncy, 14, ACCENT);
        text(num, ncx - text_w(num) / 2, ncy - 8, today ? WHITE : ((c == 0 || c == 6) ? DIM : INK));
        char ds[CAL_DATE_LEN + 1]; cal_date_str(y, m, d, ds);
        int e = find(ds);
        if (e >= 0) {
            rect(cx + 6, T + 190, 3, 18, ACCENT);
            wrapped(events[e].text, cx + 12, T + 190, col_w - 18, bottom - (T + 190), INK);
        }
        add_days(&y, &m, &d, 1);
    }
}

static void draw_day(void){
    int y = vy, m = vm, d = sel_d;
    char title[48]; int p = cal_put(title, 0, WD_FULL[cal_dow(y, m, d)]);
    p = cal_put(title, p, ", "); p = cal_put(title, p, MONTHS[m - 1]);
    p = cal_put(title, p, " "); p = cal_put_num(title, p, d);
    p = cal_put(title, p, ", "); cal_put_num(title, p, y);
    draw_title(title);
    int w = (int)win.width;
    if (y == ty && m == tm && d == td) {
        const char *t = "Today";
        int tw = text_w(t) + 20;
        capsule((w - tw) / 2 + 11, (w + tw) / 2 - 11, T + 127, 11, ACCENT);
        text(t, (w - tw) / 2 + 10, T + 119, WHITE);
    }
    rect(40, T + 150, w - 80, 1, RULE);
    char ds[CAL_DATE_LEN + 1]; cal_date_str(y, m, d, ds);
    int e = find(ds);
    if (e >= 0) {
        rect(60, T + 170, 4, 40, ACCENT);
        text("Event", 76, T + 170, HINT);
        wrapped(events[e].text, 76, T + 192, w - 152, (int)win.height - (T + 192) - 8, INK);
    } else {
        const char *a = "No event.", *b = "Press enter to add one.";
        text(a, (w - text_w(a)) / 2, T + 180, INK);
        text(b, (w - text_w(b)) / 2, T + 204, HINT);
    }
}

static void draw_year(void){
    char title[8]; cal_put_num(title, 0, vy);
    draw_title(title);
    int ww = (int)win.width, top = T + 116, bottom = (int)win.height - 6;
    int mw = (ww - 40) / 4, mh = (bottom - top) / 3;
    for (int mo = 1; mo <= 12; mo++) {
        int gx = 20 + ((mo - 1) % 4) * mw, gy = top + ((mo - 1) / 4) * mh;
        int cur = mo == vm;
        int col_w = (mw - 16) / 7;
        int gx0 = gx + (mw - col_w * 7) / 2;
        text(MONTHS[mo - 1], gx0 + 2, gy, cur ? TITLE : HINT);
        int row_top = gy + 20, row_h = (mh - 24) / 6;
        if (row_h < 4) row_h = 4;
        int first = cal_dow(vy, mo, 1), n = cal_days_in_month(vy, mo);
        for (int d = 1; d <= n; d++) {
            int idx = first + d - 1, r = idx / 7, c = idx % 7;
            int cx = gx0 + c * col_w + col_w / 2, cy = row_top + r * row_h + row_h / 2;
            int today = (vy == ty && mo == tm && d == td);
            int sel = cur && d == sel_d;
            int ev = has_event(vy, mo, d);
            unsigned fg = today ? WHITE : ev ? ACCENT : ((c == 0 || c == 6) ? DIM : INK);
            int half = row_h / 2;
            if (sel) rect(cx - col_w / 2 + 1, cy - half, col_w - 2, row_h, SEL);
            if (today) fill_circle(cx, cy, half > 1 ? half : 2, ACCENT);
            if (row_h >= 9) {
                char num[4]; int nl = cal_put_num(num, 0, d);
                text_half(num, cx - nl * 4 / 2, cy - 4, fg);
            } else if (!today) {
                rect(cx - 1, cy - 1, 3, 3, fg);
            }
        }
    }
}

static void draw(void){
    rect(0, 0, (int)win.width, (int)win.height, BG);
    if (editing) {
        char ds[CAL_DATE_LEN + 1]; cal_date_str(vy, vm, sel_d, ds);
        text(ds, 20, T + 52, HINT);
        text("type the event, enter saves, esc cancels:", 20, T + 72, HINT);
        rect(20, T + 96, (int)win.width - 40, 20, WHITE);
        text(entry, 24, T + 98, INK);
        jt_write(1, "calendarprompt\n", 15); /* one per redraw: what the keystroke checks count */
        return;
    }
    draw_header();
    if (view == VIEW_DAY) draw_day();
    else if (view == VIEW_WEEK) draw_week();
    else if (view == VIEW_YEAR) draw_year();
    else draw_month();
}

static void begin_edit(void){
    editing = 1; entry_len = 0; entry[0] = 0;
    char ds[CAL_DATE_LEN + 1]; cal_date_str(vy, vm, sel_d, ds);
    int e = find(ds);
    if (e >= 0) { int c = 0; while (events[e].text[c] && c < EVENT_TEXT_MAX - 1) { entry[c] = events[e].text[c]; c++; } entry_len = c; entry[c] = 0; }
}

static void finish_edit(void){
    editing = 0;
    entry[entry_len] = 0;
    if (entry_len == 0) return;
    char ds[CAL_DATE_LEN + 1]; cal_date_str(vy, vm, sel_d, ds);
    set_event(ds, entry);
}

/* Returns 1 when the window should close. */
static int on_key(int k){
    if (editing) {
        if (k == JT_KEY_ESC) editing = 0;
        else if (k == JT_KEY_ENTER) finish_edit();
        else if (k == 8) { if (entry_len > 0) entry[--entry_len] = 0; }
        else if (k >= 32 && k < 127 && entry_len < EVENT_TEXT_MAX - 1) { entry[entry_len++] = (char)k; entry[entry_len] = 0; }
        return 0;
    }
    if (k == JT_KEY_ESC) return 1;
    if (k >= '1' && k <= '4') { view = k - '1'; say_num("calendar: view ", view); return 0; }
    int step = 0;
    if (k == JT_KEY_LEFT || k == JT_KEY_UP || k == 'a') step = -1;
    else if (k == JT_KEY_RIGHT || k == JT_KEY_DOWN || k == 'd') step = 1;
    if (step) {
        if (view == VIEW_DAY) add_days(&vy, &vm, &sel_d, step);
        else if (view == VIEW_WEEK) add_days(&vy, &vm, &sel_d, 7 * step);
        else if (view == VIEW_YEAR) add_months(&vy, &vm, &sel_d, 12 * step);
        else add_months(&vy, &vm, &sel_d, step);
    } else if (k == 't') {
        read_today();
        vy = ty; vm = tm; sel_d = td;
    } else if (k == '[') add_days(&vy, &vm, &sel_d, -1);
    else if (k == ']') add_days(&vy, &vm, &sel_d, 1);
    else if (k == JT_KEY_ENTER) begin_edit();
    return 0;
}

__attribute__((section(".text.start"), used))
void _start(int argc, char **argv) {
    (void)argc; (void)argv;
    if (jt_window_open(&win) != 0 || !win.pixels) {
        jt_write(2, "calendar: no window\n", 20);
        jt_exit(1);
    }
    {   /* one line, one write: what tools/checks/ring3calendar-check.py asserts on */
        char line[48]; int l = cal_put(line, 0, "calendar: ring-3 window ");
        l = cal_put_num(line, l, (int)win.width); line[l++] = 'x';
        l = cal_put_num(line, l, (int)win.height); line[l++] = '\n';
        jt_write(1, line, (unsigned)l);
    }

    events = (struct event *)(((unsigned long)_user_end + 15ul) & ~15ul);
    read_today();
    vy = ty; vm = tm; sel_d = td;
    {   /* the date and weekday this program computed from SYS_TIME, for the check to compare */
        char line[48]; int l = cal_put(line, 0, "calendar: today ");
        char ds[CAL_DATE_LEN + 1]; cal_date_str(ty, tm, td, ds);
        l = cal_put(line, l, ds); l = cal_put(line, l, " dow ");
        l = cal_put_num(line, l, cal_dow(ty, tm, td)); line[l++] = '\n';
        jt_write(1, line, (unsigned)l);
    }
    load();
    say_num("calendar: loaded ", count);
    draw();

    unsigned flags = JT_POLL_PRESENT;
    for (;;) {
        struct jt_event ev;
        int r = jt_window_poll(&ev, flags);
        flags = 0;
        if (r == -11 /* -EAGAIN */) { jt_sched_yield(); continue; }
        if (r != 1) break;

        if (ev.kind == JT_EV_CLICK) {  /* the titlebar X, or anywhere in the window */
            if (!editing) break;
            editing = 0; draw(); flags = JT_POLL_PRESENT; continue;
        }
        if (ev.kind != JT_EV_KEY) { flags = JT_POLL_PRESENT; continue; }
        if (on_key(ev.a)) break;
        draw(); flags = JT_POLL_PRESENT;
    }
    jt_write(1, "calendar: closed\n", 17);
    jt_exit(0);
}

#endif /* CALENDAR_MATH_ONLY */
