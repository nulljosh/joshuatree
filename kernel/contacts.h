/* v70 (0.64.0): Contacts. Same shape as mail.h, reminders.h, and calendar.h:
   a plain text file on the real FAT disk (CONTACTS.TXT), a fixed-size
   static array, write-through on every mutation, manual line parsing, no
   sscanf, no new persistence mechanism invented for this. This kernel has
   no directory/CardDAV sync backend and that's deliberately out of scope,
   same call Mail's own offline-only design already made: this is a local
   contacts app, a real persisted list you can view, add, and delete from. */
#define CONTACTS_MAX 32
#define CONTACTS_NAME_MAX 32
#define CONTACTS_PHONE_MAX 24
#define CONTACTS_EMAIL_MAX 40

typedef struct {
    char name[CONTACTS_NAME_MAX];
    char phone[CONTACTS_PHONE_MAX];
    char email[CONTACTS_EMAIL_MAX];
} contact_t;

static contact_t contacts[CONTACTS_MAX] = {
    {"Joshua", "(778) 201-4533", "trommatic@icloud.com"},
};
static int contacts_count = 1;
static int contacts_loaded = 0;

static void contacts_str_copy(char *dst, const char *src, int max) {
    int i = 0;
    while (src[i] && i < max - 1) { dst[i] = src[i]; i++; }
    dst[i] = 0;
}

/* Fields are '|'-delimited on one line: "name|phone|email\n".
   None of the contact text can contain '|' or '\n' (the add prompt only
   accepts printable ASCII 32-126, same guard reminders_add_new already
   applies), so a plain scan for the next '|' or '\n' is a real,
   unambiguous field boundary, no escaping needed. */
static void contacts_load(void) {
    if (contacts_loaded) return;
    contacts_loaded = 1;
    static char buf[4096];
    int n = vfs_read_file("CONTACTS.TXT", buf, sizeof(buf) - 1);
    if (n < 0) return; /* no file yet: keep the compiled-in starter contact */
    buf[n] = 0;
    contacts_count = 0;
    int i = 0;
    while (i < n && contacts_count < CONTACTS_MAX) {
        contact_t *c = &contacts[contacts_count];
        int j;
        j = 0; while (i < n && buf[i] != '|' && buf[i] != '\n' && j < CONTACTS_NAME_MAX - 1) c->name[j++] = buf[i++];
        c->name[j] = 0;
        while (i < n && buf[i] != '|' && buf[i] != '\n') i++; /* truncated field: eat the rest */
        if (i < n && buf[i] == '|') i++;
        j = 0; while (i < n && buf[i] != '|' && buf[i] != '\n' && j < CONTACTS_PHONE_MAX - 1) c->phone[j++] = buf[i++];
        c->phone[j] = 0;
        while (i < n && buf[i] != '|' && buf[i] != '\n') i++;
        if (i < n && buf[i] == '|') i++;
        j = 0; while (i < n && buf[i] != '\n' && j < CONTACTS_EMAIL_MAX - 1) c->email[j++] = buf[i++];
        c->email[j] = 0;
        contacts_count++;
        if (i < n && buf[i] == '\n') i++;
    }
}

static void contacts_save(void) {
    static char buf[4096];
    int n = 0;
    for (int idx = 0; idx < contacts_count; idx++) {
        contact_t *c = &contacts[idx];
        const char *s = c->name;    while (*s && n < (int)sizeof(buf) - 4) buf[n++] = *s++;
        buf[n++] = '|';
        s = c->phone;               while (*s && n < (int)sizeof(buf) - 4) buf[n++] = *s++;
        buf[n++] = '|';
        s = c->email;               while (*s && n < (int)sizeof(buf) - 2) buf[n++] = *s++;
        buf[n++] = '\n';
    }
    vfs_replace_file("CONTACTS.TXT", buf, (unsigned int)n);
}

/* Same lightweight get_key() text-capture loop reminders_add_new already
   established (renders live, backspace, enter confirms, esc cancels).
   v0.76.23: split chrome (titlebar + prompt label) from content (text box +
   typed text) to avoid redrawn-screen flashes on every keystroke, following
   the same fix pattern editor.h adopted at v0.76.10. The prompt and titlebar
   never change inside this loop, only the typed text does, so redrawing them
   every keystroke was unnecessary visual waste on a framebuffer with no double
   buffer. */
static int contacts_prompt_line(const char *prompt, char *out, int max) {
    unsigned int n = 0;
    out[0] = 0;
    mouse_click_edge_sync();

    /* Draw chrome only once, before the loop. */
    window_clear(GUI_BG);
    gui_draw_app_titlebar("Contacts");
    font_draw_string(prompt, 20, 52, 0x0075726E, -1);

    for (;;) {
        /* Redraw only the content area (text box and typed text), not the chrome. */
        window_rect(20, 76, (int)window_width() - 40, 20, 0x00FFFFFF);
        out[n] = 0;
        font_draw_string(out, 24, 78, 0x001C1C1E, -1);
        serial_puts("contactsprompt\n"); /* discriminating marker for regression test */
        int k = get_key_or_click();
        if (k == KEY_ESC || k == KEY_CLICK) return 0;
        if (k == KEY_ENTER) break;
        if (k == '\b') { if (n > 0) n--; }
        else if ((int)n < max - 1 && k >= 32 && k < 127) out[n++] = (char)k;
    }
    out[n] = 0;
    return 1;
}

static void contacts_add(void) {
    if (contacts_count >= CONTACTS_MAX) return;
    char name[CONTACTS_NAME_MAX], phone[CONTACTS_PHONE_MAX], email[CONTACTS_EMAIL_MAX];
    if (!contacts_prompt_line("name (enter to confirm, esc to cancel):", name, CONTACTS_NAME_MAX)) return;
    if (name[0] == 0) return;
    if (!contacts_prompt_line("phone:", phone, CONTACTS_PHONE_MAX)) return;
    if (!contacts_prompt_line("email:", email, CONTACTS_EMAIL_MAX)) return;
    contact_t *c = &contacts[contacts_count];
    contacts_str_copy(c->name, name, CONTACTS_NAME_MAX);
    contacts_str_copy(c->phone, phone, CONTACTS_PHONE_MAX);
    contacts_str_copy(c->email, email, CONTACTS_EMAIL_MAX);
    contacts_count++;
    contacts_save();
}

static void contacts_view(int idx) {
    contact_t *c = &contacts[idx];
    for (;;) {
        window_clear(GUI_BG);
        gui_draw_app_titlebar("Contacts");
        font_draw_string(c->name, 20, 48, 0x001C1C1E, -1);
        font_draw_string(c->phone, 20, 68, 0x00807468, -1);
        font_draw_string(c->email, 20, 88, 0x00807468, -1);
        gui_wait_close();
        return;
    }
}

static void contacts_delete_at(int sel) {
    if (sel < 0 || sel >= contacts_count) return;
    for (int j = sel; j < contacts_count - 1; j++) {
        contact_t *dst = &contacts[j], *src = &contacts[j + 1];
        for (int c = 0; c < CONTACTS_NAME_MAX; c++) dst->name[c] = src->name[c];
        for (int c = 0; c < CONTACTS_PHONE_MAX; c++) dst->phone[c] = src->phone[c];
        for (int c = 0; c < CONTACTS_EMAIL_MAX; c++) dst->email[c] = src->email[c];
    }
    contacts_count--;
    contacts_save();
}

static void gui_launch_contacts(void) {
    contacts_load();
    int sel = 0;
    for (;;) {
        window_clear(GUI_BG);
        gui_draw_app_titlebar("Contacts");
        if (!contacts_count) {
            font_draw_string("No contacts yet.", 20, 70, 0x001C1C1E, -1);
            font_draw_string("Press a to add one.", 20, 94, 0x00807468, -1);
        } else {
            font_draw_string("up/down to pick   enter views   a adds   d deletes   esc closes", 20, 52, 0x00807468, -1);
            for (int i = 0; i < contacts_count; i++) {
                int y = 84 + i * 22;
                if (i == sel) window_rect(16, y - 4, (int)window_width() - 32, 20, 0x00EDE6DC);
                font_draw_string(contacts[i].name, 28, y, 0x001C1C1E, -1);
                font_draw_string(contacts[i].phone, 220, y, 0x00807468, -1);
            }
        }
        sleep_ticks(5);
        mouse_click_edge_sync();
        int k = get_key_or_click();
        if (k == KEY_ESC || k == KEY_CLICK) return;
        if (k == 'a') { contacts_add(); continue; }
        if (!contacts_count) continue;
        if (k == KEY_UP && sel > 0) sel--;
        else if (k == KEY_DOWN && sel < contacts_count - 1) sel++;
        else if (k == KEY_ENTER) contacts_view(sel);
        else if (k == 'd') {
            contacts_delete_at(sel);
            if (sel >= contacts_count && sel > 0) sel--;
        }
    }
}
