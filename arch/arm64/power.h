/* Keyboard power controls. Only a locally confirmed chord can reach the watchdog. */
static int power_pending;   /* 1 shutdown, 2 restart */
static void power_apply(int off);
static void power_message(const char *s) {
    term_output(1); uart_puts(s); term_output(0);
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
    spot_close(); clock_close(); calc_close(); pane_open(&term_p);
    if (ask_pending()) { power_message("Wait for Samantha to finish, then try again.\n"); return 1; }
    power_pending = code == 107 ? 1 : 2;
    power_message(power_pending == 1 ? "Shut down? Unsaved conversations will be lost.\n" : "Restart? Unsaved conversations will be lost.\n");
    power_message("Enter confirms. Escape cancels.\n");
    return 1;
}
#ifndef POWER_HOST_TEST
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
