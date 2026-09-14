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

/* Left/right (or up/down, or the Apps folder's own a/d) step a month at
   a time, rolling the year over at either end; t jumps back to today.
   Today is a filled disc behind the day number, the one shape every
   phone calendar has settled on, in the same clay accent as the app's
   own dock tile. Weekends dimmed, not hidden. */
static void gui_launch_calendar(void){
    int ty, tm, td;
    cal_read_today(&ty, &tm, &td);
    int vy = ty, vm = tm;
    const unsigned int accent = 0x00A0553F, ink = 0x001C1C1E, dim = 0x00A39C92, hint = 0x00807468;
    for (;;) {
        window_clear(GUI_BG);
        gui_draw_app_titlebar("Calendar");
        font_draw_string("left/right change month   t today   esc closes", 20, 52, hint, -1);

        int cell_w = 72, cell_h = 44, grid_w = 7 * cell_w;
        int x0 = ((int)window_width() - grid_w) / 2;
        int y0 = 150;

        /* "September 2026", centered over the grid */
        char title[24]; int p = 0;
        for (const char *s = CAL_MONTHS[vm - 1]; *s; s++) title[p++] = *s;
        title[p++] = ' ';
        title[p++] = '0' + (vy / 1000) % 10; title[p++] = '0' + (vy / 100) % 10;
        title[p++] = '0' + (vy / 10) % 10;   title[p++] = '0' + vy % 10;
        title[p] = 0;
        font_draw_string(title, x0 + (grid_w - p * 8) / 2, 92, 0x0085144B, -1);

        for (int c = 0; c < 7; c++)
            font_draw_string(CAL_WD[c], x0 + c * cell_w + (cell_w - 24) / 2, 122, (c == 0 || c == 6) ? dim : hint, -1);
        window_rect(x0, 142, grid_w, 1, 0x00DDD9D3);

        int first = cal_dow(vy, vm, 1), n = cal_days_in_month(vy, vm);
        for (int d = 1; d <= n; d++) {
            int idx = first + d - 1, row = idx / 7, col = idx % 7;
            int cx = x0 + col * cell_w + cell_w / 2, cy = y0 + row * cell_h + cell_h / 2;
            int today = (vy == ty && vm == tm && d == td);
            char num[3]; int len = 0;
            if (d >= 10) num[len++] = '0' + d / 10;
            num[len++] = '0' + d % 10;
            num[len] = 0;
            if (today) gui_fill_circle(cx, cy, 15, accent, GUI_BG);
            font_draw_string(num, cx - len * 4, cy - 8, today ? 0x00FFFFFF : ((col == 0 || col == 6) ? dim : ink), -1);
        }

        sleep_ticks(5);
        mouse_click_edge_sync();
        int k = get_key_or_click();
        if (k == KEY_ESC || k == KEY_CLICK) return;
        if (k == KEY_LEFT || k == KEY_UP || k == 'a') { if (--vm < 1) { vm = 12; vy--; } }
        else if (k == KEY_RIGHT || k == KEY_DOWN || k == 'd') { if (++vm > 12) { vm = 1; vy++; } }
        else if (k == 't') { cal_read_today(&ty, &tm, &td); vy = ty; vm = tm; }
    }
}

#endif /* CALENDAR_MATH_ONLY */
