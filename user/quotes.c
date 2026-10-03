/* quotes: the film-quote guessing game, as a real ring-3 program.
 *
 * The fourth app to leave the kernel (roadmap 2.0), done exactly the way
 * user/keyrate.c, user/toroid.c and user/calculator.c were. Same deck and
 * game drivers/app_quotestreak.c ran in ring 0: a line from a film, four
 * titles, pick the right one and the streak grows, miss and it resets.
 * Built with no kernel include path, linked flat, loaded off the VFS by
 * exec_user, and it reaches the machine only through int 0x80:
 * SYS_WINDOW_OPEN for a framebuffer, SYS_WINDOW_POLL for input and the
 * present, SYS_EXIT to leave.
 *
 * Glyphs: the kernel's 8x16 VGA fallback font, same as Keyrate, Toroid
 * and Calculator. The backquote key (`) is the deliberate crash, same as
 * the other three: a write through a null pointer, a page fault at ring
 * 3, reaped by the kernel. tools/checks/ring3quotes-check.py presses it
 * on purpose.
 */
#include "jtsys.h"
#include "libjt/text.h"

#define BG    0x00FAF8F6 /* GUI_BG */
#define OPT   0x00F1EDE7
#define RIGHT 0x00CFE8D2
#define MISS  0x00F0D0CC
#define INK   0x001C1C1E
#define HINT  0x0075726E

#define QS_OPT_Y0 150
#define QS_OPT_H  34

typedef struct { const char *line; const char *film; } QsQuote;
static const QsQuote QS_DECK[] = {
    {"I'm going to make him an offer he can't refuse.", "The Godfather"},
    {"May the Force be with you.", "Star Wars"},
    {"You talking to me?", "Taxi Driver"},
    {"Here's looking at you, kid.", "Casablanca"},
    {"I'll be back.", "The Terminator"},
    {"You can't handle the truth!", "A Few Good Men"},
    {"Why so serious?", "The Dark Knight"},
    {"Life is like a box of chocolates.", "Forrest Gump"},
    {"I see dead people.", "The Sixth Sense"},
    {"Houston, we have a problem.", "Apollo 13"},
    {"There's no place like home.", "The Wizard of Oz"},
    {"To infinity and beyond!", "Toy Story"},
    {"Roads? Where we're going we don't need roads.", "Back to the Future"},
    {"Sell me this pen.", "The Wolf of Wall Street"},
    {"The first rule of Fight Club is: you do not talk about Fight Club.", "Fight Club"},
    {"Just keep swimming.", "Finding Nemo"},
};
#define QS_COUNT ((int)(sizeof(QS_DECK) / sizeof(QS_DECK[0])))

static struct jt_window_info win JT_DATA = {0, 0, 0, 0};
static int qs_round JT_DATA = 0, qs_streak JT_DATA = 0, qs_best JT_DATA = 0, qs_pick JT_DATA = -1;

static int qs_cur(void){ return (qs_round * 7) % QS_COUNT; } /* 7 is coprime with 16, so every line comes up once per lap */
/* Option slot -> deck index. The right answer sits in slot (round % 4);
   the other three are the next deck rows that are not the answer. */
static int qs_option(int slot){
    int right = qs_round % 4, cur = qs_cur();
    if (slot == right) return cur;
    int n = slot < right ? slot : slot - 1;
    return (cur + 3 + n * 5) % QS_COUNT == cur ? (cur + 1) % QS_COUNT : (cur + 3 + n * 5) % QS_COUNT;
}

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
static int text(const char *s, int x, int y, unsigned fg) { /* returns the x after the last glyph */
    return jt_text_draw(&win, JT_FACE_BODY, x, y, fg, s);
}
static void etext(const char *s, int x, int y, int maxw, unsigned fg) {
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
    jt_text_draw(&win, JT_FACE_BODY, x, y, fg, t);
}
/* Word wrap in the bold face, breaking only at spaces, 22 px a line. */
static void bwrap(const char *s, int x, int y, int w, int ymax, unsigned fg) {
    char line[96]; int ll = 0;
    while (*s && y + 20 <= ymax) {
        int we = 0;
        while (s[we] && s[we] != ' ') we++;
        char trial[96]; int tl = 0;
        for (int i = 0; i < ll; i++) trial[tl++] = line[i];
        if (ll) trial[tl++] = ' ';
        for (int i = 0; i < we && tl < 94; i++) trial[tl++] = s[i];
        trial[tl] = 0;
        if (ll && jt_text_width(JT_FACE_BOLD, trial) > w) {
            line[ll] = 0; jt_text_draw(&win, JT_FACE_BOLD, x, y, fg, line); y += 22; ll = 0;
            continue;
        }
        for (int i = 0; i <= tl; i++) line[i] = trial[i];
        ll = tl; s += we;
        while (*s == ' ') s++;
    }
    if (ll && y + 20 <= ymax) { line[ll] = 0; jt_text_draw(&win, JT_FACE_BOLD, x, y, fg, line); }
}
static int utoa10(unsigned v, char *buf) {
    char tmp[12]; int tn = 0, n = 0;
    do { tmp[tn++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (tn) buf[n++] = tmp[--tn];
    buf[n] = 0;
    return n;
}

static void qs_draw(void) {
    rect(0, 0, (int)win.width, (int)win.height, BG);
    text("Which film is this from?", 20, 40, HINT);
    bwrap(QS_DECK[qs_cur()].line, 20, 70, (int)win.width - 40, QS_OPT_Y0 - 8, INK);
    int right = qs_round % 4;
    for (int s = 0; s < 4; s++) {
        int y = QS_OPT_Y0 + s * QS_OPT_H;
        unsigned bg = OPT;
        if (qs_pick >= 0 && s == right) bg = RIGHT;   /* the answer, shown once you have picked */
        else if (qs_pick == s) bg = MISS;              /* your miss */
        rect(20, y, (int)win.width - 40, QS_OPT_H - 6, bg);
        char lab[2] = {(char)('1' + s), 0};
        text(lab, 30, y + 5, HINT);
        etext(QS_DECK[qs_option(s)].film, 56, y + 5, (int)win.width - 96, INK);
    }
    char line[40]; int l = 0;
    const char *t;
    for (t = "streak "; *t; t++) line[l++] = *t;
    l += utoa10((unsigned)qs_streak, line + l);
    for (t = "   best "; *t; t++) line[l++] = *t;
    l += utoa10((unsigned)qs_best, line + l);
    line[l] = 0;
    text(line, 20, (int)win.height - 52, INK);
    etext(qs_pick < 0 ? "1-4 or click to answer   esc closes" : (qs_pick == right ? "right. any key for the next one" : "missed. any key for the next one"), 20, (int)win.height - 30, (int)win.width - 40, HINT);
}

__attribute__((section(".text.start"), used))
void _start(int argc, char **argv) {
    (void)argc; (void)argv;
    if (jt_window_open(&win) != 0 || !win.pixels) {
        jt_write(2, "quotes: no window\n", 19);
        jt_exit(1);
    }
    {   /* one line, one write: what tools/checks/ring3quotes-check.py asserts on */
        char line[48]; int l = 0;
        const char *pfx = "quotes: ring-3 window ";
        while (*pfx) line[l++] = *pfx++;
        l += utoa10(win.width, line + l); line[l++] = 'x';
        l += utoa10(win.height, line + l); line[l++] = '\n';
        jt_write(1, line, (unsigned)l);
    }

    qs_round = 0; qs_streak = 0; qs_best = 0; qs_pick = -1;
    qs_draw();

    unsigned flags = JT_POLL_PRESENT;
    for (;;) {
        struct jt_event ev;
        int r = jt_window_poll(&ev, flags);
        if (r == 1 && jt_window_resized(&ev, &win)) { qs_draw(); flags = JT_POLL_PRESENT; continue; } /* JT_EV_RESIZE: remapped, repaint at the new size */
        flags = 0;
        if (r == -11 /* -EAGAIN */) { jt_sched_yield(); continue; }
        if (r != 1) break;

        int slot = -1;
        if (ev.kind == JT_EV_CLICK) {
            if (ev.b >= QS_OPT_Y0 && ev.b < QS_OPT_Y0 + 4 * QS_OPT_H && ev.a >= 20 && ev.a < (int)win.width - 20)
                slot = (ev.b - QS_OPT_Y0) / QS_OPT_H;
            else
                break; /* the titlebar X, or anywhere off the answers, same as every other app */
        } else if (ev.kind == JT_EV_KEY) {
            if (ev.a == JT_KEY_ESC) break;
            if (ev.a == '`') {
                jt_write(1, "quotes: crashing on purpose\n", 29);
                *(volatile int *)0 = 1;
            }
            if (ev.a >= '1' && ev.a <= '4') slot = ev.a - '1';
        } else {
            flags = JT_POLL_PRESENT;
            continue;
        }

        if (qs_pick >= 0) {
            qs_pick = -1; qs_round++;
            /* One line, one write: the marker tools/checks/ring3quotes-check.py
               reads to confirm the round advanced past a picked answer. */
            char line[16]; int l = 0;
            const char *pfx = "quotes: next\n"; while (*pfx) line[l++] = *pfx++;
            jt_write(1, line, (unsigned)l);
            qs_draw(); flags = JT_POLL_PRESENT;
            continue;
        }
        if (slot < 0) { flags = JT_POLL_PRESENT; continue; }
        qs_pick = slot;
        int right = qs_round % 4;
        if (slot == right) { qs_streak++; if (qs_streak > qs_best) qs_best = qs_streak; }
        else qs_streak = 0;
        /* One line, one write: what tools/checks/ring3quotes-check.py reads
           to confirm the real parser/answer logic ran, not just the draw. */
        char line[32]; int l = 0;
        const char *pfx = "quotes: pick "; while (*pfx) line[l++] = *pfx++;
        line[l++] = (char)('1' + slot);
        const char *sfx = slot == right ? " right\n" : " miss\n";
        while (*sfx) line[l++] = *sfx++;
        jt_write(1, line, (unsigned)l);
        qs_draw(); flags = JT_POLL_PRESENT;
    }
    jt_write(1, "quotes: closed\n", 16);
    jt_exit(0);
}
