/* Keyboard power controls. Only a locally confirmed chord can reach the watchdog. */
static int power_pending;   /* 1 shutdown, 2 restart */
static void power_apply(int off);
static void power_message(const char *s) {
    term_output(1); uart_puts(s); term_output(0);
}
/* The one entry the confirmed chords and the tree menu's Restart and Shut Down rows share: the Terminal comes up, a busy
   Samantha refuses, otherwise the same Enter-confirms prompt. Nothing here touches the watchdog. */
static void power_ask(int off) {
    spot_close(); brick_close(); clock_close(); calc_close(); pane_open(&term_p);
    if (ask_pending()) { power_message("Wait for Samantha to finish, then try again.\n"); return; }
    power_pending = off ? 1 : 2;
    power_message(off ? "Shut down? Unsaved conversations will be lost.\n" : "Restart? Unsaved conversations will be lost.\n");
    power_message("Enter confirms. Escape cancels.\n");
}
static int power_key(unsigned code) {
    if (power_pending) {
        if (!term_front()) { power_pending = 0; return 0; }
        if (code == 1) { power_pending = 0; power_message("Power action cancelled.\n"); }
        else if (code == 28 || code == 96) {
            int off = power_pending == 1;
            power_pending = 0;
            if (ask_pending()) power_message("Wait for Samantha to finish, then try again.\n");
            else power_apply(off);
        }
        return 1;
    }
    if (!ctrl_held || !alt_held || (code != 111 && code != 107)) return 0;
    power_ask(code == 107);
    return 1;
}
#if !defined(POWER_HOST_TEST) && !defined(PI_BUILD)
static void power_apply(int off) { power_message(off ? "power: shutdown (this build has no power control)\n" : "power: restart (this build has no power control)\n"); }   /* the virt test build: only the Pi has a watchdog to pull */
#elif !defined(POWER_HOST_TEST)
static void power_apply(int off) {
    power_message(off ? "power: shutdown\n" : "power: restart\n");
    /* shortcut: no writable storage yet; flush pending disk writes when SD saving lands. */
    if (off) {
        unsigned m[] = { 32, 0, 0x00028001, 8, 8, 0, 0, 0 };   /* SD power off through VideoCore */
        for (unsigned i = 0; i < 8; i++) mbox[i] = m[i];
        if (!mbox_call()) power_message("SD power-off not acknowledged; halting.\n");
    }
    for (unsigned n = 0; (REG(UART_FR) & BUSY) && n < 5000000; n++) {}   /* let the final message leave the UART */
    __asm__ volatile ("msr daifset, #15\n dsb sy\n" ::: "memory");
    unsigned status = REG(0xFE100020UL) & ~0xFFFFFAAAu;
    REG(0xFE100020UL) = 0x5A000000u | status | (off ? 0x555u : 0);
    REG(0xFE100024UL) = 0x5A00000Au;
    REG(0xFE10001CUL) = 0x5A000000u | (REG(0xFE10001CUL) & ~0x30u) | 0x20u;
    __asm__ volatile ("dsb sy" ::: "memory");
    for (;;) __asm__ volatile ("wfe");
}
#endif
