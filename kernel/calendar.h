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

/* v0.76.24: Full-screen view/add for one day, same takeover-then-return shape as
   reminders_add_new (copied almost verbatim): pre-fills the edit line
   with that date's existing event text if there is one, so enter with
   no changes is a no-op re-save and typing overwrites it, one event per
   day. Esc cancels without writing anything, even over existing text.
   Now uses shared gui_prompt_line_input_with_date to fix per-keystroke
   window_clear bug. */
static void cal_day_view(int y, int m, int d){
    char datestr[CAL_DATE_LEN + 1];
    cal_date_str(y, m, d, datestr);
    int existing = cal_events_find(datestr);
    static char msg[CAL_EVENT_TEXT_MAX];
    unsigned int n = 0;
    if (existing >= 0) while (cal_event_text[existing][n] && n < sizeof(msg) - 1) { msg[n] = cal_event_text[existing][n]; n++; }
    msg[n] = 0;
    if (!gui_prompt_line_input_with_date("Calendar", datestr,
                                        "type the event, enter to save, esc or click to cancel:", msg, CAL_EVENT_TEXT_MAX)) return;
    if (msg[0] == 0) return;
    cal_events_set(datestr, msg);
}

/* The month grid both Calendar paths draw (the blocking one below and the
   multi-window gui_draw_calendar_content). Rows are sized from the real
   window height, capped at the original 44px: a six-week month needs six
   of them, and at a fixed 44px from y=150 the fifth week was cut in half
   by the bottom of a 385px dock window and the sixth never drawn. Text is
   centred by its real rendered width, not 8px per character. */
static void cal_draw_month(int vy, int vm, int sel_d, int ty, int tm, int td){
    const unsigned int accent = 0x00A0553F, ink = 0x001C1C1E, dim = 0x00A39C92, hint = 0x00807468;
    int T = gui_app_dy();
    font_draw_string("left/right month   [ ] pick a day   enter opens it   t today   esc closes", 20, T + 52, hint, -1);
    int first = cal_dow(vy, vm, 1), n = cal_days_in_month(vy, vm);
    int rows = (first + n + 6) / 7;
    int cell_w = 72, grid_w = 7 * cell_w;
    int x0 = ((int)window_width() - grid_w) / 2;
    int y0 = T + 150;
    int cell_h = ((int)window_height() - y0 - 6) / rows;
    if (cell_h > 44) cell_h = 44;
    if (cell_h < 32) cell_h = 32;

    /* "September 2026", centred over the grid */
    char title[24]; int p = 0;
    for (const char *s = CAL_MONTHS[vm - 1]; *s; s++) title[p++] = *s;
    title[p++] = ' ';
    title[p++] = '0' + (vy / 1000) % 10; title[p++] = '0' + (vy / 100) % 10;
    title[p++] = '0' + (vy / 10) % 10;   title[p++] = '0' + vy % 10;
    title[p] = 0;
    font_draw_string(title, x0 + (grid_w - font_string_width(title)) / 2, T + 92, 0x0085144B, -1);

    for (int c = 0; c < 7; c++)
        font_draw_string(CAL_WD[c], x0 + c * cell_w + (cell_w - font_string_width(CAL_WD[c])) / 2, T + 122, (c == 0 || c == 6) ? dim : hint, -1);
    window_rect(x0, T + 142, grid_w, 1, 0x00DDD9D3);

    for (int d = 1; d <= n; d++) {
        int idx = first + d - 1, row = idx / 7, col = idx % 7;
        int cx = x0 + col * cell_w + cell_w / 2, cy = y0 + row * cell_h + cell_h / 2;
        int today = (vy == ty && vm == tm && d == td);
        char num[3]; int len = 0;
        if (d >= 10) num[len++] = '0' + d / 10;
        num[len++] = '0' + d % 10;
        num[len] = 0;
        if (d == sel_d) window_rect(x0 + col * cell_w + 2, y0 + row * cell_h + 2, cell_w - 4, cell_h - 4, 0x00EDE6DC);
        if (today) gui_fill_circle(cx, cy, 15, accent, GUI_BG);
        font_draw_string(num, cx - font_string_width(num) / 2, cy - 8, today ? 0x00FFFFFF : ((col == 0 || col == 6) ? dim : ink), -1);

        char datestr[CAL_DATE_LEN + 1];
        cal_date_str(vy, vm, d, datestr);
        if (cal_events_find(datestr) >= 0) gui_fill_circle(cx, cy + 14, 2, today ? 0x00FFFFFF : accent, GUI_BG);
    }
}

/* Left/right (or up/down, or the Apps folder's own a/d) step a month at
   a time, rolling the year over at either end; t jumps back to today.
   Today is a filled disc behind the day number, the one shape every
   phone calendar has settled on, in the same clay accent as the app's
   own dock tile. Weekends dimmed, not hidden.

   v55 adds a day cursor (sel_d, starting on today) moved with [ and ]
   (deliberately not the arrow keys, which this screen already spends on
   month stepping): a light square behind the selected cell, independent
   of the "today" accent disc so both can show on the same day at once.
   Enter opens cal_day_view for whichever day is selected. Changing month
   clamps sel_d into the new month's real day count instead of resetting
   it to 1, so picking the 30th and stepping a month still lands on a
   real day; t re-syncs sel_d to today the same way it re-syncs vy/vm. */
static void gui_launch_calendar(void){
    int ty, tm, td;
    cal_read_today(&ty, &tm, &td);
    int vy = ty, vm = tm, sel_d = td;
    cal_events_load();
    for (;;) {
        window_clear(GUI_BG);
        gui_draw_app_titlebar("Calendar");
        cal_draw_month(vy, vm, sel_d, ty, tm, td);
        int n = cal_days_in_month(vy, vm);

        sleep_ticks(5);
        mouse_click_edge_sync();
        int k = get_key_or_click();
        if (k == KEY_ESC || k == KEY_CLICK) return;
        if (k == KEY_LEFT || k == KEY_UP || k == 'a') {
            if (--vm < 1) { vm = 12; vy--; }
            if (sel_d > cal_days_in_month(vy, vm)) sel_d = cal_days_in_month(vy, vm);
        }
        else if (k == KEY_RIGHT || k == KEY_DOWN || k == 'd') {
            if (++vm > 12) { vm = 1; vy++; }
            if (sel_d > cal_days_in_month(vy, vm)) sel_d = cal_days_in_month(vy, vm);
        }
        else if (k == 't') { cal_read_today(&ty, &tm, &td); vy = ty; vm = tm; sel_d = td; }
        else if (k == '[') { if (sel_d > 1) sel_d--; }
        else if (k == ']') { if (sel_d < n) sel_d++; }
        else if (k == KEY_ENTER) cal_day_view(vy, vm, sel_d);
    }
}

/* v0.75.0 (multi-window batch 2): same real-state split reminders.h just
   established -- gui_launch_calendar above is untouched, this is a
   second, separate state machine over the same shared data functions
   (cal_read_today/cal_events_*), so month/day-cursor position and
   day-view-edit-mode persist across repaints and across losing focus to
   another open window. */
static int cal_mw_loaded = 0;
static int cal_mw_ty, cal_mw_tm, cal_mw_td;
static int cal_mw_vy, cal_mw_vm, cal_mw_sel_d;
static int cal_mw_dayview = 0;
static char cal_mw_buf[CAL_EVENT_TEXT_MAX];
static unsigned int cal_mw_buflen = 0;

static void cal_mw_init(void){
    if (cal_mw_loaded) return;
    cal_mw_loaded = 1;
    cal_read_today(&cal_mw_ty, &cal_mw_tm, &cal_mw_td);
    cal_mw_vy = cal_mw_ty; cal_mw_vm = cal_mw_tm; cal_mw_sel_d = cal_mw_td;
    cal_events_load();
}

static void gui_draw_calendar_content(void){
    cal_mw_init();
    window_clear(GUI_BG);
    gui_draw_app_titlebar("Calendar");
    if (cal_mw_dayview) {
        const unsigned int ink = 0x001C1C1E, hint = 0x00807468;
        int T = gui_app_dy();
        char datestr[CAL_DATE_LEN + 1];
        cal_date_str(cal_mw_vy, cal_mw_vm, cal_mw_sel_d, datestr);
        font_draw_string(datestr, 20, T + 52, hint, -1);
        font_draw_string("type the event, enter saves, esc cancels:", 20, T + 72, hint, -1);
        window_rect(20, T + 96, (int)window_width() - 40, 20, 0x00FFFFFF);
        cal_mw_buf[cal_mw_buflen] = 0;
        font_draw_string(cal_mw_buf, 24, T + 98, ink, -1);
        return;
    }
    cal_draw_month(cal_mw_vy, cal_mw_vm, cal_mw_sel_d, cal_mw_ty, cal_mw_tm, cal_mw_td);
}

/* Returns 1 when this window should close (esc from the month grid); esc
   from day-view just cancels back to the grid, matching cal_day_view's
   own esc contract, not a whole-window close. */
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
    int n = cal_days_in_month(cal_mw_vy, cal_mw_vm);
    if (k == KEY_LEFT || k == KEY_UP || k == 'a') {
        if (--cal_mw_vm < 1) { cal_mw_vm = 12; cal_mw_vy--; }
        if (cal_mw_sel_d > cal_days_in_month(cal_mw_vy, cal_mw_vm)) cal_mw_sel_d = cal_days_in_month(cal_mw_vy, cal_mw_vm);
    } else if (k == KEY_RIGHT || k == KEY_DOWN || k == 'd') {
        if (++cal_mw_vm > 12) { cal_mw_vm = 1; cal_mw_vy++; }
        if (cal_mw_sel_d > cal_days_in_month(cal_mw_vy, cal_mw_vm)) cal_mw_sel_d = cal_days_in_month(cal_mw_vy, cal_mw_vm);
    } else if (k == 't') {
        cal_read_today(&cal_mw_ty, &cal_mw_tm, &cal_mw_td);
        cal_mw_vy = cal_mw_ty; cal_mw_vm = cal_mw_tm; cal_mw_sel_d = cal_mw_td;
    } else if (k == '[') { if (cal_mw_sel_d > 1) cal_mw_sel_d--; }
    else if (k == ']') { if (cal_mw_sel_d < n) cal_mw_sel_d++; }
    else if (k == KEY_ENTER) {
        cal_mw_dayview = 1; cal_mw_buflen = 0; cal_mw_buf[0] = 0;
        char datestr[CAL_DATE_LEN + 1]; cal_date_str(cal_mw_vy, cal_mw_vm, cal_mw_sel_d, datestr);
        int existing = cal_events_find(datestr);
        if (existing >= 0) { unsigned int c = 0; while (cal_event_text[existing][c] && c < CAL_EVENT_TEXT_MAX - 1) { cal_mw_buf[c] = cal_event_text[existing][c]; c++; } cal_mw_buflen = c; }
    }
    return 0;
}

#endif /* CALENDAR_MATH_ONLY */
