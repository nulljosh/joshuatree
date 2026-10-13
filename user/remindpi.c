/* remindpi: Reminders for the ARM (Pi) build, a ring-3 program at EL0 (3.17.0).
 *
 * user/reminders.c is the i386 app: a "[ ]" list and an add prompt behind the a key. This is the macOS-shaped one the Pi
 * gets: a title and a count, a list of rows with round check circles (terracotta when done), and a text field along the
 * bottom that is always there. Type, Enter adds. Up from the field walks into the list; Space, Enter or a click on the
 * circle toggles done; Backspace or Delete on a row removes it; Down past the last row, Esc, or typing goes back to the
 * field; Esc in the field closes. A done item sinks to the bottom of the list; ticking it again lifts it above the done ones.
 *
 * The Makefile builds this file as the app called "reminders" (the dock tile and Spotlight match a table name to a tile's
 * name), so the Reminders tile opens it. It reaches the machine only through jtsys.h, as every EL0 app does.
 *
 * File: JT/REMINDER.TXT on the card (FAT keeps 8.3 names; i386's RAM file system calls the same file REMINDERS.TXT), the
 * line format i386 uses unchanged: one line per item, a '0' or '1' done flag, a space, then the text ("1 buy milk"); an
 * empty list is one blank line. Rewritten whole after every add, toggle or delete. With no card the open fails: the list
 * lives in memory only and the window says so under the title.
 *
 * The list sits in the zeroed pages past _user_end, as in user/reminders.c.
 * tools/checks/arm64-reminders-check.py drives it.
 */
#include "jtsys.h"
#include "libjt/text.h"

#define BG     0x00FAF8F6
#define INK    0x001C1C1E
#define DIM    0x0075726E
#define GREY   0x00A39C92
#define SEL    0x00F1EBE3
#define WHITE  0x00FFFFFF
#define RULE   0x00E4DED4
#define ACCENT 0x00B5502C

#define MAXR   24
#define TEXT_N 48
#define PAD    24
#define ROW_H  34
#define CIRC_R 10          /* circle radius */
#define FIELD_H 32
#define FILE_NAME "REMINDER.TXT"

struct item { char text[TEXT_N]; int done; };
extern char _user_end[];

static struct jt_window_info win JT_DATA = {0, 0, 0, 0};
static struct item *list JT_DATA = 0;
static int count JT_DATA = 0;
static int sel JT_DATA = -1;          /* the row, or -1 with the text field holding the keys */
static int top JT_DATA = 0;
static int nocard JT_DATA = 0;
static int full JT_DATA = 0;          /* an add was refused: the list is full */
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
static int text(int face, const char *s, int x, int y, unsigned fg) { return jt_text_draw(&win, face, x, y, fg, s); }

static unsigned blend(unsigned bg, unsigned fg, int a /* 0..16 */) {
    unsigned r = 0;
    for (int sh = 0; sh <= 16; sh += 8) {
        int b = (int)((bg >> sh) & 255), f = (int)((fg >> sh) & 255);
        r |= (unsigned)((b * (16 - a) + f * a) / 16) << sh;
    }
    return r;
}
/* A disc or a ring around (cx, cy), 4x4 samples a pixel so the edge is soft. ri 0 is a disc of radius ro; a ring is the
   band from ri to ro. Units inside are eighths of a pixel. */
static void round_mark(int cx, int cy, int ro, int ri, unsigned c) {
    long O = (long)ro * 8, I = (long)ri * 8;
    for (int y = cy - ro - 1; y <= cy + ro + 1; y++) {
        if (y < 0 || y >= (int)win.height) continue;
        for (int x = cx - ro - 1; x <= cx + ro + 1; x++) {
            if (x < 0 || x >= (int)win.width) continue;
            int hit = 0;
            for (int sy = 0; sy < 4; sy++) for (int sx = 0; sx < 4; sx++) {
                long dx = (long)x * 8 + sx * 2 + 1 - ((long)cx * 8 + 4), dy = (long)y * 8 + sy * 2 + 1 - ((long)cy * 8 + 4), d = dx * dx + dy * dy;
                if (d <= O * O && (ri == 0 || d >= I * I)) hit++;
            }
            if (!hit) continue;
            unsigned *p = win.pixels + (unsigned)y * win.width + (unsigned)x;
            *p = blend(*p, c, hit);
        }
    }
}
static void dot2(int x, int y, unsigned c) { rect(x, y, 2, 2, c); }
static void tick_mark(int cx, int cy) {   /* a white check inside a filled circle */
    int x0 = cx - 5, y0 = cy, x1 = cx - 2, y1 = cy + 3, x2 = cx + 5, y2 = cy - 4;
    for (int i = 0; i <= 3; i++) dot2(x0 + (x1 - x0) * i / 3, y0 + (y1 - y0) * i / 3, WHITE);
    for (int i = 0; i <= 7; i++) dot2(x1 + (x2 - x1) * i / 7, y1 + (y2 - y1) * i / 7, WHITE);
}
/* Cuts s to maxw pixels, ending in "..." when it had to cut. */
static void fit(char *out, const char *s, int maxw, int cap) {
    int n = 0;
    while (s[n] && n < cap - 1) { out[n] = s[n]; n++; }
    out[n] = 0;
    if (jt_text_width(JT_FACE_BODY, out) <= maxw) return;
    while (n > 0) {
        char t[TEXT_N + 4]; int k = 0;
        for (int i = 0; i < n - 1 && k < cap - 4; i++) t[k++] = s[i];
        t[k++] = '.'; t[k++] = '.'; t[k++] = '.'; t[k] = 0;
        if (jt_text_width(JT_FACE_BODY, t) <= maxw || n == 1) { for (int i = 0; i <= k && i < cap; i++) out[i] = t[i]; out[cap - 1] = 0; return; }
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
static void say(const char *pfx, unsigned v) {   /* one line, one write: what the check reads */
    char line[48]; int l = 0;
    while (*pfx) line[l++] = *pfx++;
    l += utoa10(v, line + l); line[l++] = '\n';
    jt_write(1, line, (unsigned)l);
}
static void add_item(const char *line, int p) {   /* one "D text" line off the file */
    if (p < 3 || count >= MAXR) return;
    struct item *it = &list[count++];
    it->done = line[0] == '1';
    int t = 0;
    for (int k = 2; k < p && t < TEXT_N - 1; k++) it->text[t++] = line[k];
    it->text[t] = 0;
}
static void load(void) {
    count = 0;
    int fd = jt_open(FILE_NAME, JT_O_RDONLY);
    if (fd < 0) return;
    char chunk[128], line[TEXT_N + 2];
    int p = 0, n;
    while ((n = jt_read(fd, chunk, sizeof chunk)) > 0) {
        for (int i = 0; i < n; i++) {
            char ch = chunk[i];
            if (ch == '\r') continue;
            if (ch == '\n') { add_item(line, p); p = 0; continue; }
            if (p < (int)sizeof line) line[p++] = ch;
        }
    }
    add_item(line, p);   /* a last line with no newline */
    jt_close(fd);
}
static int save(void) {
    int fd = jt_open(FILE_NAME, JT_O_WRONLY | JT_O_CREAT | JT_O_TRUNC);
    if (fd < 0) { nocard = 1; return 0; }
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
    if (jt_close(fd) < 0) { nocard = 1; return 0; }
    nocard = 0;
    say("reminders: saved ", (unsigned)count);
    return 1;
}

static int field_y(void) { return (int)win.height - FIELD_H - 14; }
static int list_y(void) { return 18 + jt_text_height(JT_FACE_BOLD) + 6 + jt_text_height(JT_FACE_BODY) + 14; }
static int visible_rows(void) {
    int v = (field_y() - 10 - list_y()) / ROW_H;
    return v < 1 ? 1 : v;
}
static void keep_sel_visible(void) {
    int v = visible_rows();
    if (sel < 0) return;
    if (sel < top) top = sel;
    if (sel >= top + v) top = sel - v + 1;
    if (top < 0) top = 0;
}
static int todo(void) { int n = 0; for (int i = 0; i < count; i++) n += !list[i].done; return n; }

static void draw(void) {
    rect(0, 0, (int)win.width, (int)win.height, BG);
    int w = (int)win.width, ty = 18;
    text(JT_FACE_BOLD, "Reminders", PAD, ty, ACCENT);
    {   char sub[40]; int l = 0;
        const char *s;
        if (nocard) s = "No card: reminders stay in memory only";
        else if (count == 0) s = "Nothing to do";
        else if (todo() == 0) s = "All done";
        else { l = utoa10((unsigned)todo(), sub); s = " to do"; for (int i = 0; s[i]; i++) sub[l++] = s[i]; sub[l] = 0; s = sub; }
        text(JT_FACE_BODY, s, PAD, ty + jt_text_height(JT_FACE_BOLD) + 6, nocard ? ACCENT : DIM);
    }
    int ly = list_y(), v = visible_rows();
    if (top > count - v) top = count - v;
    if (top < 0) top = 0;
    for (int i = top; i < count && i < top + v; i++) {
        int y = ly + (i - top) * ROW_H, cy = y + ROW_H / 2;
        if (i == sel) rect(PAD - 10, y + 1, w - 2 * PAD + 20, ROW_H - 2, SEL);
        if (list[i].done) { round_mark(PAD + CIRC_R, cy, CIRC_R, 0, ACCENT); tick_mark(PAD + CIRC_R, cy); }
        else round_mark(PAD + CIRC_R, cy, CIRC_R, CIRC_R - 2, GREY);
        char t[TEXT_N + 4];
        fit(t, list[i].text, w - (PAD + 2 * CIRC_R + 14) - PAD, (int)sizeof t);
        text(JT_FACE_BODY, t, PAD + 2 * CIRC_R + 14, cy - jt_text_height(JT_FACE_BODY) / 2, list[i].done ? GREY : INK);
        if (i + 1 < count && i + 1 < top + v) rect(PAD + 2 * CIRC_R + 14, y + ROW_H - 1, w - (PAD + 2 * CIRC_R + 14) - PAD, 1, RULE);
    }
    /* the text field: a white bar with a hairline border, the caret when it holds the keys */
    int fy = field_y(), fx = PAD, fw = w - 2 * PAD;
    rect(fx, fy, fw, FIELD_H, sel < 0 ? ACCENT : RULE);
    rect(fx + 1, fy + 1, fw - 2, FIELD_H - 2, WHITE);
    int tx = fx + 12, tyy = fy + (FIELD_H - jt_text_height(JT_FACE_BODY)) / 2;
    if (entry_len == 0) {
        text(JT_FACE_BODY, full ? "The list is full" : "New Reminder", tx, tyy, GREY);
        if (sel < 0) rect(tx, tyy, 1, jt_text_height(JT_FACE_BODY), INK);
    } else {
        char t[TEXT_N + 4];
        fit(t, entry, fw - 24, (int)sizeof t);
        int x = text(JT_FACE_BODY, t, tx, tyy, INK);
        if (sel < 0) rect(x + 1, tyy, 1, jt_text_height(JT_FACE_BODY), INK);
    }
}

/* Done items sink: a ticked item goes to the end of the list, an un-ticked one to the end of the undone block. Returns
   where the item landed. */
static int place(int i) {
    struct item it = list[i];
    for (int j = i; j < count - 1; j++) list[j] = list[j + 1];
    int at = count - 1;
    if (!it.done) { at = 0; for (int j = 0; j < count - 1; j++) if (!list[j].done) at = j + 1; }
    for (int j = count - 1; j > at; j--) list[j] = list[j - 1];
    list[at] = it;
    return at;
}
static void add_entry(void) {
    if (entry_len == 0) return;
    if (count >= MAXR) { full = 1; return; }
    struct item *it = &list[count];
    for (int i = 0; i <= entry_len; i++) it->text[i] = entry[i];
    it->done = 0;
    count++;
    sel = -1; entry_len = 0; entry[0] = 0;
    int at = place(count - 1);
    top = at + 1 - visible_rows();
    save();
    say("reminders: count ", (unsigned)count);
}
static void toggle(int i) {
    list[i].done = !list[i].done;
    sel = place(i);
    keep_sel_visible();
    save();
    say("reminders: done ", (unsigned)list[sel].done);
}
static void remove_row(int i) {
    for (int j = i; j < count - 1; j++) list[j] = list[j + 1];
    count--;
    if (sel >= count) sel = count - 1;   /* -1 when the list is empty: the field again */
    keep_sel_visible();
    full = 0;
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
    {   char line[48]; int l = 0;
        const char *pfx = "reminders: ring-3 window ";
        while (*pfx) line[l++] = *pfx++;
        l += utoa10(win.width, line + l); line[l++] = 'x';
        l += utoa10(win.height, line + l); line[l++] = '\n';
        jt_write(1, line, (unsigned)l);
    }
    list = (struct item *)(((unsigned long)_user_end + 15ul) & ~15ul);
    load();
    if (count == 0 && !save()) jt_write(1, "reminders: no card, memory only\n", 32);   /* an empty save is how a missing card shows */
    say("reminders: loaded ", (unsigned)count);
    say("reminders: checked ", (unsigned)(count - todo()));   /* done items that came back off the card */
    draw();

    unsigned flags = JT_POLL_PRESENT;
    for (;;) {
        struct jt_event ev;
        int r = jt_window_poll(&ev, flags);
        if (r == 1 && jt_window_resized(&ev, &win)) { draw(); flags = JT_POLL_PRESENT; continue; }
        flags = 0;
        if (r == -11 /* -EAGAIN */) { jt_sched_yield(); continue; }
        if (r != 1) break;

        if (ev.kind == JT_EV_CLICK) {
            if (ev.a < 0 || ev.b < 0 || ev.a >= (int)win.width || ev.b >= (int)win.height) break;   /* chrome X or dock */
            int ly = list_y(), v = visible_rows();
            if (ev.b >= ly && ev.b < ly + v * ROW_H && top + (ev.b - ly) / ROW_H < count) {
                int i = top + (ev.b - ly) / ROW_H;
                sel = i;
                if (ev.a < PAD + 2 * CIRC_R + 8) toggle(i);
                keep_sel_visible();
            } else if (ev.b >= field_y()) sel = -1;
            draw(); flags = JT_POLL_PRESENT; continue;
        }
        if (ev.kind != JT_EV_KEY) { flags = JT_POLL_PRESENT; continue; }
        int k = ev.a;
        if (k == '`') { jt_write(1, "reminders: crashing on purpose\n", 31); *(volatile int *)0 = 1; }
        if (sel < 0) {   /* the text field has the keys */
            if (k == JT_KEY_ESC) break;
            else if (k == JT_KEY_ENTER) add_entry();
            else if (k == 8 && entry_len > 0) { entry[--entry_len] = 0; full = 0; }
            else if (k == JT_KEY_UP && count > 0) { sel = count - 1; keep_sel_visible(); say("reminders: sel ", (unsigned)sel); }
            else if (entry_len < TEXT_N - 1 && k >= 32 && k < 127) { entry[entry_len++] = (char)k; entry[entry_len] = 0; full = 0; }
        } else {   /* a row is selected */
            if (k == JT_KEY_ESC) sel = -1;
            else if (k == JT_KEY_UP && sel > 0) { sel--; keep_sel_visible(); say("reminders: sel ", (unsigned)sel); }
            else if (k == JT_KEY_DOWN) { if (sel < count - 1) { sel++; keep_sel_visible(); say("reminders: sel ", (unsigned)sel); } else sel = -1; }
            else if (k == ' ' || k == JT_KEY_ENTER) toggle(sel);
            else if (k == 8 || k == JT_KEY_DELETE) remove_row(sel);
            else if (k >= 33 && k < 127 && entry_len < TEXT_N - 1) { sel = -1; entry[entry_len++] = (char)k; entry[entry_len] = 0; }   /* typing goes back to the field */
        }
        draw(); flags = JT_POLL_PRESENT;
    }
    jt_write(1, "reminders: closed\n", 18);
    jt_exit(0);
}
