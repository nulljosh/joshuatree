/* v54 (0.54.0): a real month grid. Same file-per-app shape as
   reminders.h, same titlebar, same sleep_ticks/mouse_click_edge_sync/
   get_key_or_click polling loop every screen in this GUI already uses.
   Nothing persisted, nothing fetched: the only input is the RTC, read
   through the exact same cmos() BCD registers gui_draw_menubar's clock
   already trusts (7 = day of month, 8 = month), plus register 9 (two-
   digit year) and 0x32 (century), which the menu bar never needed.

   Day-of-week is Zeller's congruence (Gregorian form, integer only, no
   floating point anywhere in this kernel to lean on), the textbook shape:
   h = (q + 13(m+1)/5 + K + K/4 + J/4 + 5J) mod 7 with January/February
   counted as months 13/14 of the previous year, h = 0 meaning Saturday.
   Converted to 0 = Sunday here so the grid's own column order (Sun..Sat,
   the same order the menu bar's WD[] table already uses) indexes it
   directly. Cross-checked two ways before shipping, see roadmap.md v54:
   against libc's tm_wday for every day 1900-2099 on the host
   (tools/check-calendar.sh, which is why the pure date math sits under
   its own CALENDAR_MATH_ONLY guard, so a host compiler can pull it in
   without the rest of the kernel), and against the RTC's own weekday
   register (cmos 6) inside the real booted kernel. */

static int cal_is_leap(int y){ return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0; }

static int cal_days_in_month(int y, int m){
    static const int days[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    return (m == 2 && cal_is_leap(y)) ? 29 : days[m - 1];
}

/* 0 = Sunday .. 6 = Saturday. y is the full year (2026, not 26). */
static int cal_dow(int y, int m, int q){
    if (m < 3) { m += 12; y--; }
    int K = y % 100, J = y / 100;
    int h = (q + 13 * (m + 1) / 5 + K + K / 4 + J / 4 + 5 * J) % 7;
    return (h + 6) % 7;
}

/* v55 (0.55.0): the same manual digit-building idiom as this file's own
   month title further down ('0' + (vy / 1000) % 10, ...) and reminders.h's
   line-count formatting, no sprintf anywhere in this kernel. Pure integer
   math like the rest of this section, so it sits above the
   CALENDAR_MATH_ONLY guard too: tools/check-calendar.sh links it straight
   into its host-side harness and checks its output against snprintf's
   own "%04d-%02d-%02d" for every date 1900-2099, real proof the exact
   zero-padded format EVENTS.TXT and the grid's event-dot lookup both key
   off of is right, not just "looks right by eye". out must hold
   CAL_DATE_LEN+1 bytes. */
#define CAL_DATE_LEN 10 /* "YYYY-MM-DD", not counting the nul */
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

#ifndef CALENDAR_MATH_ONLY

static const char *CAL_MONTHS[12] = {"January", "February", "March", "April", "May", "June",
                                     "July", "August", "September", "October", "November", "December"};
static const char *CAL_WD[7] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};

static int cal_bcd(u8 v){ return (v & 0x0F) + ((v >> 4) * 10); }

/* Same registers, same BCD decode gui_draw_menubar already does for the
   clock. The century byte (0x32) is what QEMU, v86 and every ACPI-era PC
   fill in; a machine that leaves it blank or garbage falls back to 20xx,
   which is right for the entire lifetime of this kernel so far. */
static void cal_read_today(int *y, int *m, int *d){
    while (cmos(0x0A) & 0x80) {} /* wait out an in-progress RTC update, same as show_time */
    int dom = cal_bcd(cmos(7)), mon = cal_bcd(cmos(8)), yy = cal_bcd(cmos(9)), cc = cal_bcd(cmos(0x32));
    if (cc < 19 || cc > 21) cc = 20;
    if (mon < 1 || mon > 12) mon = 1;
    *y = cc * 100 + yy;
    *m = mon;
    if (dom < 1 || dom > cal_days_in_month(*y, *m)) dom = 1;
    *d = dom;
}

/* v55 (0.55.0): real events, one line per date in EVENTS.TXT
   (`YYYY-MM-DD|text\n`), through the exact same vfs_replace_file/
   vfs_read_file pair reminders.h already established for REMINDERS.TXT.
   Same shape throughout: a fixed-size static array, a `_loaded` guard so
   the file is only ever read once per boot, manual line-parsing (no
   sscanf), and a lightweight get_key()-driven text-capture loop for
   entry, copied almost verbatim from reminders_add_new. One event per
   date, not a list: cal_events_set overwrites a date's existing line in
   place rather than appending a second one, so a day's dot/lookup stays
   a single first-match scan and EVENTS.TXT never grows duplicate dates.
   64 events * up to 51 bytes/line ("YYYY-MM-DD|" + 39 chars of text +
   "\n") is 3264 bytes worst case, hence the 4096 buffer, same headroom
   editor.h's own NOTES.TXT buffer keeps. cal_date_str/CAL_DATE_LEN live
   above, next to the rest of this file's pure date math, so
   tools/check-calendar.sh can exercise the exact same formatting
   function this section matches against. */
#define CAL_EVENTS_MAX 64
#define CAL_EVENT_TEXT_MAX 40
static char cal_event_date[CAL_EVENTS_MAX][CAL_DATE_LEN + 1];
static char cal_event_text[CAL_EVENTS_MAX][CAL_EVENT_TEXT_MAX];
static int cal_event_count = 0;
static int cal_events_loaded = 0;

static void cal_events_load(void){
    if (cal_events_loaded) return;
    cal_events_loaded = 1;
    static char buf[4096];
    int n = vfs_read_file("EVENTS.TXT", buf, sizeof(buf) - 1);
    if (n < 0) n = 0;
    buf[n] = 0;
    int i = 0;
    cal_event_count = 0;
    while (i < n && cal_event_count < CAL_EVENTS_MAX) {
        int j = 0;
        while (i < n && buf[i] != '|' && buf[i] != '\n' && j < CAL_DATE_LEN) cal_event_date[cal_event_count][j++] = buf[i++];
        cal_event_date[cal_event_count][j] = 0;
        if (i < n && buf[i] == '|') i++; /* skip the separator */
        j = 0;
        while (i < n && buf[i] != '\n' && j < CAL_EVENT_TEXT_MAX - 1) cal_event_text[cal_event_count][j++] = buf[i++];
        cal_event_text[cal_event_count][j] = 0;
        if (i < n && buf[i] == '\n') i++;
        cal_event_count++;
    }
}

static void cal_events_save(void){
    static char buf[4096];
    int n = 0;
    for (int idx = 0; idx < cal_event_count; idx++) {
        const char *s = cal_event_date[idx];
        while (*s && n < (int)sizeof(buf) - 2) buf[n++] = *s++;
        buf[n++] = '|';
        s = cal_event_text[idx];
        while (*s && n < (int)sizeof(buf) - 2) buf[n++] = *s++;
        buf[n++] = '\n';
    }
    vfs_replace_file("EVENTS.TXT", buf, (unsigned int)n);
}

/* First-match scan against the same YYYY-MM-DD string cal_date_str
   builds for a grid cell; -1 when that date has no event. */
static int cal_events_find(const char *datestr){
    for (int i = 0; i < cal_event_count; i++)
        if (strcmp(cal_event_date[i], datestr) == 0) return i;
    return -1;
}

/* One event per date (documented above): overwrites the existing line
   for datestr in place rather than appending a second one. */
static void cal_events_set(const char *datestr, const char *text){
    int idx = cal_events_find(datestr);
    if (idx < 0) {
        if (cal_event_count >= CAL_EVENTS_MAX) return;
        idx = cal_event_count++;
        for (int c = 0; c <= CAL_DATE_LEN; c++) cal_event_date[idx][c] = datestr[c];
    }
    int c = 0;
    for (; text[c] && c < CAL_EVENT_TEXT_MAX - 1; c++) cal_event_text[idx][c] = text[c];
    cal_event_text[idx][c] = 0;
    cal_events_save();
}

/* Calendar: Day, Week, Month and Year views over one shared state, the
   Mac Calendar set. Both entry points run it: the multi-window path
   (gui_draw_calendar_content + gui_calendar_on_key, called by gui_run)
   and the blocking gui_launch_calendar the Apps folder and tests use, so
   the two can never drift apart again.

   Keys: 1/2/3/4 pick Day/Week/Month/Year. Left/right (up/down, a/d) step
   by the view's own unit: a day, a week, a month, a year. [ and ] move the
   selected day by one in every view, rolling across month and year ends.
   t jumps to today, enter edits the selected day's one event, esc closes.
   Month is the default view, so the old month-grid contract still holds.

   Everything is laid out from the real window size and gui_app_dy(), so a
   snapped, halved window or the full screen gets the same design. */
#define CAL_VIEW_DAY   0
#define CAL_VIEW_WEEK  1
#define CAL_VIEW_MONTH 2
#define CAL_VIEW_YEAR  3
static const char *CAL_VIEW_NAMES[4] = {"Day", "Week", "Month", "Year"};
static const char *CAL_WD_FULL[7] = {"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};
#define CAL_ACCENT 0x00A0553F
#define CAL_INK    0x001C1C1E
#define CAL_DIM    0x00A39C92
#define CAL_HINT   0x00807468
#define CAL_TITLE  0x0085144B
#define CAL_SEL    0x00EDE6DC
#define CAL_RULE   0x00DDD9D3

static int cal_mw_loaded = 0;
static int cal_mw_ty, cal_mw_tm, cal_mw_td;
static int cal_mw_vy, cal_mw_vm, cal_mw_sel_d;
static int cal_mw_view = CAL_VIEW_MONTH;
static int cal_mw_dayview = 0; /* 1 while typing the selected day's event */
static char cal_mw_buf[CAL_EVENT_TEXT_MAX];
static unsigned int cal_mw_buflen = 0;

static void cal_mw_init(void){
    if (cal_mw_loaded) return;
    cal_mw_loaded = 1;
    cal_read_today(&cal_mw_ty, &cal_mw_tm, &cal_mw_td);
    cal_mw_vy = cal_mw_ty; cal_mw_vm = cal_mw_tm; cal_mw_sel_d = cal_mw_td;
    cal_events_load();
}

/* Pure date stepping, rolling month and year ends. */
static void cal_add_days(int *y, int *m, int *d, int delta){
    *d += delta;
    while (*d < 1) { if (--*m < 1) { *m = 12; --*y; } *d += cal_days_in_month(*y, *m); }
    while (*d > cal_days_in_month(*y, *m)) { *d -= cal_days_in_month(*y, *m); if (++*m > 12) { *m = 1; ++*y; } }
}
static void cal_add_months(int *y, int *m, int *d, int delta){
    int k = (*y) * 12 + (*m - 1) + delta;
    *y = k / 12; *m = k % 12 + 1;
    if (*d > cal_days_in_month(*y, *m)) *d = cal_days_in_month(*y, *m);
}

/* Small string builders; no libc here. */
static int cal_put(char *out, int p, const char *s){ while (*s) out[p++] = *s++; out[p] = 0; return p; }
static int cal_put_num(char *out, int p, int v){
    char t[12]; int n = 0;
    if (v == 0) t[n++] = '0';
    while (v > 0) { t[n++] = (char)('0' + v % 10); v /= 10; }
    while (n) out[p++] = t[--n];
    out[p] = 0; return p;
}
static int cal_put_mon3(char *out, int p, int m){
    const char *s = CAL_MONTHS[m - 1];
    for (int i = 0; i < 3 && s[i]; i++) out[p++] = s[i];
    out[p] = 0; return p;
}

static int cal_has_event(int y, int m, int d){
    char ds[CAL_DATE_LEN + 1];
    cal_date_str(y, m, d, ds);
    return cal_events_find(ds) >= 0;
}

/* Segmented Day | Week | Month | Year control, top left: capsules drawn at
   physical resolution (gui_draw_capsule), so the ends are smooth arcs. The hint for the
   current view sits right-aligned on the same row, so the control costs no
   vertical space. */
static void cal_draw_header(int T){
    int seg_w = 64, seg_h = 22, x = 20, y = T + 48;
    gui_draw_capsule(x + seg_h / 2, y + seg_h / 2, x + seg_w * 4 - seg_h / 2, y + seg_h / 2, seg_h / 2, 0x00ECE6DE, GUI_BG);
    for (int i = 0; i < 4; i++) {
        int sx = x + i * seg_w, on = i == cal_mw_view;
        if (on) gui_draw_capsule(sx + seg_h / 2, y + seg_h / 2, sx + seg_w - seg_h / 2, y + seg_h / 2, seg_h / 2 - 2, CAL_ACCENT, 0x00ECE6DE);
        font_draw_string(CAL_VIEW_NAMES[i], sx + (seg_w - font_string_width(CAL_VIEW_NAMES[i])) / 2, y + 3, on ? 0x00FFFFFF : CAL_HINT, -1);
    }
    const char *hint = "1-4 view   left/right step   [ ] day   enter edits   t today";
    font_draw_string(hint, (int)window_width() - 20 - font_string_width(hint), y + 3, CAL_HINT, -1);
}

static void cal_draw_title(const char *title, int T){
    font_draw_string(title, ((int)window_width() - font_string_width(title)) / 2, T + 92, CAL_TITLE, -1);
}

/* The month grid. Rows are sized from the real window height, capped at
   44px: a six-week month needs six of them, and at a fixed 44px from y=150
   the fifth week was cut in half by the bottom of a 385px dock window and
   the sixth never drawn. Text is centred by its real rendered width. */
static void cal_draw_month_view(int T){
    int vy = cal_mw_vy, vm = cal_mw_vm;
    int first = cal_dow(vy, vm, 1), n = cal_days_in_month(vy, vm);
    int rows = (first + n + 6) / 7;
    int cell_w = 72, grid_w = 7 * cell_w;
    int x0 = ((int)window_width() - grid_w) / 2;
    int y0 = T + 150;
    int cell_h = ((int)window_height() - y0 - 6) / rows;
    if (cell_h > 44) cell_h = 44;
    if (cell_h < 32) cell_h = 32;

    char title[24]; int p = cal_put(title, 0, CAL_MONTHS[vm - 1]);
    p = cal_put(title, p, " "); cal_put_num(title, p, vy);
    cal_draw_title(title, T);

    for (int c = 0; c < 7; c++)
        font_draw_string(CAL_WD[c], x0 + c * cell_w + (cell_w - font_string_width(CAL_WD[c])) / 2, T + 122, (c == 0 || c == 6) ? CAL_DIM : CAL_HINT, -1);
    window_rect(x0, T + 142, grid_w, 1, CAL_RULE);

    for (int d = 1; d <= n; d++) {
        int idx = first + d - 1, row = idx / 7, col = idx % 7;
        int cx = x0 + col * cell_w + cell_w / 2, cy = y0 + row * cell_h + cell_h / 2;
        int today = (vy == cal_mw_ty && vm == cal_mw_tm && d == cal_mw_td);
        char num[4]; cal_put_num(num, 0, d);
        if (d == cal_mw_sel_d) window_rect(x0 + col * cell_w + 2, y0 + row * cell_h + 2, cell_w - 4, cell_h - 4, CAL_SEL);
        if (today) gui_fill_circle(cx, cy, 15, CAL_ACCENT, d == cal_mw_sel_d ? CAL_SEL : GUI_BG);
        font_draw_string(num, cx - font_string_width(num) / 2, cy - 8, today ? 0x00FFFFFF : ((col == 0 || col == 6) ? CAL_DIM : CAL_INK), -1);
        if (cal_has_event(vy, vm, d)) gui_fill_circle(cx, cy + 14, 2, today ? 0x00FFFFFF : CAL_ACCENT, GUI_BG);
    }
}

/* Seven columns, Sunday first, for the week holding the selected day:
   weekday, date (today on the accent disc), and that day's event text
   wrapped inside its own column. */
static void cal_draw_week_view(int T){
    int sy = cal_mw_vy, sm = cal_mw_vm, sd = cal_mw_sel_d;
    cal_add_days(&sy, &sm, &sd, -cal_dow(sy, sm, sd));
    int ey = sy, em = sm, ed = sd;
    cal_add_days(&ey, &em, &ed, 6);

    char title[40]; int p = cal_put_mon3(title, 0, sm);
    p = cal_put(title, p, " "); p = cal_put_num(title, p, sd);
    if (sy != ey) { p = cal_put(title, p, ", "); p = cal_put_num(title, p, sy); }
    p = cal_put(title, p, " - ");
    if (em != sm) { p = cal_put_mon3(title, p, em); p = cal_put(title, p, " "); }
    p = cal_put_num(title, p, ed); p = cal_put(title, p, ", "); cal_put_num(title, p, ey);
    cal_draw_title(title, T);

    int col_w = ((int)window_width() - 40) / 7, x0 = ((int)window_width() - col_w * 7) / 2;
    int top = T + 118, bottom = (int)window_height() - 8;
    window_rect(x0, T + 142, col_w * 7, 1, CAL_RULE);
    int y = sy, m = sm, d = sd;
    for (int c = 0; c < 7; c++) {
        int cx = x0 + c * col_w;
        int sel = (y == cal_mw_vy && m == cal_mw_vm && d == cal_mw_sel_d);
        int today = (y == cal_mw_ty && m == cal_mw_tm && d == cal_mw_td);
        if (sel) window_rect(cx + 2, T + 146, col_w - 4, bottom - (T + 146), CAL_SEL);
        if (c) window_rect(cx, T + 146, 1, bottom - (T + 146), 0x00ECE6DE);
        font_draw_string(CAL_WD[c], cx + (col_w - font_string_width(CAL_WD[c])) / 2, top + 4, (c == 0 || c == 6) ? CAL_DIM : CAL_HINT, -1);
        char num[4]; cal_put_num(num, 0, d);
        int ncx = cx + col_w / 2, ncy = T + 166;
        if (today) gui_fill_circle(ncx, ncy, 14, CAL_ACCENT, sel ? CAL_SEL : GUI_BG);
        font_draw_string(num, ncx - font_string_width(num) / 2, ncy - 8, today ? 0x00FFFFFF : ((c == 0 || c == 6) ? CAL_DIM : CAL_INK), -1);
        char ds[CAL_DATE_LEN + 1]; cal_date_str(y, m, d, ds);
        int e = cal_events_find(ds);
        if (e >= 0) {
            window_rect(cx + 6, T + 190, 3, 18, CAL_ACCENT);
            render_wrapped_text(cal_event_text[e], cx + 12, T + 190, col_w - 18, bottom - (T + 190), CAL_INK);
        }
        cal_add_days(&y, &m, &d, 1);
    }
}

/* One day: its full date as the title, a Today tag, and the one event
   this calendar stores per day, or how to add it. */
static void cal_draw_day_view(int T){
    int y = cal_mw_vy, m = cal_mw_vm, d = cal_mw_sel_d;
    char title[48]; int p = cal_put(title, 0, CAL_WD_FULL[cal_dow(y, m, d)]);
    p = cal_put(title, p, ", "); p = cal_put(title, p, CAL_MONTHS[m - 1]);
    p = cal_put(title, p, " "); p = cal_put_num(title, p, d);
    p = cal_put(title, p, ", "); cal_put_num(title, p, y);
    cal_draw_title(title, T);
    int w = (int)window_width();
    if (y == cal_mw_ty && m == cal_mw_tm && d == cal_mw_td) {
        const char *t = "Today";
        int tw = font_string_width(t) + 20;
        gui_draw_capsule((w - tw) / 2 + 11, T + 127, (w + tw) / 2 - 11, T + 127, 11, CAL_ACCENT, GUI_BG);
        font_draw_string(t, (w - tw) / 2 + 10, T + 119, 0x00FFFFFF, -1);
    }
    window_rect(40, T + 150, w - 80, 1, CAL_RULE);
    char ds[CAL_DATE_LEN + 1]; cal_date_str(y, m, d, ds);
    int e = cal_events_find(ds);
    if (e >= 0) {
        window_rect(60, T + 170, 4, 40, CAL_ACCENT);
        font_draw_string("Event", 76, T + 170, CAL_HINT, -1);
        render_wrapped_text(cal_event_text[e], 76, T + 192, w - 152, (int)window_height() - (T + 192) - 8, CAL_INK);
    } else {
        const char *a = "No event.", *b = "Press enter to add one.";
        font_draw_string(a, (w - font_string_width(a)) / 2, T + 180, CAL_INK, -1);
        font_draw_string(b, (w - font_string_width(b)) / 2, T + 204, CAL_HINT, -1);
    }
}

/* Twelve mini months, four across, three down. At window scale 2 the day
   numbers are drawn with the 16 physical px face (8 logical px tall, the
   same coverage glyphs Weather draws with), so a full year fits a dock
   window legibly. At scale 1 there is no room for digits, so each day is a
   small square instead: same positions, same today/selected/event marks. */
static void cal_draw_year_view(int T){
    int vy = cal_mw_vy;
    char title[8]; cal_put_num(title, 0, vy);
    cal_draw_title(title, T);
    int sc = (int)window_scale();
    int ww = (int)window_width(), top = T + 116, bottom = (int)window_height() - 6;
    int mw = (ww - 40) / 4, mh = (bottom - top) / 3;
    for (int mo = 1; mo <= 12; mo++) {
        int gx = 20 + ((mo - 1) % 4) * mw, gy = top + ((mo - 1) / 4) * mh;
        int cur = mo == cal_mw_vm;
        const char *name = CAL_MONTHS[mo - 1];
        int col_w = (mw - 16) / 7;
        int gx0 = gx + (mw - col_w * 7) / 2;
        font_draw_string(name, gx0 + 2, gy, cur ? CAL_TITLE : CAL_HINT, -1);
        int row_top = gy + 20, row_h = (mh - 24) / 6;
        if (row_h < 4) row_h = 4;
        int first = cal_dow(vy, mo, 1), n = cal_days_in_month(vy, mo);
        for (int d = 1; d <= n; d++) {
            int idx = first + d - 1, r = idx / 7, c = idx % 7;
            int cx = gx0 + c * col_w + col_w / 2, cy = row_top + r * row_h + row_h / 2;
            int today = (vy == cal_mw_ty && mo == cal_mw_tm && d == cal_mw_td);
            int sel = cur && d == cal_mw_sel_d;
            int ev = cal_has_event(vy, mo, d);
            unsigned int fg = today ? 0x00FFFFFF : ev ? CAL_ACCENT : ((c == 0 || c == 6) ? CAL_DIM : CAL_INK);
            int half = row_h / 2;
            if (sel) window_rect(cx - col_w / 2 + 1, cy - half, col_w - 2, row_h, CAL_SEL);
            if (today) gui_fill_circle(cx, cy, half > 1 ? half : 2, CAL_ACCENT, sel ? CAL_SEL : GUI_BG);
            if (sc >= 2 && row_h >= 9) {
                char num[4]; cal_put_num(num, 0, d);
                int lw = wx_text_lw(num, 0, 0, 1);
                wx_text(num, cx - lw / 2, cy - 3, 0, 0, 1, fg);
            } else if (!today) {
                window_rect(cx - 1, cy - 1, 3, 3, fg);
            }
        }
    }
}

static void gui_draw_calendar_content(void){
    cal_mw_init();
    window_clear(GUI_BG);
    gui_draw_app_titlebar("Calendar");
    int T = gui_app_dy();
    if (cal_mw_dayview) {
        char datestr[CAL_DATE_LEN + 1];
        cal_date_str(cal_mw_vy, cal_mw_vm, cal_mw_sel_d, datestr);
        font_draw_string(datestr, 20, T + 52, CAL_HINT, -1);
        font_draw_string("type the event, enter saves, esc cancels:", 20, T + 72, CAL_HINT, -1);
        window_rect(20, T + 96, (int)window_width() - 40, 20, 0x00FFFFFF);
        cal_mw_buf[cal_mw_buflen] = 0;
        font_draw_string(cal_mw_buf, 24, T + 98, CAL_INK, -1);
        return;
    }
    cal_draw_header(T);
    if (cal_mw_view == CAL_VIEW_DAY) cal_draw_day_view(T);
    else if (cal_mw_view == CAL_VIEW_WEEK) cal_draw_week_view(T);
    else if (cal_mw_view == CAL_VIEW_YEAR) cal_draw_year_view(T);
    else cal_draw_month_view(T);
}

/* Returns 1 when this window should close (esc from a view); esc from
   the event editor just cancels back to the view. */
static int gui_calendar_on_key(int k){
    cal_mw_init();
    if (cal_mw_dayview) {
        if (k == KEY_ESC) { cal_mw_dayview = 0; return 0; }
        if (k == KEY_ENTER) {
            cal_mw_buf[cal_mw_buflen] = 0;
            if (cal_mw_buflen > 0) {
                char datestr[CAL_DATE_LEN + 1];
                cal_date_str(cal_mw_vy, cal_mw_vm, cal_mw_sel_d, datestr);
                cal_events_set(datestr, cal_mw_buf);
            }
            cal_mw_dayview = 0;
            return 0;
        }
        if (k == '\b') { if (cal_mw_buflen > 0) cal_mw_buflen--; return 0; }
        if (k >= 32 && k < 127 && cal_mw_buflen < CAL_EVENT_TEXT_MAX - 1) cal_mw_buf[cal_mw_buflen++] = (char)k;
        return 0;
    }
    if (k == KEY_ESC) return 1;
    if (k >= '1' && k <= '4') { cal_mw_view = k - '1'; return 0; }
    int step = 0;
    if (k == KEY_LEFT || k == KEY_UP || k == 'a') step = -1;
    else if (k == KEY_RIGHT || k == KEY_DOWN || k == 'd') step = 1;
    if (step) {
        if (cal_mw_view == CAL_VIEW_DAY) cal_add_days(&cal_mw_vy, &cal_mw_vm, &cal_mw_sel_d, step);
        else if (cal_mw_view == CAL_VIEW_WEEK) cal_add_days(&cal_mw_vy, &cal_mw_vm, &cal_mw_sel_d, 7 * step);
        else if (cal_mw_view == CAL_VIEW_YEAR) cal_add_months(&cal_mw_vy, &cal_mw_vm, &cal_mw_sel_d, 12 * step);
        else cal_add_months(&cal_mw_vy, &cal_mw_vm, &cal_mw_sel_d, step);
    } else if (k == 't') {
        cal_read_today(&cal_mw_ty, &cal_mw_tm, &cal_mw_td);
        cal_mw_vy = cal_mw_ty; cal_mw_vm = cal_mw_tm; cal_mw_sel_d = cal_mw_td;
    } else if (k == '[') cal_add_days(&cal_mw_vy, &cal_mw_vm, &cal_mw_sel_d, -1);
    else if (k == ']') cal_add_days(&cal_mw_vy, &cal_mw_vm, &cal_mw_sel_d, 1);
    else if (k == KEY_ENTER) {
        cal_mw_dayview = 1; cal_mw_buflen = 0; cal_mw_buf[0] = 0;
        char datestr[CAL_DATE_LEN + 1]; cal_date_str(cal_mw_vy, cal_mw_vm, cal_mw_sel_d, datestr);
        int existing = cal_events_find(datestr);
        if (existing >= 0) { unsigned int c = 0; while (cal_event_text[existing][c] && c < CAL_EVENT_TEXT_MAX - 1) { cal_mw_buf[c] = cal_event_text[existing][c]; c++; } cal_mw_buflen = c; }
    }
    return 0;
}

/* The blocking entry point (Apps folder, tests): the same state machine,
   driven by get_key_or_click until esc or a click closes it. */
static void gui_launch_calendar(void){
    cal_mw_init();
    cal_mw_dayview = 0;
    for (;;) {
        gui_draw_calendar_content();
        window_present();
        sleep_ticks(5);
        mouse_click_edge_sync();
        int k = get_key_or_click();
        if (k == KEY_CLICK) return;
        if (gui_calendar_on_key(k)) return;
    }
}

#endif /* CALENDAR_MATH_ONLY */
