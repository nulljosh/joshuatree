/* Display only: USB, networking and apps keep running while HDMI is blank. */
#define SCREEN_IDLE_CS 30000u
static unsigned screen_last;
static int screen_asleep;
static void screen_activity(void) {
    screen_last = notice_now();
    if (screen_asleep && screen_blank(0)) {
        screen_asleep = 0;
        uart_puts("screen: awake\n");
    }
}
static void screen_tick(void) {
    unsigned now = notice_now();
    if (ask_pending() || brick_live) { screen_last = now; return; }
    if (screen_asleep || now - screen_last < SCREEN_IDLE_CS) return;
    screen_last = now;   /* a refused request gets another five minutes, never a tight retry loop */
    if (screen_blank(1)) {
        screen_asleep = 1;
        uart_puts("screen: asleep\n");
    } else uart_puts("screen: blank refused\n");
}
