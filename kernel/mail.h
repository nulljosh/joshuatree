/* Mail's data layer. The app itself is ring 3 (user/mail.c, a compositor
   window); this file keeps only what Samantha's read_mail/send_mail tools
   (chat.h) and mailtest still need: the message array and MAIL.TXT
   load/save, one line per message, "<r/u>|from|subject|body". Starter
   messages are only used before MAIL.TXT exists on disk. */
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
