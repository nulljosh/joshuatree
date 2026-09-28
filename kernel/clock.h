/* Clock app: current time, countdown timer, and alarm. */

#define CLOCK_TIME_X 60
#define CLOCK_TIME_Y 80

static int clock_timer_sec;        /* countdown timer in seconds, 0=idle */
static int clock_timer_paused;
static unsigned int clock_timer_last_draw;
static int clock_alarm_h, clock_alarm_m;
static int clock_alarm_armed;
static unsigned int clock_alarm_fired_until;

static void clock_bcd_to_int(u8 bcd, int *val) {
    *val = (bcd & 0x0F) + ((bcd >> 4) * 10);
}

static void clock_draw_time(void) {
    u8 h, m, s, wd, dom, mon;
    cmos_read_time_stable(&h, &m, &wd, &dom, &mon);
    s = cmos(0);
    int hv, mv, sv;
    clock_bcd_to_int(h, &hv);
    clock_bcd_to_int(m, &mv);
    clock_bcd_to_int(s, &sv);

    char buf[12];
    int pos = 0;
    buf[pos++] = '0' + hv / 10; buf[pos++] = '0' + hv % 10;
    buf[pos++] = ':';
    buf[pos++] = '0' + mv / 10; buf[pos++] = '0' + mv % 10;
    buf[pos++] = ':';
    buf[pos++] = '0' + sv / 10; buf[pos++] = '0' + sv % 10;
    buf[pos] = 0;

    window_rect(20, CLOCK_TIME_Y, (int)window_width() - 40, 40, GUI_BG);
    font_draw_string(buf, CLOCK_TIME_X, CLOCK_TIME_Y + 8, 0x001C1C1E, -1);
}

static void clock_draw_timer(void) {
    int y = CLOCK_TIME_Y + 80;
    window_rect(20, y, (int)window_width() - 40, 60, GUI_BG);

    if (clock_timer_sec > 0) {
        int mins = clock_timer_sec / 60, secs = clock_timer_sec % 60;
        char buf[16];
        int pos = 0;
        if (mins > 0) { buf[pos++] = '0' + mins / 10; buf[pos++] = '0' + mins % 10; buf[pos++] = ':'; }
        buf[pos++] = '0' + secs / 10; buf[pos++] = '0' + secs % 10;
        buf[pos] = 0;
        font_draw_string("Timer: ", 20, y + 10, 0x0075726E, -1);
        font_draw_string(buf, 90, y + 10, 0x001C1C1E, -1);
        const char *status = clock_timer_paused ? "paused" : "running";
        font_draw_string(status, 140, y + 10, 0x00375A4A, -1);
    } else {
        font_draw_string("Timer: off", 20, y + 10, 0x0075726E, -1);
    }

    if (clock_alarm_armed) {
        font_draw_string("Alarm: ", 20, y + 35, 0x0075726E, -1);
        char buf[8];
        buf[0] = '0' + clock_alarm_h / 10; buf[1] = '0' + clock_alarm_h % 10;
        buf[2] = ':';
        buf[3] = '0' + clock_alarm_m / 10; buf[4] = '0' + clock_alarm_m % 10;
        buf[5] = 0;
        font_draw_string(buf, 90, y + 35, 0x001C1C1E, -1);
    }
}

static void gui_launch_clock(void) {
    window_clear(GUI_BG);
    gui_draw_app_titlebar("Clock");
    gui_draw_hint(20, 42, "space starts timer  r resets  a alarm  esc closes", 0x0075726E);

    clock_timer_sec = 0;
    clock_timer_paused = 0;
    clock_timer_last_draw = 0;
    clock_alarm_armed = 0;
    clock_alarm_h = 8;
    clock_alarm_m = 0;
    clock_alarm_fired_until = 0;

    mouse_click_edge_sync();

    for (;;) {
        unsigned int now = ticks();
        if (now - clock_timer_last_draw >= 100 || clock_timer_last_draw == 0) {
            clock_draw_time();
            clock_draw_timer();
            clock_timer_last_draw = now;

            if (clock_timer_sec > 0 && !clock_timer_paused && now % 100 == 0) {
                clock_timer_sec--;
                if (clock_timer_sec == 0) clock_alarm_fired_until = now + 300;
            }

            if (clock_alarm_armed && clock_alarm_fired_until == 0) {
                u8 h, m, wd, dom, mon;
                cmos_read_time_stable(&h, &m, &wd, &dom, &mon);
                int hv, mv;
                clock_bcd_to_int(h, &hv);
                clock_bcd_to_int(m, &mv);
                if (hv == clock_alarm_h && mv == clock_alarm_m) {
                    clock_alarm_fired_until = now + 300;
                }
            }

            if (clock_alarm_fired_until && now < clock_alarm_fired_until) {
                int y = (int)window_height() / 2 - 20;
                window_rect(40, y, (int)window_width() - 80, 40, 0x00A13F3F);
                font_draw_string("ALARM!", 140, y + 12, 0x00F5F0EB, -1);
            }
        }

        int k = kbd_pop();
        if (k >= 0 && !(k & 0x80)) {
            char c = kbd_map(k);
            if (c == 27) return;
            else if (c == ' ') {
                if (clock_timer_sec > 0) clock_timer_paused = !clock_timer_paused;
                else { char buf[16]; if (gui_prompt_line_input("Timer", "Minutes:", buf, 5)) { int m = 0; for (int i = 0; buf[i]; i++) m = m * 10 + (buf[i] - '0'); clock_timer_sec = m * 60; clock_timer_paused = 0; } }
            }
            else if (c == 'r') { clock_timer_sec = 0; clock_timer_paused = 0; }
            else if (c == 'a') {
                char buf[8]; if (gui_prompt_line_input("Alarm", "HH:MM:", buf, 6)) {
                    int h = (buf[0] - '0') * 10 + (buf[1] - '0');
                    int m = (buf[3] - '0') * 10 + (buf[4] - '0');
                    clock_alarm_h = h; clock_alarm_m = m; clock_alarm_armed = 1;
                }
            }
        }

        if (mouse_click_edge()) { gui_close_was_click = 1; return; }

        window_present();
        sleep_ticks(1);
    }
}
