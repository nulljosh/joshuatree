/* reminders: a persisted checklist kept in REMINDERS.TXT, as a real ring-3 program.
 *
 * The fifteenth app to leave the kernel (roadmap 2.0), done the way
 * user/contacts.c was. Same job kernel/reminders.h did in ring 0: up to 24
 * items of up to 47 characters, each with a done flag, the file rewritten
 * whole after every add, toggle or delete, no separate save step. Built with
 * no kernel include path, linked flat, loaded off the VFS by exec_user, and
 * it reaches the machine only through int 0x80: SYS_WINDOW_OPEN for a
 * framebuffer, SYS_WINDOW_POLL for input and the present, and the ordinary
 * file calls (open, read, write, close) for REMINDERS.TXT. No new syscall.
 *
 * File format, unchanged: one line per item, a '0' or '1' done flag, a
 * space, then the text, e.g. "1 buy milk". An empty list is saved as one
 * blank line so the truncate always leaves something behind; the loader and
 * Samantha's reminder tool both skip blank lines.
 *
 * A flat binary has no .bss and the RAM file system caps a file at 8KB, so
 * the list does not live in .data. exec_user zeroes the whole 128KB image
 * window before loading, so the list sits in the zeroed pages just past
 * _user_end, which the link script defines.
 *
 * Glyphs: antialiased DejaVu via libjt/text.h, same as Mail. a adds, space toggles done, d deletes, Esc closes.
 * tools/checks/ring3reminders-check.py drives all of it.
 */
#include "jtsys.h"
#include "libjt/text.h"

#define BG    0x00FAF8F6 /* GUI_BG */
#define INK   0x001C1C1E
#define HINT  0x0075726E
#define DIM   0x00807468
#define SEL   0x00EDE6DC
#define WHITE 0x00FFFFFF
#define GREEN 0x002F7B4F
#define GREY  0x00A39C92

#define MAXR   24
#define TEXT_N 48
#define ROW_Y  52
#define ROW_H  22

struct item { char text[TEXT_N]; int done; };
extern char _user_end[];

static struct jt_window_info win JT_DATA = {0, 0, 0, 0};
static struct item *list JT_DATA = 0;
static int count JT_DATA = 0;
static int sel JT_DATA = 0;
static int top JT_DATA = 0;
static int adding JT_DATA = 0;
static char entry[TEXT_N] JT_DATA = {0};
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
static int text(const char *s, int x, int y, unsigned fg) { return jt_text_draw(&win, JT_FACE_BODY, x, y, fg, s); }
/* Cuts s to maxw pixels, ending in "..." when it had to cut. */
static void fit(char *out, const char *s, int maxw, int cap) {
    int n = 0;
    while (s[n] && n < cap - 1) { out[n] = s[n]; n++; }
    out[n] = 0;
    if (jt_text_width(JT_FACE_BODY, out) <= maxw) return;
    while (n > 0) {
        out[n] = 0;
        char t[cap + 4]; int k = 0;
        for (int i = 0; i < n; i++) t[k++] = out[i];
        t[k++] = '.'; t[k++] = '.'; t[k++] = '.'; t[k] = 0;
        if (jt_text_width(JT_FACE_BODY, t) <= maxw) { for (int i = 0; i <= k && i < cap; i++) out[i] = t[i]; out[cap - 1] = 0; return; }
        n--;
    }
    out[0] = 0;
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

/* Read REMINDERS.TXT a chunk at a time and split it on newline. Each line is
   a done flag, a space, then the text; an over-long text keeps its head, a
   blank line is skipped. A missing file is an empty list. */
static void load(void) {
    count = 0;
    int fd = jt_open("REMINDERS.TXT", JT_O_RDONLY);
    if (fd < 0) return;
    char chunk[128], line[TEXT_N + 2];
    int p = 0, n;
    while ((n = jt_read(fd, chunk, sizeof chunk)) > 0) {
        for (int i = 0; i < n; i++) {
            char ch = chunk[i];
            if (ch == '\r') continue;
            if (ch == '\n') {
                if (p >= 3 && count < MAXR) {
                    struct item *it = &list[count++];
                    it->done = line[0] == '1';
                    int t = 0;
                    for (int k = 2; k < p && t < TEXT_N - 1; k++) it->text[t++] = line[k];
                    it->text[t] = 0;
                }
                p = 0;
                continue;
            }
            if (p < (int)sizeof line) line[p++] = ch;
        }
    }
    if (p >= 3 && count < MAXR) { /* last line had no newline */
        struct item *it = &list[count++];
        it->done = line[0] == '1';
        int t = 0;
        for (int k = 2; k < p && t < TEXT_N - 1; k++) it->text[t++] = line[k];
        it->text[t] = 0;
    }
    jt_close(fd);
}

static int save(void) {
    int fd = jt_open("REMINDERS.TXT", JT_O_WRONLY | JT_O_CREAT | JT_O_TRUNC);
    if (fd < 0) { jt_write(1, "reminders: save failed\n", 23); return 0; }
    char line[TEXT_N + 3];
    if (count == 0) jt_write(fd, "\n", 1);
    for (int i = 0; i < count; i++) {
        int l = 0;
        line[l++] = list[i].done ? '1' : '0';
        line[l++] = ' ';
        for (const char *s = list[i].text; *s; s++) line[l++] = *s;
        line[l++] = '\n';
        jt_write(fd, line, (unsigned)l);
    }
    if (jt_close(fd) < 0) { jt_write(1, "reminders: save failed\n", 23); return 0; }
    say("reminders: saved ", (unsigned)count);
    return 1;
}

static int visible_rows(void) {
    int v = ((int)win.height - ROW_Y) / ROW_H;
    return v < 1 ? 1 : v;
}

static void draw(void) {
    rect(0, 0, (int)win.width, (int)win.height, BG);
    if (adding) {
        text("type the reminder, enter adds, esc cancels:", 20, 36, HINT);
        rect(20, 60, (int)win.width - 40, 24, WHITE);
        {   char t[TEXT_N + 4]; int x;
            fit(t, entry, (int)win.width - 40 - 20, (int)sizeof t);
            x = text(t, 26, 63, INK);
            rect(x + 1, 65, 1, 14, INK); /* caret */
        }
        jt_write(1, "remindersprompt\n", 16); /* one per redraw: what the keystroke check counts */
        return;
    }
    if (count == 0) {
        text("No reminders yet.", 20, 40, INK);
        text("Press a to add one.", 20, 64, DIM);
        return;
    }
    {   char t[96];
        fit(t, "up/down to pick   space toggles done   d deletes   a adds   esc closes", (int)win.width - 40, (int)sizeof t);
        text(t, 20, 36, DIM);
    }
    int v = visible_rows();
    for (int i = top; i < count && i < top + v; i++) {
        int y = ROW_Y + (i - top) * ROW_H;
        if (i == sel) rect(16, y - 4, (int)win.width - 32, 20, SEL);
        text(list[i].done ? "[x]" : "[ ]", 28, y, list[i].done ? GREEN : INK);
        {   char t[TEXT_N + 4];
            fit(t, list[i].text, (int)win.width - 60 - 24, (int)sizeof t);
            text(t, 60, y, list[i].done ? GREY : INK);
        }
    }
}

static void keep_sel_visible(void) {
    int v = visible_rows();
    if (sel < top) top = sel;
    if (sel >= top + v) top = sel - v + 1;
    if (top < 0) top = 0;
}

static void finish_add(void) {
    adding = 0;
    if (entry_len == 0 || count >= MAXR) return;
    struct item *it = &list[count];
    for (int i = 0; i <= entry_len; i++) it->text[i] = entry[i];
    it->done = 0;
    count++;
    save();
    say("reminders: count ", (unsigned)count);
}

static void toggle_sel(void) {
    list[sel].done = !list[sel].done;
    save();
    say("reminders: done ", (unsigned)list[sel].done);
}

static void delete_sel(void) {
    for (int j = sel; j < count - 1; j++) list[j] = list[j + 1];
    count--;
    if (sel >= count && sel > 0) sel--;
    keep_sel_visible();
    save();
    say("reminders: count ", (unsigned)count);
}

__attribute__((section(".text.start"), used))
void _start(int argc, char **argv) {
    (void)argc; (void)argv;
    if (jt_window_open(&win) != 0 || !win.pixels) {
        jt_write(2, "reminders: no window\n", 21);
        jt_exit(1);
    }
    {   /* one line, one write: what tools/checks/ring3reminders-check.py asserts on */
        char line[48]; int l = 0;
        const char *pfx = "reminders: ring-3 window ";
        while (*pfx) line[l++] = *pfx++;
        l += utoa10(win.width, line + l); line[l++] = 'x';
        l += utoa10(win.height, line + l); line[l++] = '\n';
        jt_write(1, line, (unsigned)l);
    }

    list = (struct item *)(((unsigned)_user_end + 15u) & ~15u);
    load();
    say("reminders: loaded ", (unsigned)count);
    {   int ticked = 0;
        for (int i = 0; i < count; i++) ticked += list[i].done;
        say("reminders: checked ", (unsigned)ticked); /* proves a toggle came back off the file */
    }
    draw();

    unsigned flags = JT_POLL_PRESENT;
    for (;;) {
        struct jt_event ev;
        int r = jt_window_poll(&ev, flags);
        flags = 0;
        if (r == -11 /* -EAGAIN */) { jt_sched_yield(); continue; }
        if (r != 1) break;

        if (ev.kind == JT_EV_CLICK) {
            if (ev.a < 0 || ev.b < 0 || ev.a >= (int)win.width || ev.b >= (int)win.height) break; /* chrome X or dock */
            if (!adding) { flags = JT_POLL_PRESENT; continue; } /* click inside, no action */
            adding = 0; draw(); flags = JT_POLL_PRESENT; continue;
        }
        if (ev.kind != JT_EV_KEY) { flags = JT_POLL_PRESENT; continue; }
        int k = ev.a;
        if (adding) {
            if (k == JT_KEY_ESC) adding = 0;
            else if (k == JT_KEY_ENTER) finish_add();
            else if (k == 8 && entry_len > 0) entry[--entry_len] = 0;
            else if (entry_len < TEXT_N - 1 && k >= 32 && k < 127) {
                entry[entry_len++] = (char)k; entry[entry_len] = 0;
            }
        } else if (k == JT_KEY_ESC) break;
        else if (k == '`') { jt_write(1, "reminders: crashing on purpose\n", 31); *(volatile int *)0 = 1; } /* deliberate crash, as in every ring-3 app */
        else if (k == 'a') {
            if (count < MAXR) { adding = 1; entry_len = 0; entry[0] = 0; }
        } else if (count > 0) {
            if (k == JT_KEY_UP && sel > 0) { sel--; keep_sel_visible(); say("reminders: sel ", (unsigned)sel); }
            else if (k == JT_KEY_DOWN && sel < count - 1) { sel++; keep_sel_visible(); say("reminders: sel ", (unsigned)sel); }
            else if (k == ' ') toggle_sel();
            else if (k == 'd') delete_sel();
        }
        draw(); flags = JT_POLL_PRESENT;
    }
    jt_write(1, "reminders: closed\n", 18);
    jt_exit(0);
}
