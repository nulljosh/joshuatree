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
#include "../drivers/vgafont.h"

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
static void glyph(unsigned char ch, int x, int y, unsigned fg) {
    if (ch < VGAFONT_FIRST || ch > VGAFONT_LAST) ch = '?';
    const unsigned char *g = vgafont_glyphs + (ch - VGAFONT_FIRST) * 16;
    for (int r = 0; r < 16; r++) {
        int py = y + r;
        if (py < 0 || py >= (int)win.height) continue;
        for (int c = 0; c < 8; c++) {
            int px = x + c;
            if (px < 0 || px >= (int)win.width) continue;
            if (g[r] & (0x80 >> c)) win.pixels[(unsigned)py * win.width + (unsigned)px] = fg;
        }
    }
}
static void text(const char *s, int x, int y, unsigned fg) {
    for (; *s; s++, x += 8) glyph((unsigned char)*s, x, y, fg);
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
    text("Which film is this from?", 20, 56, HINT);
    text(QS_DECK[qs_cur()].line, 20, 96, INK);
    int right = qs_round % 4;
    for (int s = 0; s < 4; s++) {
        int y = QS_OPT_Y0 + s * QS_OPT_H;
        unsigned bg = OPT;
        if (qs_pick >= 0 && s == right) bg = RIGHT;   /* the answer, shown once you have picked */
        else if (qs_pick == s) bg = MISS;              /* your miss */
        rect(20, y, (int)win.width - 40, QS_OPT_H - 6, bg);
        char lab[2] = {(char)('1' + s), 0};
        text(lab, 30, y + 6, HINT);
        text(QS_DECK[qs_option(s)].film, 56, y + 6, INK);
    }
    char line[96]; int l = 0;
    const char *t;
    for (t = "streak "; *t; t++) line[l++] = *t;
    l += utoa10((unsigned)qs_streak, line + l);
    for (t = "   best "; *t; t++) line[l++] = *t;
    l += utoa10((unsigned)qs_best, line + l);
    for (t = qs_pick < 0 ? "   1-4 or click to answer   esc closes" : (qs_pick == right ? "   right. any key for the next one" : "   missed. any key for the next one"); *t; t++) line[l++] = *t;
    line[l] = 0;
    text(line, 20, (int)win.height - 30, HINT);
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
