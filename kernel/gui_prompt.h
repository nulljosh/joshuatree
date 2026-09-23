/* v0.76.24 (Sep 2026): Shared text-input prompt loop used by mail.h, calendar.h,
   reminders.h, and calculator.h. Fixes per-keystroke screen-clear bug by splitting
   chrome (titlebar, prompt labels) drawn once before the loop from content (text box,
   typed text) redrawn only per keystroke, following the pattern editor.h (v0.76.10)
   and contacts.h (v0.76.23) established.

   gui_prompt_line_input: prompt at y=52, input at y=76 (both plus
   gui_app_dy()). Returns 1 on enter, 0 on esc/click. */

/* Standard layout: titlebar + prompt at y=52 + input box at y=76.
   Handles single-line text input for mail, reminders, and calculator apps. */
static int gui_prompt_line_input(const char *title, const char *prompt, char *out, int max) {
    unsigned int n = 0;
    out[0] = 0;
    mouse_click_edge_sync();

    /* Draw chrome only once, before the loop. */
    window_clear(GUI_BG);
    gui_draw_app_titlebar(title);
    int T = gui_app_dy();
    font_draw_string(prompt, 20, T + 52, 0x0075726E, -1);

    for (;;) {
        /* Redraw only the content area (text box and typed text), not the chrome. */
        window_rect(20, T + 76, (int)window_width() - 40, 20, 0x00FFFFFF);
        out[n] = 0;
        font_draw_string(out, 24, T + 78, 0x001C1C1E, -1);
        serial_puts("guiprompt\n"); /* discriminating marker for regression tests */
        int k = get_key_or_click();
        if (k == KEY_ESC || k == KEY_CLICK) return 0;
        if (k == KEY_ENTER) break;
        if (k == '\b') { if (n > 0) n--; }
        else if ((int)n < max - 1 && k >= 32 && k < 127) out[n++] = (char)k;
    }
    out[n] = 0;
    return 1;
}
