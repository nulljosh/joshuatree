/* mailpi: Mail for the ARM (Pi) build, three panes at the Mac Mail scale: mailboxes, the message list, the reading pane.
 *
 * Built only by arch/arm64/Makefile, as the app called "mail" (the Mail dock tile, Spotlight and Samantha's [[open mail]]
 * start it). i386 keeps user/mail.c. Read only: no network mail, no passwords, no address book, nothing written to the card.
 * The mail is MAIL.TXT in the card's JT/ folder when there is one, the very format user/mail.c keeps ("r|from|subject|body"
 * lines, a first character of u unread, r read, s sent, x unsent), else a handful of made-up sample messages, and the
 * window says so ("Sample messages" under the mailboxes). Up and Down move in the list, Left and Right change mailbox,
 * Enter opens the message in the reading pane (and marks it read in memory), Esc closes the message, then the window.
 * Serial markers: mail: ring-3 window, mail: n=COUNT samples=0|1, mail: box=I, mail: sel=I, mail: read=I (the message's
 * place in the file), mail: closed.
 */
#include "jtsys.h"
#include "libjt/text.h"
#include "libjt/hires.h"

#define BG     0x00FAF8F6
#define INK    0x001C1C1E
#define DIM    0x00807468
#define FADE   0x00A39C92
#define SEL    0x00EDE6DC
#define RULE   0x00E0D8CE
#define SIDE   0x00F1EDE6
#define ACCENT 0x00B5502C
#define WHITE  0x00FFFFFF

#define MAX 24
#define SIDE_W 112
#define LIST_W 196
#define ROW_H 46
#define LIST_Y0 34
#define BOX_Y0 16
#define BOX_H 26
#define LINE 18

struct msg { const char *from, *subject, *body, *date; int read, sent; };   /* sent: 0 plain, 1 sent, 2 not sent */
static const char *const box_name[3] = {"Inbox", "Unread", "Sent"};

static struct jt_window_info win JT_DATA = {0, 0, 0, 0};
extern char _user_end[];
struct arena { struct msg msgs[MAX]; char filebuf[4096]; };   /* past _user_end: a flat image has no .bss */
static struct arena *ar JT_DATA = 0;
#define msgs (ar->msgs)
static int count JT_DATA = 0;
static int samples JT_DATA = 0;
static int box JT_DATA = 0;        /* 0 Inbox, 1 Unread, 2 Sent */
static int vis[MAX] JT_DATA;       /* message indexes in the chosen mailbox */
static int nvis JT_DATA = 0;
static int sel JT_DATA = 0;        /* row in vis */
static int opened JT_DATA = -1;    /* message index shown in the reading pane, -1 for none */

static void rect(int x, int y, int w, int h, unsigned c) { jt_fill(&win, x, y, w, h, c); }   /* hires.h: logical pixels, sharp at scale 2 */
static void text(const char *s, int x, int y, unsigned fg) { jt_text_draw(&win, JT_FACE_BODY, x, y, fg, s); }
static void btext(const char *s, int x, int y, unsigned fg) { jt_text_draw(&win, JT_FACE_BOLD, x, y, fg, s); }
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
static void fit(char *out, const char *s, int maxw, int face) {   /* cut to a pixel width */
    int n = 0;
    while (s[n] && n < 100) { out[n] = s[n]; n++; }
    out[n] = 0;
    while (n > 1 && jt_text_width(face, out) > maxw) out[--n] = 0;
}

static void add_sample(const char *f, const char *s, const char *b, const char *d, int read, int sent) {
    struct msg *m = &msgs[count++];
    m->from = f; m->subject = s; m->body = b; m->date = d; m->read = read; m->sent = sent;
}
/* Made-up messages, labelled as samples on screen. No real people, addresses or accounts. */
static void seed(void) {
    samples = 1;
    add_sample("Joshua Tree", "Welcome to Mail",
        "This is the Mail window on the Pi. The messages you see here are samples that ship with the system, not real mail. "
        "Put a MAIL.TXT in the JT folder on the card and they are replaced by yours.", "Oct 12", 0, 0);
    add_sample("Dock Weekly", "Ten things the dock does",
        "Hover a tile for its name. Left and Right walk the dock from the keyboard, Enter opens the tile, and F1 opens Spotlight to find anything by name.", "Oct 11", 0, 0);
    add_sample("Field Notes", "Your reading list",
        "Three books are waiting in the Books app. Drop plain text files into the BOOKS folder on the card and they show up in the library.", "Oct 9", 1, 0);
    add_sample("Calendar", "Tomorrow at a glance",
        "Nothing on the calendar for tomorrow. A sample reminder: water the plants, then take the long way home.", "Oct 8", 1, 0);
    add_sample("Sample Sender", "A message with a longer subject line than fits",
        "Long subjects are cut to the width of the list, and the whole subject is shown in the reading pane above the body. "
        "Bodies wrap to the width of the window, so a narrower window just means more lines.", "Oct 6", 1, 0);
    add_sample("To: sample@example.com", "Re: Welcome to Mail",
        "Thanks, the three panes look right. Sent messages live in their own mailbox on the left.", "Oct 12", 1, 1);
}

/* user/mail.c's format, parsed in place: the separators become NULs. */
static void load(void) {
    char *b = ar->filebuf;
    int n = jt_readfile("MAIL.TXT", b, 4096 - 1);
    if (n <= 0) { seed(); return; }
    b[n] = 0;
    int i = 0;
    while (i < n && count < MAX) {
        struct msg *m = &msgs[count];
        m->read = (b[i] != 'u');
        m->sent = b[i] == 's' ? 1 : b[i] == 'x' ? 2 : 0;
        m->date = "";
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
    if (!count) seed();
}

static int in_box(int i, int bx) { return bx == 2 ? msgs[i].sent != 0 : msgs[i].sent == 0 && (bx == 0 || !msgs[i].read); }
static int box_count(int bx) { int c = 0; for (int i = 0; i < count; i++) if (in_box(i, bx)) c++; return c; }
static void rebuild(void) { nvis = 0; for (int i = 0; i < count; i++) if (in_box(i, box)) vis[nvis++] = i; if (sel >= nvis) sel = nvis ? nvis - 1 : 0; }
static void num(char *out, int v) { int n = 0; char d[8]; if (!v) d[n++] = '0'; while (v) { d[n++] = (char)('0' + v % 10); v /= 10; } for (int i = 0; i < n; i++) out[i] = d[n - 1 - i]; out[n] = 0; }

/* Flat glyphs, a few filled rectangles each, in the accent: a tray, a dot, an arrow. */
static void glyph(int bx, int x, int y, unsigned c) {
    if (bx == 0) { rect(x, y + 2, 2, 9, c); rect(x + 12, y + 2, 2, 9, c); rect(x, y + 9, 14, 2, c); rect(x + 4, y + 5, 6, 2, c); rect(x + 6, y + 2, 2, 3, c); }
    else if (bx == 1) { rect(x + 3, y + 1, 8, 12, c); rect(x + 1, y + 3, 12, 8, c); }
    else { rect(x, y + 6, 11, 3, c); rect(x + 8, y + 2, 3, 11, c); rect(x + 11, y + 4, 3, 7, c); }
}

static int wrapped(const char *s, int x, int y, int w, int ymax, unsigned fg) {   /* word wrap; returns the y below the last line */
    char line[120], trial[120];
    int ll = 0;
    while (*s && y + LINE <= ymax) {
        int we = 0;
        while (s[we] && s[we] != ' ') we++;
        int tl = 0;
        for (int i = 0; i < ll; i++) trial[tl++] = line[i];
        if (ll) trial[tl++] = ' ';
        for (int i = 0; i < we && tl < 118; i++) trial[tl++] = s[i];
        trial[tl] = 0;
        if (ll && jt_text_width(JT_FACE_BODY, trial) > w) { line[ll] = 0; text(line, x, y, fg); y += LINE; ll = 0; continue; }
        for (int i = 0; i <= tl; i++) line[i] = trial[i];
        ll = tl;
        s += we;
        while (*s == ' ') s++;
    }
    if (ll && y + LINE <= ymax) { line[ll] = 0; text(line, x, y, fg); y += LINE; }
    return y;
}

static void draw(void) {
    char t[104], c[8];
    int W = (int)win.width, H = (int)win.height, lx = SIDE_W + 1, rx = SIDE_W + LIST_W + 2;
    rect(0, 0, W, H, BG);
    /* mailboxes */
    rect(0, 0, SIDE_W, H, SIDE);
    rect(SIDE_W, 0, 1, H, RULE);
    for (int b = 0; b < 3; b++) {
        int y = BOX_Y0 + b * BOX_H;
        if (b == box) rect(6, y - 4, SIDE_W - 12, BOX_H - 2, SEL);
        glyph(b, 14, y, ACCENT);
        (b == box ? btext : text)(box_name[b], 36, y - 1, INK);
        num(c, box_count(b));
        text(c, SIDE_W - 14 - jt_text_width(JT_FACE_BODY, c), y - 1, DIM);
    }
    if (samples) { text("Sample", 14, H - 44, DIM); text("messages", 14, H - 26, DIM); }
    /* message list */
    btext(box_name[box], lx + 12, 8, INK);
    num(c, nvis);
    text(c, lx + LIST_W - 12 - jt_text_width(JT_FACE_BODY, c), 8, DIM);
    rect(lx, LIST_Y0 - 2, LIST_W, 1, RULE);
    rect(lx + LIST_W, 0, 1, H, RULE);
    if (!nvis) text("No messages", lx + 12, LIST_Y0 + 16, DIM);
    for (int r = 0; r < nvis; r++) {
        int y = LIST_Y0 + r * ROW_H;
        if (y + ROW_H > H) break;
        struct msg *m = &msgs[vis[r]];
        if (r == sel) rect(lx + 4, y + 1, LIST_W - 8, ROW_H - 2, SEL);
        else rect(lx + 12, y + ROW_H - 1, LIST_W - 24, 1, RULE);
        if (!m->read) rect(lx + 9, y + 9, 7, 7, ACCENT);
        int dw = jt_text_width(JT_FACE_BODY, m->date);
        text(m->date, lx + LIST_W - 12 - dw, y + 4, DIM);
        fit(t, m->from, LIST_W - 40 - dw - 8, JT_FACE_BOLD);
        (m->read ? text : btext)(t, lx + 22, y + 4, m->read ? FADE : INK);
        fit(t, m->subject, LIST_W - 40, JT_FACE_BODY);
        text(t, lx + 22, y + 24, m->read ? FADE : DIM);
    }
    /* reading pane */
    if (opened < 0) {
        text(nvis ? "Press Enter to read" : "Nothing to read", rx + 24, H / 2 - 20, FADE);
        return;
    }
    struct msg *m = &msgs[opened];
    int pw = W - rx - 24;
    fit(t, m->subject, pw, JT_FACE_BOLD);
    btext(t, rx + 12, 14, INK);
    fit(t, m->from, pw - 70, JT_FACE_BODY);
    text(t, rx + 12, 38, DIM);
    text(m->date, W - 12 - jt_text_width(JT_FACE_BODY, m->date), 38, DIM);
    rect(rx + 12, 62, W - rx - 24, 1, RULE);
    wrapped(m->body, rx + 12, 74, pw, H - 8, INK);
}

static void open_row(int r) {
    if (r < 0 || r >= nvis) return;
    sel = r; opened = vis[r];
    msgs[opened].read = 1;
    say("mail: read=", opened);
}
static void set_box(int b) {
    if (b < 0 || b > 2 || b == box) return;
    box = b; sel = 0; opened = -1;
    rebuild();
    say("mail: box=", box);
}

__attribute__((section(".text.start"), used))
void _start(int argc, char **argv) {
    (void)argc; (void)argv;
    if (jt_window_open_hires(&win) != 0 || !win.pixels) { jt_write(2, "mail: no window\n", 16); jt_exit(1); }
    ar = (struct arena *)(((unsigned long)_user_end + 15u) & ~15ul);
    load();
    rebuild();
    draw();
    jt_write(1, "mail: ring-3 window\n", 20);
    say("mail: n=", count);
    say("mail: samples=", samples);
    unsigned flags = JT_POLL_PRESENT;
    for (;;) {
        struct jt_event ev;
        int r = jt_window_poll(&ev, flags);
        if (r == 1 && jt_window_resized(&ev, &win)) { draw(); flags = JT_POLL_PRESENT; continue; }
        flags = 0;
        if (r == -11) { jt_sched_yield(); continue; }
        if (r != 1) break;
        if (ev.kind == JT_EV_CLICK) {
            if (ev.b < 0) break;   /* the red dot */
            if (ev.a < SIDE_W) { int b = (ev.b - (BOX_Y0 - 4)) / BOX_H; if (ev.b >= BOX_Y0 - 4) set_box(b); }
            else if (ev.a < SIDE_W + LIST_W && ev.b >= LIST_Y0) {
                int hit = (ev.b - LIST_Y0) / ROW_H;
                if (hit >= 0 && hit < nvis) { if (hit == sel) open_row(hit); else { sel = hit; say("mail: sel=", sel); } }
            }
        } else if (ev.kind == JT_EV_KEY) {
            int k = ev.a;
            if (k == JT_KEY_ESC) { if (opened >= 0) opened = -1; else break; }
            else if (k == JT_KEY_UP && sel > 0) { sel--; say("mail: sel=", sel); }
            else if (k == JT_KEY_DOWN && sel < nvis - 1) { sel++; say("mail: sel=", sel); }
            else if (k == JT_KEY_LEFT) set_box(box - 1);
            else if (k == JT_KEY_RIGHT) set_box(box + 1);
            else if (k == JT_KEY_ENTER) open_row(sel);
        } else { flags = JT_POLL_PRESENT; continue; }
        draw();
        flags = JT_POLL_PRESENT;
    }
    jt_write(1, "mail: closed\n", 13);
    jt_exit(0);
}
