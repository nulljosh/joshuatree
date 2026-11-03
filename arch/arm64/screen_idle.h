/* Display only: USB, networking and apps keep running while HDMI is blank. 7.5.3: with no keyboard or mouse attached
   (input_present() is 0) the screen never sleeps, since nothing could wake it: the first board test went dark for good. */
#define SCREEN_IDLE_CS 30000u   /* the default wait; 13.4.0: Settings, Display, Screen saver after picks it (settings_sleep_cs) */
unsigned settings_sleep_cs(void);
extern int saver_hold;   /* main.c: fb_flush leaves the scanout alone while the saver owns it */
/* 13.4.0, Energy Saver against burn-in: at the wait the saver starts, the time small and grey on black, jumping to a new
   place every 10 s so no pixel stays lit. It draws on the scanout only (fb_scan), never on fb, so waking is one fb_flush.
   SAVER_CS after that the HDMI goes dark and the loop naps, as before. A screen drawn straight to the scanout gets no saver. */
#define SAVER_CS 30000u
static int saver_on;
static unsigned saver_t0, saver_moved, saver_seed = 12345;
static void saver_paint(void) {
    char t[12]; unsigned long utc = cal_utc(); unsigned h = 0, m = 0, mo, dy; int n = 0;
    if (utc) { clock_local(utc, &h, &m, &mo, &dy); unsigned h12 = h % 12 ? h % 12 : 12; if (h12 >= 10) t[n++] = (char)('0' + h12 / 10); t[n++] = (char)('0' + h12 % 10); t[n++] = ':'; t[n++] = (char)('0' + m / 10); t[n++] = (char)('0' + m % 10); }
    t[n] = 0;
    for (unsigned long i = 0; i < (unsigned long)fb_pitch * fb_h; i++) fb_scan[i] = 0;
    if (n && text_ok) {
        int px = sg(360), tw = text_width(2, t, px), W = (int)fb_w, H = (int)fb_h;
        saver_seed = saver_seed * 1103515245u + 12345u;
        int x = W > tw + 40 ? 20 + (int)((saver_seed >> 8) % (unsigned)(W - tw - 40)) : 0;
        saver_seed = saver_seed * 1103515245u + 12345u;
        int y = H > px / 5 + 60 ? px / 10 + 20 + (int)((saver_seed >> 8) % (unsigned)(H - px / 5 - 60)) : H / 2;
        text_draw(2, t, x, y, px, fb_color(0x00505050), fb_scan, fb_pitch, W, H);
    }
    saver_moved = notice_now();
}
static unsigned screen_last;
static int screen_asleep;
/* 11.0.0, the tree menu's Sleep: the screen goes dark a second later, busy or not, as a Mac's does. The deadline does
   not look at screen_last, so the release of the very click or key that chose it cannot cancel it. */
static unsigned sleep_at;
void screen_sleep_soon(void) { sleep_at = notice_now() + 100; if (!sleep_at) sleep_at = 1; }
static void screen_activity(void) {
    screen_last = notice_now();
    if (saver_on) { saver_on = 0; saver_hold = 0; fb_flush(0, 0, (int)fb_w, (int)fb_h); uart_puts("screen: saver off\n"); }
    if (screen_asleep && screen_blank(0)) {
        screen_asleep = 0;
        uart_puts("screen: awake\n");
    }
}
static void screen_tick(void) {
    unsigned now = notice_now();
    int asked = 0;
    if (sleep_at) { if ((int)(now - sleep_at) < 0) return; sleep_at = 0; asked = input_present(); }
    if (!asked) {
        if (ask_pending() || brick_live || !input_present()) { screen_last = now; return; }
        unsigned wait = settings_sleep_cs();
        if (screen_asleep || !wait || now - screen_last < wait) return;
        if (fb && fb_scan && fb_scan != fb) {   /* the saver first, then dark */
            if (!saver_on) { saver_on = 1; saver_hold = 1; saver_t0 = now; saver_paint(); uart_puts("screen: saver on\n"); return; }
            if (now - saver_t0 < SAVER_CS) { if (now - saver_moved >= 1000) saver_paint(); return; }
        }
    }
    screen_last = now;   /* a refused request gets another five minutes, never a tight retry loop */
    if (screen_blank(1)) {
        screen_asleep = 1;
        uart_puts("screen: asleep\n");
    } else uart_puts("screen: blank refused\n");
}
/* the self-tests' idle: the wait has passed, then the saver's time too, so the screen goes dark now (13.4.0) */
__attribute__((unused)) static void screen_force_idle(void) {
    screen_last -= SCREEN_IDLE_CS > settings_sleep_cs() ? SCREEN_IDLE_CS : settings_sleep_cs(); screen_tick();
    if (saver_on) { saver_t0 -= SAVER_CS; screen_tick(); }
}
/* 10.1.0, deep idle: the Pi's loop spins flat out, thousands of passes a second, for a screen nobody is looking at.
   Once the screen sleeps, each pass first naps NAP_MS, so the whole machine runs at about 50 passes a second. USB is
   still polled every pass (the only wake path), so a key or a click still wakes it, 20 ms late at worst. Not while
   something is working (busy: a question, a sound, an app, a download), which keeps its full speed. The Pi 4 has no
   suspend to RAM; this is as deep as it goes without one. */
#define NAP_MS 20
static unsigned nap_count;
static void screen_nap(int busy) {
    if (!screen_asleep || busy) return;
    nap_count++;
    nap_ms(NAP_MS);
}
