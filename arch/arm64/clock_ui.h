/* One read-only Clock window, using the desktop's Vancouver time conversion. */
static unsigned long clock_shown = ~0UL;
static void clock_paint(void) {
    unsigned long utc = cal_utc();
    unsigned h = 0, m = 0, mo = 0, d = 0;
    char time[9] = "--:--", date[32] = "Waiting for network time";
    static const char *const months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    if (utc) {
        clock_local(utc, &h, &m, &mo, &d);
        unsigned hour = h % 12 ? h % 12 : 12, n = 0;
        if (hour >= 10) time[n++] = '1';
        time[n++] = '0' + hour % 10; time[n++] = ':';
        time[n++] = '0' + m / 10; time[n++] = '0' + m % 10;
        time[n++] = ' '; time[n++] = h >= 12 ? 'P' : 'A'; time[n++] = 'M'; time[n] = 0;
        n = 0; for (unsigned i = 0; i < 3; i++) date[n++] = months[mo - 1][i];
        date[n++] = ' '; if (d >= 10) date[n++] = '0' + d / 10;
        date[n++] = '0' + d % 10; date[n++] = ','; date[n++] = ' ';
        for (unsigned div = 1000; div; div /= 10) date[n++] = '0' + (unsigned)clock_year / div % 10;
        date[n] = 0;
    }
    int s = (int)window_scale(), center = (win_lx + win_lw / 2) * s;
    cur_hide();
    gui_draw_window_frame(win_lx, win_ly, win_lw, win_lh, "Clock");
    window_rect(win_lx + 8, win_ly + 30, win_lw - 16, win_lh - 38, 0x00faf8f4);
    text_draw(1, time, center - text_width(1, time, 400 * s) / 2, (win_ly + 120) * s,
              400 * s, fb_color(0x001C1C1E), fb, fb_pitch, fb_w, fb_h);
    gui_text(date, win_lx + (win_lw - gui_text_width(date)) / 2, win_ly + 150, 0x001C1C1E);
    gui_text("Vancouver", win_lx + (win_lw - gui_text_width("Vancouver")) / 2, win_ly + 180, 0x0075726E);
    fb_flush(win_lx * s, win_ly * s, win_lw * s, win_lh * s);
    cur_show(); clock_shown = utc / 60;
    con_quiet = 1; uart_puts("clock: "); uart_puts(time); uart_putc(' '); uart_puts(date); uart_putc('\n');
}
static void clock_open(void) {
    if (clock_live || !con_under) return;
    calc_close(); console_close(); clock_live = 1;
    clock_paint(); uart_puts("clock open\n");
}
static void clock_close(void) {
    if (!clock_live) return;
    clock_live = 0; con_live = 1; console_close(); pane_open(cp);
    uart_puts("clock closed\n");
}
static void clock_tick(void) {
    if (clock_live && !spot_live && cal_utc() / 60 != clock_shown) clock_paint();
}
