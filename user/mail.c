/* mail: the Mail app as a real ring-3 program.
 *
 * Same look and behavior as kernel/mail.h: an inbox list (unread rows dark
 * with a dot, read rows faded), Enter or a second click opens a message with
 * its sender, subject, a rule and the wrapped body, d deletes, c composes,
 * Esc goes back (and closes from the list). The mail itself is MAIL.TXT, the
 * kernel's own file and format, one "r|from|subject|body" line per message,
 * read and rewritten through the ordinary file syscalls. When MAIL.TXT does
 * not exist the same two starter messages appear, as in the kernel.
 *
 * Compose is an inline sheet over the same window (no second window). From,
 * Subject, Body; Enter moves on, Enter on Body or the Send button files the
 * message, Esc or Cancel drops it. Like the kernel app there is no SMTP: Send
 * writes the message into the local inbox, and the sheet says so. Type is the
 * antialiased libjt face. The backquote key is the deliberate crash (outside
 * the compose sheet, where it types). Serial markers: mail: n=COUNT,
 * mail: read=I, mail: compose=1 (the inline sheet opened), mail: deleted n=COUNT, mail: filed n=COUNT.
 */
#include "jtsys.h"
#include "libjt/text.h"

#define BG    0x00FAF8F6
#define INK   0x001C1C1E
#define DIM   0x00807468
#define FADE  0x00A39C92
#define SEL   0x00EDE6DC
#define RULE  0x00E0D8CE
#define WHITE 0x00FFFFFF
#define BTN   0x00EFEBE4
#define BTN_ON 0x00E5DCCC
#define SHEET 0x00F3EEE5

#define MAX 24
#define FROM_MAX 32
#define SUBJ_MAX 48
#define BODY_MAX 240
#define ROW_Y0 52
#define ROW_H 22

/* Messages are views into the raw file text: '|' and newline are turned into NULs in place. */
struct msg { char *from; char *subject; char *body; int read; };

static struct jt_window_info win JT_DATA = {0, 0, 0, 0};
extern char _user_end[];
/* Big buffers live past _user_end (a flat image has no .bss), like user/contacts.c. */
struct arena { struct msg msgs[MAX]; char filebuf[4096]; char cfb[FROM_MAX + SUBJ_MAX + BODY_MAX]; int used; };
static struct arena *ar JT_DATA = 0;
#define msgs (ar->msgs)
#define filebuf (ar->filebuf)
static int count JT_DATA = 0;
static int sel JT_DATA = 0;
static int mode JT_DATA = 0;      /* 0 list, 1 read, 2 compose sheet */
static int field JT_DATA = 0;     /* compose: 0 from, 1 subject, 2 body */
static int cl[3] JT_DATA = {0, 0, 0};
static char *cfp(int f) { return ar->cfb + (f == 0 ? 0 : f == 1 ? FROM_MAX : FROM_MAX + SUBJ_MAX); }
#define cf(f) cfp(f)
static const char *note JT_DATA = 0;

static void rect(int x, int y, int w, int h, unsigned c) {
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > (int)win.width)  w = (int)win.width - x;
    if (y + h > (int)win.height) h = (int)win.height - y;
    for (int yy = 0; yy < h; yy++) {
        unsigned *row = win.pixels + (unsigned)(y + yy) * win.width + (unsigned)x;
        for (int xx = 0; xx < w; xx++) row[xx] = c;
    }
}
static void text(const char *s, int x, int y, unsigned fg) { jt_text_draw(&win, JT_FACE_BODY, x, y, fg, s); }
static void say(const char *s, int n) {
    char b[48]; int l = 0;
    while (*s) b[l++] = *s++;
    if (n >= 0) {
        char d[8]; int nd = 0, v = n;
        if (!v) d[nd++] = '0';
        while (v) { d[nd++] = (char)('0' + v % 10); v /= 10; }
        while (nd) b[l++] = d[--nd];
    }
    b[l++] = '\n';
    jt_write(1, b, (unsigned)l);
}

/* Append a string to the arena text; returns its address, or 0 when full. */
static char *stash(const char *src) {
    int n = 0;
    while (src[n]) n++;
    if (ar->used + n + 1 > 4096) return 0;
    char *d = filebuf + ar->used;
    for (int i = 0; i <= n; i++) d[i] = src[i];
    ar->used += n + 1;
    return d;
}
static void add_msg(const char *f, const char *s, const char *b, int read) {
    struct msg *m = &msgs[count];
    m->from = stash(f); m->subject = stash(s); m->body = stash(b); m->read = read;
    count++;
}

static void seed(void) {
    add_msg("Joshua Tree", "Welcome to Mail",
        "This is a real local inbox, no network behind it. Press enter to read a message, c to compose one, d to delete, esc to close.", 0);
    add_msg("Joshua Tree", "About this app",
        "Same shape as Notes and Reminders: everything here is written through to MAIL.TXT on the real FAT disk immediately, no Save button, no draft you can lose.", 0);
}

/* Same format as mail_load in kernel/mail.h: "r|from|subject|body\n". Parsed in
   place, the separators becoming NULs. */
static void load(void) {
    int fd = jt_open("MAIL.TXT", JT_O_RDONLY);
    if (fd < 0) { seed(); return; }
    int n = jt_read(fd, filebuf, 4096 - 1);
    jt_close(fd);
    if (n < 0) { seed(); return; }
    filebuf[n] = 0;
    char *b = filebuf;
    int i = 0;
    count = 0;
    while (i < n && count < MAX) {
        struct msg *m = &msgs[count];
        m->read = (b[i] == 'r');
        i += 2;
        m->from = b + i;
        while (i < n && b[i] != '|' && b[i] != '\n') i++;
        if (i < n && b[i] == '|') b[i++] = 0;
        m->subject = b + i;
        while (i < n && b[i] != '|' && b[i] != '\n') i++;
        if (i < n && b[i] == '|') b[i++] = 0;
        m->body = b + i;
        while (i < n && b[i] != '\n') i++;
        b[i] = 0;
        count++;
        i++;
    }
    ar->used = n + 1;
}

static void put(int fd, const char *s) {
    int n = 0;
    while (s[n]) n++;
    jt_write(fd, s, (unsigned)n);
}
static void save(void) {
    int fd = jt_open("MAIL.TXT", JT_O_WRONLY | JT_O_CREAT | JT_O_TRUNC);
    if (fd < 0) { note = "could not save MAIL.TXT"; return; }
    for (int k = 0; k < count; k++) {
        struct msg *m = &msgs[k];
        put(fd, m->read ? "r|" : "u|");
        put(fd, m->from); put(fd, "|");
        put(fd, m->subject); put(fd, "|");
        put(fd, m->body); put(fd, "\n");
    }
    jt_close(fd);
}

static void del_at(int i) {
    if (i < 0 || i >= count) return;
    for (int j = i; j < count - 1; j++) msgs[j] = msgs[j + 1];
    count--;
    save();
    say("mail: deleted n=", count);
}

/* Word-wrap body into the box; returns the y below the last line. */
static int wrapped(const char *s, int x, int y, int w, int ymax, unsigned fg) {
    char line[BODY_MAX];
    int ll = 0;
    while (*s && y + 18 <= ymax) {
        int we = 0;
        while (s[we] && s[we] != ' ') we++;
        char trial[BODY_MAX];
        int tl = 0;
        for (int i = 0; i < ll; i++) trial[tl++] = line[i];
        if (ll) trial[tl++] = ' ';
        for (int i = 0; i < we && tl < BODY_MAX - 1; i++) trial[tl++] = s[i];
        trial[tl] = 0;
        if (ll && jt_text_width(JT_FACE_BODY, trial) > w) {
            line[ll] = 0;
            text(line, x, y, fg);
            y += 18; ll = 0;
            continue;
        }
        for (int i = 0; i <= tl; i++) line[i] = trial[i];
        ll = tl;
        s += we;
        while (*s == ' ') s++;
    }
    if (ll && y + 18 <= ymax) { line[ll] = 0; text(line, x, y, fg); y += 18; }
    return y;
}

/* Fit text to maxw by cutting characters. */
static void fit(char *out, const char *s, int maxw) {
    int n = 0;
    while (s[n] && n < BODY_MAX - 1) { out[n] = s[n]; n++; }
    out[n] = 0;
    while (n > 1 && jt_text_width(JT_FACE_BODY, out) > maxw) out[--n] = 0;
}

#define SH_X 24
#define SH_Y 40
static int sh_w(void) { return (int)win.width - 48; }
static int fld_y(int f) { return SH_Y + 44 + f * 54; }
static int btn_y(void) { return fld_y(2) + 90; }

static void draw_sheet(void) {
    char t[BODY_MAX];
    int w = sh_w();
    rect(SH_X - 6, SH_Y - 6, w + 12, btn_y() + 52 - SH_Y, SHEET);
    jt_text_draw(&win, JT_FACE_BOLD, SH_X + 6, SH_Y + 4, INK, "New Message");
    const char *lab[3] = {"From", "Subject", "Body"};
    for (int f = 0; f < 3; f++) {
        int y = fld_y(f);
        text(lab[f], SH_X + 6, y - 18, DIM);
        int h = f == 2 ? 70 : 24;
        rect(SH_X + 6, y, w - 12, h, f == field ? WHITE : BTN);
        if (f == field) { rect(SH_X + 6, y, w - 12, 1, DIM); rect(SH_X + 6, y + h - 1, w - 12, 1, DIM); }
        cf(f)[cl[f]] = 0;
        if (f == 2) wrapped(cf(f), SH_X + 12, y + 4, w - 24, y + h, INK);
        else { fit(t, cf(f), w - 28); text(t, SH_X + 12, y + 3, INK); }
    }
    int by = btn_y();
    rect(SH_X + 6, by, 80, 24, BTN);        text("Cancel", SH_X + 18, by + 3, INK);
    rect(SH_X + 94, by, 80, 24, BTN_ON);    jt_text_draw(&win, JT_FACE_BOLD, SH_X + 112, by + 3, INK, "Send");
    text("Local inbox only: nothing leaves this machine, no SMTP.", SH_X + 186, by + 3, DIM);
}

static void draw(void) {
    char t[BODY_MAX];
    rect(0, 0, (int)win.width, (int)win.height, BG);
    int W = (int)win.width;
    if (mode == 1 && sel < count) {
        struct msg *m = &msgs[sel];
        text(m->from, 20, 40, DIM);
        text(m->subject, 20, 62, INK);
        rect(20, 86, W - 40, 1, RULE);
        wrapped(m->body, 20, 98, W - 40, (int)win.height - 40, INK);
        text("esc goes back   d deletes", 20, (int)win.height - 28, DIM);
        return;
    }
    if (!count) {
        text("No mail yet.", 20, 44, INK);
        text("Press c to compose one.", 20, 68, DIM);
    } else {
        text("up/down to pick   enter reads   c composes   d deletes   esc closes", 20, 40 - 4, DIM);
        for (int i = 0; i < count; i++) {
            int y = ROW_Y0 + 16 + i * ROW_H;
            if (y + ROW_H > (int)win.height) break;
            if (i == sel) rect(16, y - 4, W - 32, 20, SEL);
            unsigned fg = msgs[i].read ? FADE : INK;
            if (!msgs[i].read) rect(24, y + 5, 6, 6, INK);
            fit(t, msgs[i].from, 150);
            text(t, 40, y - 1, fg);
            fit(t, msgs[i].subject, W - 220 - 20);
            text(t, 200, y - 1, fg);
        }
    }
    if (note) text(note, 20, (int)win.height - 28, DIM);
    if (mode == 2) draw_sheet();
}

static void open_msg(int i) {
    if (i < 0 || i >= count) return;
    sel = i;
    if (!msgs[i].read) { msgs[i].read = 1; save(); }
    mode = 1;
    say("mail: read=", i);
}
static void compose_begin(void) {
    if (count >= MAX) { note = "Inbox is full (24). Delete something first."; return; }
    mode = 2; field = 0; note = 0;
    say("mail: compose=", 1);
    for (int f = 0; f < 3; f++) { cl[f] = 0; cf(f)[0] = 0; }
}
static void compose_send(void) {
    if (!cf(0)[0]) { mode = 0; return; }
    if (count < MAX && ar->used + cl[0] + cl[1] + cl[2] + 3 <= 4096) {
        add_msg(cf(0), cf(1), cf(2), 0);
        save();
        say("mail: filed n=", count);
        note = "Filed in the local inbox. Not sent anywhere: there is no mail server.";
    } else note = "No room left in the inbox. Not filed, not sent.";
    mode = 0;
}
static int field_max(int f) { return f == 0 ? FROM_MAX : f == 1 ? SUBJ_MAX : BODY_MAX; }

static void sheet_click(int x, int y) {
    int by = btn_y();
    if (y >= by && y < by + 24) {
        if (x >= SH_X + 6 && x < SH_X + 86) mode = 0;
        else if (x >= SH_X + 94 && x < SH_X + 174) compose_send();
        return;
    }
    for (int f = 0; f < 3; f++) {
        int h = f == 2 ? 70 : 24;
        if (y >= fld_y(f) && y < fld_y(f) + h) { field = f; return; }
    }
}

__attribute__((section(".text.start"), used))
void _start(int argc, char **argv) {
    (void)argc; (void)argv;
    if (jt_window_open(&win) != 0 || !win.pixels) { jt_write(2, "mail: no window\n", 16); jt_exit(1); }
    ar = (struct arena *)(((unsigned)_user_end + 15u) & ~15u);
    for (int f = 0; f < 3; f++) cf(f)[0] = 0;
    load();
    draw();
    jt_write(1, "mail: ring-3 window\n", 20);
    say("mail: n=", count);
    unsigned flags = JT_POLL_PRESENT;
    for (;;) {
        struct jt_event ev;
        int r = jt_window_poll(&ev, flags);
        flags = 0;
        if (r == -11) { jt_sched_yield(); continue; }
        if (r != 1) break;
        if (ev.kind == JT_EV_CLICK) {
            if (mode == 2) sheet_click(ev.a, ev.b);
            else if (mode == 0 && ev.b >= ROW_Y0 + 12) {
                int hit = (ev.b - (ROW_Y0 + 12)) / ROW_H;
                if (hit >= 0 && hit < count) { if (hit == sel) open_msg(hit); else sel = hit; }
            }
        } else if (ev.kind == JT_EV_KEY) {
            int k = ev.a;
            note = 0;
            if (mode == 2) {
                if (k == JT_KEY_ESC) mode = 0;
                else if (k == JT_KEY_ENTER) { if (field < 2) field++; else compose_send(); }
                else if (k == 8) { if (cl[field] > 0) cl[field]--; }
                else if (k == 9 || k == JT_KEY_DOWN) { field = (field + 1) % 3; }
                else if (k == JT_KEY_UP) { field = (field + 2) % 3; }
                else if (k >= 32 && k < 127 && cl[field] < field_max(field) - 1) cf(field)[cl[field]++] = (char)k;
            } else {
                if (k == '`') { jt_write(1, "mail: crashing on purpose\n", 26); *(volatile int *)0 = 1; }
                if (mode == 1) {
                    if (k == JT_KEY_ESC) mode = 0;
                    else if (k == 'd') { del_at(sel); if (sel >= count && sel > 0) sel--; mode = 0; }
                } else {
                    if (k == JT_KEY_ESC) break;
                    else if (k == 'c') compose_begin();
                    else if (count) {
                        if (k == JT_KEY_UP && sel > 0) sel--;
                        else if (k == JT_KEY_DOWN && sel < count - 1) sel++;
                        else if (k == JT_KEY_ENTER) open_msg(sel);
                        else if (k == 'd') { del_at(sel); if (sel >= count && sel > 0) sel--; }
                    }
                }
            }
        } else { flags = JT_POLL_PRESENT; continue; }
        draw();
        flags = JT_POLL_PRESENT;
    }
    jt_write(1, "mail: closed\n", 13);
    jt_exit(0);
}
