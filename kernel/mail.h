/* v58 (0.58.0): Mail. Same shape as reminders.h and calendar.h: a plain
   text file on the real FAT disk (MAIL.TXT), a fixed-size static array,
   write-through on every mutation, manual line parsing, no sscanf, no
   new persistence mechanism invented for this. This kernel has no
   SMTP/IMAP client and that's deliberately out of scope, same call
   Curbfind's "no real Craigslist POST" and every other locally-shaped
   app here already made: this is a local mail-shaped app, a real
   persisted inbox you can list, read, compose into and delete from, the
   same relationship Reminders has to a real to-do sync service or Notes
   to a real cloud note app.

   Two starter messages are compiled in so the inbox isn't a blank box
   the very first time it opens (same "not empty by default" call
   Reminders and Notes both made); they're only ever used before
   MAIL.TXT exists on disk, a real saved (even empty) file always wins
   over them from then on. */
#define MAIL_MAX 24
#define MAIL_FROM_MAX 32
#define MAIL_SUBJECT_MAX 48
#define MAIL_BODY_MAX 240

typedef struct {
    char from[MAIL_FROM_MAX];
    char subject[MAIL_SUBJECT_MAX];
    char body[MAIL_BODY_MAX];
    int read;
} mail_msg_t;

static mail_msg_t mail_msgs[MAIL_MAX] = {
    {"Joshua Tree", "Welcome to Mail",
     "This is a real local inbox, no network behind it. Press enter to "
     "read a message, c to compose one, d to delete, esc to close.", 0},
    {"Joshua Tree", "About this app",
     "Same shape as Notes and Reminders: everything here is written "
     "through to MAIL.TXT on the real FAT disk immediately, no Save "
     "button, no draft you can lose.", 0},
};
static int mail_count = 2;
static int mail_loaded = 0;

static void mail_str_copy(char *dst, const char *src, int max) {
    int i = 0;
    while (src[i] && i < max - 1) { dst[i] = src[i]; i++; }
    dst[i] = 0;
}

/* Fields are '|'-delimited on one line: "<r/u>|from|subject|body\n".
   None of the seed or composed text can contain '|' or '\n' (the
   compose prompt only accepts printable ASCII 32-126, same guard
   reminders_add_new already applies), so a plain scan for the next '|'
   or '\n' is a real, unambiguous field boundary, no escaping needed. */
static void mail_load(void) {
    if (mail_loaded) return;
    mail_loaded = 1;
    static char buf[4096];
    int n = vfs_read_file("MAIL.TXT", buf, sizeof(buf) - 1);
    if (n < 0) return; /* no file yet: keep the compiled-in starter messages */
    buf[n] = 0;
    mail_count = 0;
    int i = 0;
    while (i < n && mail_count < MAIL_MAX) {
        int is_read = (buf[i] == 'r');
        i += 2; /* flag + the '|' right after it */
        mail_msg_t *m = &mail_msgs[mail_count];
        int j;
        j = 0; while (i < n && buf[i] != '|' && buf[i] != '\n' && j < MAIL_FROM_MAX - 1) m->from[j++] = buf[i++];
        m->from[j] = 0;
        while (i < n && buf[i] != '|' && buf[i] != '\n') i++; /* truncated field: eat the rest */
        if (i < n && buf[i] == '|') i++;
        j = 0; while (i < n && buf[i] != '|' && buf[i] != '\n' && j < MAIL_SUBJECT_MAX - 1) m->subject[j++] = buf[i++];
        m->subject[j] = 0;
        while (i < n && buf[i] != '|' && buf[i] != '\n') i++;
        if (i < n && buf[i] == '|') i++;
        j = 0; while (i < n && buf[i] != '\n' && j < MAIL_BODY_MAX - 1) m->body[j++] = buf[i++];
        m->body[j] = 0;
        while (i < n && buf[i] != '\n') i++;
        m->read = is_read;
        mail_count++;
        if (i < n && buf[i] == '\n') i++;
    }
}

static void mail_save(void) {
    static char buf[4096];
    int n = 0;
    for (int idx = 0; idx < mail_count; idx++) {
        mail_msg_t *m = &mail_msgs[idx];
        buf[n++] = m->read ? 'r' : 'u';
        buf[n++] = '|';
        const char *s = m->from;    while (*s && n < (int)sizeof(buf) - 4) buf[n++] = *s++;
        buf[n++] = '|';
        s = m->subject;             while (*s && n < (int)sizeof(buf) - 4) buf[n++] = *s++;
        buf[n++] = '|';
        s = m->body;                while (*s && n < (int)sizeof(buf) - 2) buf[n++] = *s++;
        buf[n++] = '\n';
    }
    vfs_replace_file("MAIL.TXT", buf, (unsigned int)n);
}

/* v0.76.24: Uses shared gui_prompt_line_input from gui_prompt.h,
   which splits chrome (titlebar+prompt) drawn once before the loop
   from content (text box+text) redrawn per keystroke, fixing the
   per-keystroke window_clear bug. Same three-field composition flow. */
static int mail_prompt_line(const char *prompt, char *out, int max) {
    return gui_prompt_line_input("Mail", prompt, out, max);
}

static void mail_compose(void) {
    if (mail_count >= MAIL_MAX) return;
    char from[MAIL_FROM_MAX], subject[MAIL_SUBJECT_MAX], body[MAIL_BODY_MAX];
    if (!mail_prompt_line("from (enter to confirm, esc to cancel):", from, MAIL_FROM_MAX)) return;
    if (from[0] == 0) return;
    if (!mail_prompt_line("subject:", subject, MAIL_SUBJECT_MAX)) return;
    if (!mail_prompt_line("body:", body, MAIL_BODY_MAX)) return;
    mail_msg_t *m = &mail_msgs[mail_count];
    mail_str_copy(m->from, from, MAIL_FROM_MAX);
    mail_str_copy(m->subject, subject, MAIL_SUBJECT_MAX);
    mail_str_copy(m->body, body, MAIL_BODY_MAX);
    m->read = 0;
    mail_count++;
    mail_save();
}

/* A real message, read full-screen with the same render_wrapped_text
   gui_launch_html already uses for parsed page bodies, not squeezed
   onto one line like the list row is. Marks the message read and
   writes that through immediately, same "no separate Save step" rule
   every mutation in this file follows. */
static void mail_read_message(int idx) {
    mail_msg_t *m = &mail_msgs[idx];
    if (!m->read) { m->read = 1; mail_save(); }
    for (;;) {
        window_clear(GUI_BG);
        gui_draw_app_titlebar("Mail");
        font_draw_string(m->from, 20, 48, 0x00807468, -1);
        font_draw_string(m->subject, 20, 68, 0x001C1C1E, -1);
        window_rect(20, 92, (int)window_width() - 40, 1, 0x00E0D8CE);
        render_wrapped_text(m->body, 20, 106, (int)window_width() - 40, (int)window_height() - 150, 0x001C1C1E);
        gui_wait_close();
        return;
    }
}

/* Deletes message `sel`, shifting everything after it down one slot.
   Field-by-field, not a whole-struct assignment: this struct is large
   enough (~320 bytes) that clang can lower `a = b` to a real call to
   memcpy, a libc symbol this freestanding build doesn't link, the same
   reason reminders.h's own delete loop shifts its plain char array one
   field at a time instead of relying on a builtin. Pulled out on its own
   (v59 test gap fix) so mailtest can exercise the exact same shift path
   the UI's 'd' key runs, not a re-typed copy that could drift from it. */
static void mail_delete_at(int sel) {
    if (sel < 0 || sel >= mail_count) return;
    for (int j = sel; j < mail_count - 1; j++) {
        mail_msg_t *dst = &mail_msgs[j], *src = &mail_msgs[j + 1];
        for (int c = 0; c < MAIL_FROM_MAX; c++) dst->from[c] = src->from[c];
        for (int c = 0; c < MAIL_SUBJECT_MAX; c++) dst->subject[c] = src->subject[c];
        for (int c = 0; c < MAIL_BODY_MAX; c++) dst->body[c] = src->body[c];
        dst->read = src->read;
    }
    mail_count--;
    mail_save();
}

/* Same up/down/select list contract Reminders and Trash already use,
   plus 'c' to compose (Reminders' 'a' would collide with the from/
   subject/body text fields' own letters, so this file picks its own),
   enter to open a message full-screen, 'd' to delete. Unread messages
   read dark/bold like Reminders' unchecked rows; read ones fade to the
   same de-emphasized gray Reminders gives a checked-off item. */
static void gui_launch_mail(void) {
    mail_load();
    int sel = 0;
    for (;;) {
        window_clear(GUI_BG);
        gui_draw_app_titlebar("Mail");
        if (!mail_count) {
            font_draw_string("No mail yet.", 20, 70, 0x001C1C1E, -1);
            font_draw_string("Press c to compose one.", 20, 94, 0x00807468, -1);
        } else {
            font_draw_string("up/down to pick   enter reads   c composes   d deletes   esc closes", 20, 52, 0x00807468, -1);
            for (int i = 0; i < mail_count; i++) {
                int y = 84 + i * 22;
                if (i == sel) window_rect(16, y - 4, (int)window_width() - 32, 20, 0x00EDE6DC);
                unsigned int fg = mail_msgs[i].read ? 0x00A39C92 : 0x001C1C1E;
                font_draw_string(mail_msgs[i].read ? "   " : "  *", 28, y, fg, -1); /* unread dot, same idea as a real inbox's bold row */
                font_draw_string(mail_msgs[i].from, 60, y, fg, -1);
                font_draw_string(mail_msgs[i].subject, 220, y, fg, -1);
            }
        }
        sleep_ticks(5);
        mouse_click_edge_sync();
        int k = get_key_or_click();
        if (k == KEY_ESC || k == KEY_CLICK) return;
        if (k == 'c') { mail_compose(); continue; }
        if (!mail_count) continue;
        if (k == KEY_UP && sel > 0) sel--;
        else if (k == KEY_DOWN && sel < mail_count - 1) sel++;
        else if (k == KEY_ENTER) mail_read_message(sel);
        else if (k == 'd') {
            mail_delete_at(sel);
            if (sel >= mail_count && sel > 0) sel--;
        }
    }
}

/* v0.75.0 (multi-window batch 2): same real-state split reminders.h/
   calendar.h just established -- gui_launch_mail above is untouched,
   this is a second, separate state machine (list/read/three-stage
   compose) over the same shared data functions (mail_load/save/
   delete_at, mail_str_copy), so which message is selected, whether a
   message is being read, and an in-progress compose all persist across
   repaints and across losing focus to another open window. */
#define MAIL_MW_LIST            0
#define MAIL_MW_READ            1
#define MAIL_MW_COMPOSE_FROM    2
#define MAIL_MW_COMPOSE_SUBJECT 3
#define MAIL_MW_COMPOSE_BODY    4

static int mail_mw_sel = 0;
static int mail_mw_mode = MAIL_MW_LIST;
static char mail_mw_from[MAIL_FROM_MAX];
static char mail_mw_subject[MAIL_SUBJECT_MAX];
static char mail_mw_body[MAIL_BODY_MAX];
static unsigned int mail_mw_len = 0; /* length of whichever compose field is currently being typed */

/* Windowed apps have no titlebar strip to clear (the compositor draws
   real window chrome), so content starts right under it, same as
   Stocks' stx_top(). The old fixed offsets here assumed the full-screen
   title band; a windowed run left ~50px of dead space above content. */
static int mail_top(void) { return gui_app_windowed ? 8 : 40; }

static void gui_draw_mail_content(void){
    mail_load();
    window_clear(GUI_BG);
    gui_draw_app_titlebar("Mail");
    int T = mail_top();
    if (mail_mw_mode == MAIL_MW_READ) {
        mail_msg_t *m = &mail_msgs[mail_mw_sel];
        font_draw_string(m->from, 20, T + 8, 0x00807468, -1);
        font_draw_string(m->subject, 20, T + 28, 0x001C1C1E, -1);
        window_rect(20, T + 52, (int)window_width() - 40, 1, 0x00E0D8CE);
        render_wrapped_text(m->body, 20, T + 66, (int)window_width() - 40, (int)window_height() - T - 110, 0x001C1C1E);
        return;
    }
    if (mail_mw_mode == MAIL_MW_COMPOSE_FROM || mail_mw_mode == MAIL_MW_COMPOSE_SUBJECT || mail_mw_mode == MAIL_MW_COMPOSE_BODY) {
        const char *prompt = mail_mw_mode == MAIL_MW_COMPOSE_FROM ? "from (enter confirms, esc cancels):" :
                              mail_mw_mode == MAIL_MW_COMPOSE_SUBJECT ? "subject:" : "body:";
        char *buf = mail_mw_mode == MAIL_MW_COMPOSE_FROM ? mail_mw_from : mail_mw_mode == MAIL_MW_COMPOSE_SUBJECT ? mail_mw_subject : mail_mw_body;
        font_draw_string(prompt, 20, T + 12, 0x0075726E, -1);
        window_rect(20, T + 36, (int)window_width() - 40, 20, 0x00FFFFFF);
        buf[mail_mw_len] = 0;
        font_draw_string(buf, 24, T + 38, 0x001C1C1E, -1);
        return;
    }
    if (!mail_count) {
        font_draw_string("No mail yet.", 20, T + 30, 0x001C1C1E, -1);
        font_draw_string("Press c to compose one.", 20, T + 54, 0x00807468, -1);
        return;
    }
    font_draw_string("up/down to pick   enter reads   c composes   d deletes   esc closes", 20, T + 12, 0x00807468, -1);
    if (mail_mw_sel >= mail_count) mail_mw_sel = mail_count - 1;
    for (int i = 0; i < mail_count; i++) {
        int y = T + 44 + i * 22;
        if (i == mail_mw_sel) window_rect(16, y - 4, (int)window_width() - 32, 20, 0x00EDE6DC);
        unsigned int fg = mail_msgs[i].read ? 0x00A39C92 : 0x001C1C1E;
        font_draw_string(mail_msgs[i].read ? "   " : "  *", 28, y, fg, -1);
        font_draw_string(mail_msgs[i].from, 60, y, fg, -1);
        font_draw_string(mail_msgs[i].subject, 220, y, fg, -1);
    }
}

/* Returns 1 when this window should close (esc from the list view); esc
   from read/compose just cancels back to the list, matching
   mail_read_message/mail_prompt_line's own esc contracts, not a
   whole-window close. */
static int gui_mail_on_key(int k){
    if (mail_mw_mode == MAIL_MW_READ) {
        if (k == KEY_ESC) { mail_mw_mode = MAIL_MW_LIST; return 0; }
        return 0;
    }
    if (mail_mw_mode == MAIL_MW_COMPOSE_FROM || mail_mw_mode == MAIL_MW_COMPOSE_SUBJECT || mail_mw_mode == MAIL_MW_COMPOSE_BODY) {
        char *buf = mail_mw_mode == MAIL_MW_COMPOSE_FROM ? mail_mw_from : mail_mw_mode == MAIL_MW_COMPOSE_SUBJECT ? mail_mw_subject : mail_mw_body;
        int max = mail_mw_mode == MAIL_MW_COMPOSE_FROM ? MAIL_FROM_MAX : mail_mw_mode == MAIL_MW_COMPOSE_SUBJECT ? MAIL_SUBJECT_MAX : MAIL_BODY_MAX;
        if (k == KEY_ESC) { mail_mw_mode = MAIL_MW_LIST; return 0; }
        if (k == KEY_ENTER) {
            buf[mail_mw_len] = 0;
            if (mail_mw_mode == MAIL_MW_COMPOSE_FROM) {
                if (mail_mw_from[0] == 0) { mail_mw_mode = MAIL_MW_LIST; return 0; }
                mail_mw_mode = MAIL_MW_COMPOSE_SUBJECT; mail_mw_len = 0; mail_mw_subject[0] = 0;
            } else if (mail_mw_mode == MAIL_MW_COMPOSE_SUBJECT) {
                mail_mw_mode = MAIL_MW_COMPOSE_BODY; mail_mw_len = 0; mail_mw_body[0] = 0;
            } else {
                if (mail_count < MAIL_MAX) {
                    mail_msg_t *m = &mail_msgs[mail_count];
                    mail_str_copy(m->from, mail_mw_from, MAIL_FROM_MAX);
                    mail_str_copy(m->subject, mail_mw_subject, MAIL_SUBJECT_MAX);
                    mail_str_copy(m->body, mail_mw_body, MAIL_BODY_MAX);
                    m->read = 0;
                    mail_count++;
                    mail_save();
                }
                mail_mw_mode = MAIL_MW_LIST;
            }
            return 0;
        }
        if (k == '\b') { if (mail_mw_len > 0) mail_mw_len--; return 0; }
        if (k >= 32 && k < 127 && (int)mail_mw_len < max - 1) buf[mail_mw_len++] = (char)k;
        return 0;
    }
    /* list mode */
    if (k == KEY_ESC) return 1;
    if (k == 'c') { mail_mw_mode = MAIL_MW_COMPOSE_FROM; mail_mw_len = 0; mail_mw_from[0] = 0; return 0; }
    if (!mail_count) return 0;
    if (k == KEY_UP && mail_mw_sel > 0) mail_mw_sel--;
    else if (k == KEY_DOWN && mail_mw_sel < mail_count - 1) mail_mw_sel++;
    else if (k == KEY_ENTER) {
        if (!mail_msgs[mail_mw_sel].read) { mail_msgs[mail_mw_sel].read = 1; mail_save(); }
        mail_mw_mode = MAIL_MW_READ;
    } else if (k == 'd') {
        mail_delete_at(mail_mw_sel);
        if (mail_mw_sel >= mail_count && mail_mw_sel > 0) mail_mw_sel--;
    }
    return 0;
}
