/* Display only: USB, networking and apps keep running while HDMI is blank. 7.5.3: with no keyboard or mouse attached
   (input_present() is 0) the screen never sleeps, since nothing could wake it: the first board test went dark for good. */
#define SCREEN_IDLE_CS 30000u
static unsigned screen_last;
static int screen_asleep;
/* 11.0.0, the tree menu's Sleep: the screen goes dark a second later, busy or not, as a Mac's does. The deadline does
   not look at screen_last, so the release of the very click or key that chose it cannot cancel it. */
static unsigned sleep_at;
void screen_sleep_soon(void) { sleep_at = notice_now() + 100; if (!sleep_at) sleep_at = 1; }
static void screen_activity(void) {
    screen_last = notice_now();
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
        if (screen_asleep || now - screen_last < SCREEN_IDLE_CS) return;
    }
    screen_last = now;   /* a refused request gets another five minutes, never a tight retry loop */
    if (screen_blank(1)) {
        screen_asleep = 1;
        uart_puts("screen: asleep\n");
    } else uart_puts("screen: blank refused\n");
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
