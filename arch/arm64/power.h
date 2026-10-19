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
int ufile_sync(void);   /* ufile.c */
#if defined(PI_BUILD) && !defined(POWER_HOST_TEST)
/* The Pi 4 cannot cut its own power, so a shutdown ends on a plain screen: the mark, "Shut down" and "It is safe to unplug
   the power now" on the wallpaper. */
static void power_halt_screen(void) {
    if (!fb || !text_ok) return;
    int s = (int)window_scale(), W = (int)fb_w, H = (int)fb_h, lw = 320, lh = 150;
    int cx = (W / s - lw) / 2, cy = (H / s - lh) / 2;
    cur_hide();
    fb_rect(0, 0, W, H, wall_get());
    gui_rounded_rect_on_wallpaper(cx, cy, lw, lh, 0x00faf8f4, 14);
    menu_mark_paint(W / 2, (cy + 44) * s, sg(48), 0x001C1C1E);
    const char *a = "Shut down", *b = "It is safe to unplug the power now.";
    gui_icon_text(a, cx + (lw - gui_icon_text_w(a, 2, 1)) / 2, cy + 82, 2, 1, 0x001C1C1E);
    gui_text(b, cx + (lw - gui_text_width(b)) / 2, cy + 116, 0x0075726E);
    fb_flush(0, 0, W, H);
    uart_puts("power: safe to unplug\n");
}
#endif
#if !defined(POWER_HOST_TEST) && !defined(PI_BUILD)
static void power_apply(int off) { power_message(off ? "power: shutdown (this build has no power control)\n" : "power: restart (this build has no power control)\n"); }   /* the virt test build: only the Pi has a watchdog to pull */
#elif !defined(POWER_HOST_TEST)
static void power_apply(int off) {
    power_message(off ? "power: shutdown\n" : "power: restart\n");
#ifndef POWER_NOFLUSH   /* the power check's mutation build leaves this out and must lose the file it wrote */
    int flushed = ufile_sync();   /* ufile.c: what an open app (Notes mid-save) has not closed yet goes to the card first */
    if (flushed < 0) power_message("power: a file would not save\n");
    else { power_message("power: saved open files "); uart_dec((unsigned)flushed); uart_putc('\n'); }
#endif
    if (off) power_halt_screen();
    if (off) {
        unsigned m[] = { 32, 0, 0x00028001, 8, 8, 0, 0, 0 };   /* SD power off through VideoCore */
        for (unsigned i = 0; i < 8; i++) mbox[i] = m[i];
        if (!mbox_call()) power_message("SD power-off not acknowledged; halting.\n");
    }
    for (unsigned n = 0; (REG(UART_FR) & BUSY) && n < 5000000; n++) {}   /* let the final message leave the UART */
    __asm__ volatile ("msr daifset, #15\n dsb sy\n" ::: "memory");
    unsigned long hz; __asm__ volatile ("mrs %0, cntfrq_el0" : "=r"(hz));
    if (off && hz == 54000000UL) for (;;) __asm__ volatile ("wfe");   /* the board's 54 MHz counter (QEMU's differs): stay on the screen, the watchdog halt would blank it */
    unsigned status = REG(0xFE100020UL) & ~0xFFFFFAAAu;
    REG(0xFE100020UL) = 0x5A000000u | status | (off ? 0x555u : 0);
    REG(0xFE100024UL) = 0x5A00000Au;
    REG(0xFE10001CUL) = 0x5A000000u | (REG(0xFE10001CUL) & ~0x30u) | 0x20u;
    __asm__ volatile ("dsb sy" ::: "memory");
    for (;;) __asm__ volatile ("wfe");
}
#endif
