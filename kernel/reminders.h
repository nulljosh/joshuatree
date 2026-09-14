/* v52 (0.52.0): a real persisted checklist. Same shape as editor.h's
   Notes (a plain text file on the real FAT disk via vfs_replace_file/
   vfs_read_file) and settings.c's SETTINGS.TXT (write-through on every
   mutation, no separate "Apply" step), no new persistence mechanism
   invented for this. Format is one line per item: a '0'/'1' done flag,
   a space, then the text, e.g. "1 buy milk\n". */
#define REMINDERS_MAX 24
#define REMINDERS_TEXT_MAX 48
static char reminders_text[REMINDERS_MAX][REMINDERS_TEXT_MAX];
static int reminders_done[REMINDERS_MAX];
static int reminders_count = 0;
static int reminders_loaded = 0;

static void reminders_load(void) {
    if (reminders_loaded) return;
    reminders_loaded = 1;
    static char buf[2048];
    int n = vfs_read_file("REMINDERS.TXT", buf, sizeof(buf) - 1);
    if (n < 0) n = 0;
    buf[n] = 0;
    int i = 0;
    reminders_count = 0;
    while (i < n && reminders_count < REMINDERS_MAX) {
        int done = (buf[i] == '1');
        i += 2; /* flag + the space after it */
        int j = 0;
        while (i < n && buf[i] != '\n' && j < REMINDERS_TEXT_MAX - 1) reminders_text[reminders_count][j++] = buf[i++];
        reminders_text[reminders_count][j] = 0;
        reminders_done[reminders_count] = done;
        reminders_count++;
        if (i < n && buf[i] == '\n') i++;
    }
}

static void reminders_save(void) {
    static char buf[2048];
    int n = 0;
    for (int idx = 0; idx < reminders_count; idx++) {
        buf[n++] = reminders_done[idx] ? '1' : '0';
        buf[n++] = ' ';
        const char *s = reminders_text[idx];
        while (*s && n < (int)sizeof(buf) - 2) buf[n++] = *s++;
        buf[n++] = '\n';
    }
    vfs_replace_file("REMINDERS.TXT", buf, (unsigned int)n);
}

/* Same lightweight get_key() text-capture loop gui_launch_chat already
   uses for its one-line message box, not editor.h's fuller caret/
   scroll/font-picker machinery, this only ever needs one short line. */
static void reminders_add_new(void) {
    static char msg[REMINDERS_TEXT_MAX];
    unsigned int n = 0;
    mouse_click_edge_sync(); /* v67: a click cancels, same as esc, so no screen in this GUI is keyboard-only to leave */
    for (;;) {
        window_clear(GUI_BG);
        gui_draw_app_titlebar("Reminders");
        font_draw_string("type the reminder, enter to add, esc or click to cancel:", 20, 52, 0x0075726E, -1);
        window_rect(20, 76, (int)window_width() - 40, 20, 0x00FFFFFF);
        msg[n] = 0;
        font_draw_string(msg, 24, 78, 0x001C1C1E, -1);
        int k = get_key_or_click();
        if (k == KEY_ESC || k == KEY_CLICK) return;
        if (k == KEY_ENTER) break;
        if (k == '\b') { if (n > 0) n--; }
        else if (n < sizeof(msg) - 1 && k >= 32 && k < 127) msg[n++] = (char)k;
    }
    msg[n] = 0;
    if (n == 0) return;
    for (unsigned int c = 0; c <= n; c++) reminders_text[reminders_count][c] = msg[c];
    reminders_done[reminders_count] = 0;
    reminders_count++;
    reminders_save();
}

/* Same up/down/select list contract Trash (v39) already established,
   plus 'a' to add (the only real new interaction) and 'd' to delete;
   space toggles done, saving through on every mutation. */
static void gui_launch_reminders(void) {
    reminders_load();
    int sel = 0;
    for (;;) {
        window_clear(GUI_BG);
        gui_draw_app_titlebar("Reminders");
        if (!reminders_count) {
            font_draw_string("No reminders yet.", 20, 70, 0x001C1C1E, -1);
            font_draw_string("Press a to add one.", 20, 94, 0x00807468, -1);
        } else {
            font_draw_string("up/down to pick   space toggles done   d deletes   a adds   esc closes", 20, 52, 0x00807468, -1);
            for (int i = 0; i < reminders_count; i++) {
                int y = 84 + i * 22;
                if (i == sel) window_rect(16, y - 4, (int)window_width() - 32, 20, 0x00EDE6DC);
                font_draw_string(reminders_done[i] ? "[x]" : "[ ]", 28, y, reminders_done[i] ? 0x002F7B4F : 0x001C1C1E, -1);
                font_draw_string(reminders_text[i], 60, y, reminders_done[i] ? 0x00A39C92 : 0x001C1C1E, -1);
            }
        }
        sleep_ticks(5);
        mouse_click_edge_sync();
        int k = get_key_or_click();
        if (k == KEY_ESC || k == KEY_CLICK) return;
        if (k == 'a') { if (reminders_count < REMINDERS_MAX) reminders_add_new(); continue; }
        if (!reminders_count) continue;
        if (k == KEY_UP && sel > 0) sel--;
        else if (k == KEY_DOWN && sel < reminders_count - 1) sel++;
        else if (k == ' ') { reminders_done[sel] = !reminders_done[sel]; reminders_save(); }
        else if (k == 'd') {
            for (int j = sel; j < reminders_count - 1; j++) {
                reminders_done[j] = reminders_done[j + 1];
                for (int c = 0; c < REMINDERS_TEXT_MAX; c++) reminders_text[j][c] = reminders_text[j + 1][c];
            }
            reminders_count--;
            if (sel >= reminders_count && sel > 0) sel--;
            reminders_save();
        }
    }
}
