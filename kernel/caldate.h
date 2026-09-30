/* 1.9.11: the date math the kernel still needs after Calendar itself moved
   to ring 3 (user/calendar.c). Two callers: Samantha's calendar_today tool
   in chat.h, which reads EVENTS.TXT fresh on every call because the ring-3
   program owns the file while it is open, and stocks.h, which turns a quote
   timestamp into a YYYY-MM-DD stamp. The math is the same integer-only
   shape user/calendar.c carries (leap years, days in a month, the
   zero-padded date key), kept in both places on purpose: a ring-3 program
   has no kernel include path, and tools/checks/check-calendar.sh sweeps
   the program's copy, the one that draws the grid, against libc. */
#define CAL_DATE_LEN 10 /* "YYYY-MM-DD", not counting the nul */

static int cal_is_leap(int y){ return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0; }

static int cal_days_in_month(int y, int m){
    static const int days[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    return (m == 2 && cal_is_leap(y)) ? 29 : days[m - 1];
}

/* Same manual digit-building idiom as the rest of this kernel, no sprintf.
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

static int cal_bcd(u8 v){ return (v & 0x0F) + ((v >> 4) * 10); }

/* Today off the RTC, the same BCD registers gui_draw_menubar's clock
   reads (7 = day of month, 8 = month, 9 = two-digit year, 0x32 = century).
   A blank or garbage century byte falls back to 20xx. The ring-3 program
   gets the same day through SYS_TIME instead; syscall.c's
   rtc_epoch_seconds reads these same registers, so the two agree. */
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

/* EVENTS.TXT, one line per date ("YYYY-MM-DD|text"), the file user/calendar.c
   writes. No `_loaded` guard: the ring-3 program may have rewritten the
   file since the last call, so every call reads it again. */
#define CAL_EVENTS_MAX 64
#define CAL_EVENT_TEXT_MAX 40
static char cal_event_date[CAL_EVENTS_MAX][CAL_DATE_LEN + 1];
static char cal_event_text[CAL_EVENTS_MAX][CAL_EVENT_TEXT_MAX];
static int cal_event_count = 0;

static void cal_events_load(void){
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
        while (i < n && buf[i] != '\n') i++; /* an over-long line: drop the rest */
        if (i < n && buf[i] == '\n') i++;
        if (j == 0 && cal_event_date[cal_event_count][0] == 0) continue; /* blank line */
        cal_event_count++;
    }
}

/* First-match scan against the same YYYY-MM-DD string cal_date_str builds;
   -1 when that date has no event. */
static int cal_events_find(const char *datestr){
    for (int i = 0; i < cal_event_count; i++)
        if (strcmp(cal_event_date[i], datestr) == 0) return i;
    return -1;
}
