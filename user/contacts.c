/* contacts: a plain list of people kept in CONTACTS.TXT, as a real ring-3 program.
 *
 * The thirteenth app to leave the kernel (roadmap 2.0), done the way
 * user/clock.c and user/activity.c were. Same job kernel/contacts.h did in
 * ring 0: a fixed list of 32 people with a name, phone and email, the
 * file rewritten whole after every add or delete, no directory sync. Built
 * with no kernel include path, linked flat, loaded off the VFS by exec_user,
 * and it reaches the machine only through int 0x80: SYS_WINDOW_OPEN for a
 * framebuffer, SYS_WINDOW_POLL for input and the present, and the ordinary
 * file calls (open, read, write, close) for CONTACTS.TXT. No new syscall.
 *
 * File format, unchanged: one "name|phone|email" line per person. Typing
 * refuses '|' so a field boundary is never ambiguous. An empty list is saved
 * as one blank line, because open() cannot tell an empty file from a missing
 * one and a missing file brings the starter contact back.
 *
 * A flat binary has no .bss and the RAM file system caps a file at 8KB, so
 * the 3KB list does not live in .data. exec_user zeroes the whole 128KB image
 * window before loading, so the list sits in the zeroed pages just past
 * _user_end, which the link script defines.
 *
 * Glyphs: antialiased DejaVu via libjt/text.h, same as the other ring-3
 * apps. Enter views a person, a adds, d deletes, Esc closes.
 * tools/checks/ring3contacts-check.py drives all of it.
 */
#include "jtsys.h"
#include "libjt/text.h"

#define BG    0x00FAF8F6 /* GUI_BG */
#define INK   0x001C1C1E
#define HINT  0x0075726E
#define DIM   0x00807468
#define SEL   0x00EDE6DC
#define WHITE 0x00FFFFFF

#define MAXC    32
#define NAME_N  32
#define PHONE_N 24
#define EMAIL_N 40
#define ROW_Y   72
#define ROW_H   24

struct contact { char name[NAME_N]; char phone[PHONE_N]; char email[EMAIL_N]; };
extern char _user_end[];

static struct jt_window_info win JT_DATA = {0, 0, 0, 0};
static struct contact *list JT_DATA = 0;
static int count JT_DATA = 0;
static int sel JT_DATA = 0;
static int top JT_DATA = 0;
static int mode JT_DATA = 0;       /* 0 list, 1 viewing one person, 2 adding */
static int field JT_DATA = 0;      /* while adding: 0 name, 1 phone, 2 email */
static char entry[3][EMAIL_N] JT_DATA = {{0}};
static int entry_len JT_DATA = 0;

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
static int text(const char *s, int x, int y, int sc, unsigned fg) { /* returns the x after the last glyph; sc 2 = bold */
    return jt_text_draw(&win, sc > 1 ? JT_FACE_BOLD : JT_FACE_BODY, x, y, fg, s);
}
/* text clipped to maxw px with a trailing "..." */
static int etext(const char *s, int x, int y, int maxw, unsigned fg) {
    char t[72]; int n = 0;
    while (s[n] && n < 64) { t[n] = s[n]; n++; }
    t[n] = 0;
    if (jt_text_width(JT_FACE_BODY, t) > maxw) {
        while (n > 0) {
            t[n] = '.'; t[n + 1] = '.'; t[n + 2] = '.'; t[n + 3] = 0;
            if (jt_text_width(JT_FACE_BODY, t) <= maxw) break;
            t[--n] = 0;
        }
        if (n == 0) t[0] = 0;
    }
    return jt_text_draw(&win, JT_FACE_BODY, x, y, fg, t);
}
static int utoa10(unsigned v, char *buf) {
    char tmp[12]; int tn = 0, n = 0;
    do { tmp[tn++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (tn) buf[n++] = tmp[--tn];
    buf[n] = 0;
    return n;
}
static void say(const char *pfx, unsigned v) { /* one line, one write: what the check reads */
    char line[48]; int l = 0;
    while (*pfx) line[l++] = *pfx++;
    l += utoa10(v, line + l); line[l++] = '\n';
    jt_write(1, line, (unsigned)l);
}
static void copy(char *dst, const char *src, int max) {
    int i = 0;
    while (src[i] && i < max - 1) { dst[i] = src[i]; i++; }
    dst[i] = 0;
}

static void starter(void) {
    copy(list[0].name, "Joshua", NAME_N);
    copy(list[0].phone, "(778) 201-4533", PHONE_N);
    copy(list[0].email, "trommatic@icloud.com", EMAIL_N);
    count = 1;
}

/* Read CONTACTS.TXT a chunk at a time and split it on '|' and newline. A
   field longer than its slot keeps its head and drops the rest, as the old
   parser did. A missing file keeps the starter contact. */
static void load(void) {
    int fd = jt_open("CONTACTS.TXT", JT_O_RDONLY);
    if (fd < 0) { starter(); return; }
    char chunk[128];
    int f = 0, p = 0, n;
    count = 0;
    struct contact *c = &list[0];
    static const int cap[3] = {NAME_N, PHONE_N, EMAIL_N};
    c->name[0] = c->phone[0] = c->email[0] = 0;
    while ((n = jt_read(fd, chunk, sizeof chunk)) > 0) {
        for (int i = 0; i < n; i++) {
            char ch = chunk[i];
            if (ch == '\r') continue;
            if (ch == '\n') {
                if (c->name[0] && count < MAXC) count++;
                if (count < MAXC) { c = &list[count]; c->name[0] = c->phone[0] = c->email[0] = 0; }
                f = 0; p = 0;
                continue;
            }
            if (count >= MAXC) continue;
            if (ch == '|' && f < 2) { f++; p = 0; continue; }
            char *dst = f == 0 ? c->name : f == 1 ? c->phone : c->email;
            if (p < cap[f] - 1) { dst[p++] = ch; dst[p] = 0; }
        }
    }
    if (count < MAXC && c->name[0]) count++; /* last line had no newline */
    jt_close(fd); /* the file exists, so an empty list stays empty */
}

static int save(void) {
    int fd = jt_open("CONTACTS.TXT", JT_O_WRONLY | JT_O_CREAT | JT_O_TRUNC);
    if (fd < 0) { jt_write(1, "contacts: save failed\n", 22); return 0; }
    char line[NAME_N + PHONE_N + EMAIL_N + 4];
    if (count == 0) jt_write(fd, "\n", 1);
    for (int i = 0; i < count; i++) {
        int l = 0;
        for (const char *s = list[i].name;  *s; s++) line[l++] = *s;
        line[l++] = '|';
        for (const char *s = list[i].phone; *s; s++) line[l++] = *s;
        line[l++] = '|';
        for (const char *s = list[i].email; *s; s++) line[l++] = *s;
        line[l++] = '\n';
        jt_write(fd, line, (unsigned)l);
    }
    if (jt_close(fd) < 0) { jt_write(1, "contacts: save failed\n", 22); return 0; }
    say("contacts: saved ", (unsigned)count);
    return 1;
}

static int visible_rows(void) {
    int v = ((int)win.height - ROW_Y) / ROW_H;
    return v < 1 ? 1 : v;
}

static void draw(void) {
    rect(0, 0, (int)win.width, (int)win.height, BG);
    if (mode == 2) {
        static const char *label[3] = {"name (enter to confirm, esc to cancel):", "phone:", "email:"};
        text(label[field], 20, 40, 1, HINT);
        rect(20, 66, (int)win.width - 40, 26, WHITE);
        if (entry[field][0]) {
            int cx = text(entry[field], 28, 70, 1, INK);
            rect(cx + 1, 71, 2, 16, INK); /* caret */
        } else {
            rect(28, 71, 2, 16, INK);
            text("type here", 34, 70, 1, DIM);
        }
        jt_write(1, "contactsprompt\n", 15); /* one per redraw: what the keystroke check counts */
        return;
    }
    if (mode == 1) {
        etext(list[sel].name, 20, 40, (int)win.width - 40, INK);
        etext(list[sel].phone, 20, 76, (int)win.width - 40, DIM);
        etext(list[sel].email, 20, 100, (int)win.width - 40, DIM);
        text("esc goes back", 20, 140, 1, HINT);
        return;
    }
    if (count == 0) {
        text("No contacts yet.", 20, 40, 1, INK);
        text("Press a to add one.", 20, 64, 1, DIM);
        return;
    }
    etext("up/down to pick   enter views   a adds   d deletes   esc closes", 20, 40, (int)win.width - 40, DIM);
    int v = visible_rows();
    for (int i = top; i < count && i < top + v; i++) {
        int y = ROW_Y + (i - top) * ROW_H;
        if (i == sel) rect(16, y - 4, (int)win.width - 32, 20, SEL);
        etext(list[i].name, 28, y, 180, INK);
        etext(list[i].phone, 220, y, (int)win.width - 240, DIM);
    }
}

static void keep_sel_visible(void) {
    int v = visible_rows();
    if (sel < top) top = sel;
    if (sel >= top + v) top = sel - v + 1;
    if (top < 0) top = 0;
}

static void begin_add(void) {
    if (count >= MAXC) return;
    mode = 2; field = 0; entry_len = 0;
    entry[0][0] = entry[1][0] = entry[2][0] = 0;
}

static void finish_field(void) {
    if (field == 0 && entry[0][0] == 0) { mode = 0; return; }
    if (field < 2) { field++; entry_len = 0; return; }
    struct contact *c = &list[count];
    copy(c->name, entry[0], NAME_N);
    copy(c->phone, entry[1], PHONE_N);
    copy(c->email, entry[2], EMAIL_N);
    count++;
    mode = 0;
    save();
    say("contacts: count ", (unsigned)count);
}

static void delete_sel(void) {
    if (sel < 0 || sel >= count) return;
    for (int j = sel; j < count - 1; j++) list[j] = list[j + 1];
    count--;
    if (sel >= count && sel > 0) sel--;
    keep_sel_visible();
    save();
    say("contacts: count ", (unsigned)count);
}

__attribute__((section(".text.start"), used))
void _start(int argc, char **argv) {
    (void)argc; (void)argv;
    if (jt_window_open(&win) != 0 || !win.pixels) {
        jt_write(2, "contacts: no window\n", 20);
        jt_exit(1);
    }
    {   /* one line, one write: what tools/checks/ring3contacts-check.py asserts on */
        char line[48]; int l = 0;
        const char *pfx = "contacts: ring-3 window ";
        while (*pfx) line[l++] = *pfx++;
        l += utoa10(win.width, line + l); line[l++] = 'x';
        l += utoa10(win.height, line + l); line[l++] = '\n';
        jt_write(1, line, (unsigned)l);
    }

    list = (struct contact *)(((unsigned)_user_end + 15u) & ~15u);
    load();
    say("contacts: loaded ", (unsigned)count);
    draw();

    unsigned flags = JT_POLL_PRESENT;
    for (;;) {
        struct jt_event ev;
        int r = jt_window_poll(&ev, flags);
        if (r == 1 && jt_window_resized(&ev, &win)) { draw(); flags = JT_POLL_PRESENT; continue; } /* JT_EV_RESIZE: remapped, repaint at the new size */
        flags = 0;
        if (r == -11 /* -EAGAIN */) { jt_sched_yield(); continue; }
        if (r != 1) break;

        if (ev.kind == JT_EV_CLICK) {
            if (ev.a < 0 || ev.b < 0 || ev.a >= (int)win.width || ev.b >= (int)win.height) break; /* chrome X or dock */
            if (mode == 0) { flags = JT_POLL_PRESENT; continue; } /* click inside, no action */
            mode = 0; draw(); flags = JT_POLL_PRESENT; continue;
        }
        if (ev.kind != JT_EV_KEY) { flags = JT_POLL_PRESENT; continue; }
        int k = ev.a;
        if (mode == 2) {
            if (k == JT_KEY_ESC) mode = 0;
            else if (k == JT_KEY_ENTER) finish_field();
            else if (k == 8 && entry_len > 0) entry[field][--entry_len] = 0;
            else {
                int max = field == 0 ? NAME_N : field == 1 ? PHONE_N : EMAIL_N;
                if (entry_len < max - 1 && k >= 32 && k < 127 && k != '|') {
                    entry[field][entry_len++] = (char)k; entry[field][entry_len] = 0;
                }
            }
        } else if (mode == 1) {
            if (k == JT_KEY_ESC || k == JT_KEY_ENTER) mode = 0;
        } else if (k == JT_KEY_ESC) break;
        else if (k == '`') { jt_write(1, "contacts: crashing on purpose\n", 30); *(volatile int *)0 = 1; } /* deliberate crash, as in every ring-3 app */
        else if (k == 'a') begin_add();
        else if (count > 0) {
            if (k == JT_KEY_UP && sel > 0) { sel--; keep_sel_visible(); say("contacts: sel ", (unsigned)sel); }
            else if (k == JT_KEY_DOWN && sel < count - 1) { sel++; keep_sel_visible(); say("contacts: sel ", (unsigned)sel); }
            else if (k == JT_KEY_ENTER) mode = 1;
            else if (k == 'd') delete_sel();
        }
        draw(); flags = JT_POLL_PRESENT;
    }
    jt_write(1, "contacts: closed\n", 17);
    jt_exit(0);
}
