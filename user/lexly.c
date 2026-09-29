/* lexly: Spanish vocabulary flashcard drill, as a real ring-3 program.
 *
 * The seventh app to leave the kernel (roadmap 2.0), done the way
 * user/quotes.c was. Same deck and drill kernel/lexly.h ran in ring 0: a
 * Spanish word, four English choices, pick the right one and the streak
 * grows, miss and it resets. Built with no kernel include path, linked
 * flat, loaded off the VFS by exec_user, and it reaches the machine only
 * through int 0x80: SYS_WINDOW_OPEN for a framebuffer, SYS_WINDOW_POLL for
 * input and the present, SYS_EXIT to leave.
 *
 * Glyphs: the kernel's 8x16 VGA fallback font, same as the other ring-3
 * apps. The backquote key (`) is the deliberate crash: a write through a
 * null pointer, a page fault at ring 3, reaped by the kernel.
 * tools/checks/ring3lexly-check.py presses it on purpose.
 */
#include "jtsys.h"
#include "../drivers/vgafont.h"

#define BG    0x00FAF8F6 /* GUI_BG */
#define OPT   0x00F1EDE7
#define RIGHT 0x00CFE8D2
#define MISS  0x00F0D0CC
#define INK   0x001C1C1E
#define HINT  0x0075726E

#define LX_OPT_Y0 150
#define LX_OPT_H  34

typedef struct { const char *spanish; const char *english; } LxWord;
static const LxWord LX_DECK[] = {
    {"agua", "water"},
    {"libro", "book"},
    {"casa", "house"},
    {"gato", "cat"},
    {"perro", "dog"},
    {"sol", "sun"},
    {"luna", "moon"},
    {"estrella", "star"},
    {"mesa", "table"},
    {"puerta", "door"},
    {"ventana", "window"},
    {"arbol", "tree"},
    {"flor", "flower"},
    {"pan", "bread"},
    {"queso", "cheese"},
    {"leche", "milk"},
    {"carne", "meat"},
    {"pollo", "chicken"},
    {"pescado", "fish"},
    {"vino", "wine"},
    {"cerveza", "beer"},
    {"cafe", "coffee"},
    {"te", "tea"},
    {"azucar", "sugar"},
    {"sal", "salt"},
    {"pimienta", "pepper"},
    {"mantequilla", "butter"},
    {"aceite", "oil"},
    {"vinagre", "vinegar"},
    {"manzana", "apple"},
};
#define LX_COUNT ((int)(sizeof(LX_DECK) / sizeof(LX_DECK[0])))

static struct jt_window_info win JT_DATA = {0, 0, 0, 0};
static int lx_round JT_DATA = 0, lx_streak JT_DATA = 0, lx_best JT_DATA = 0, lx_pick JT_DATA = -1;

static int lx_cur(void){ return (lx_round * 7) % LX_COUNT; } /* 7 is coprime with 30, so every word comes up once per lap */
/* Option slot -> deck index. The right answer sits in slot (round % 4);
   the other three are the next deck rows that are not the answer. */
static int lx_option(int slot){
    int right = (lx_round * 3 + lx_cur()) % 4, cur = lx_cur();
    if (slot == right) return cur;
    int n = slot < right ? slot : slot - 1;
    return (cur + 3 + n * 5) % LX_COUNT == cur ? (cur + 1) % LX_COUNT : (cur + 3 + n * 5) % LX_COUNT;
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

static void lx_draw(void) {
    rect(0, 0, (int)win.width, (int)win.height, BG);
    text("What is this Spanish word?", 20, 56, HINT);
    text(LX_DECK[lx_cur()].spanish, 20, 96, INK);
    int right = (lx_round * 3 + lx_cur()) % 4;
    for (int s = 0; s < 4; s++) {
        int y = LX_OPT_Y0 + s * LX_OPT_H;
        unsigned bg = OPT;
        if (lx_pick >= 0 && s == right) bg = RIGHT;   /* the answer, shown once you have picked */
        else if (lx_pick == s) bg = MISS;              /* your miss */
        rect(20, y, (int)win.width - 40, LX_OPT_H - 6, bg);
        char lab[4] = {(char)('1' + s), ' ', ' ', 0};
        text(lab, 30, y + 6, HINT);
        text(LX_DECK[lx_option(s)].english, 56, y + 6, INK);
    }
    char line[96]; int l = 0;
    const char *t;
    for (t = "streak "; *t; t++) line[l++] = *t;
    l += utoa10((unsigned)lx_streak, line + l);
    for (t = "   best "; *t; t++) line[l++] = *t;
    l += utoa10((unsigned)lx_best, line + l);
    for (t = lx_pick < 0 ? "   1-4 or click to answer   esc closes" : (lx_pick == right ? "   correct. any key for next" : "   incorrect. any key for next"); *t; t++) line[l++] = *t;
    line[l] = 0;
    text(line, 20, (int)win.height - 30, HINT);
}

__attribute__((section(".text.start"), used))
void _start(int argc, char **argv) {
    (void)argc; (void)argv;
    if (jt_window_open(&win) != 0 || !win.pixels) {
        jt_write(2, "lexly: no window\n", 18);
        jt_exit(1);
    }
    {   /* one line, one write: what tools/checks/ring3quotes-check.py asserts on */
        char line[48]; int l = 0;
        const char *pfx = "lexly: ring-3 window ";
        while (*pfx) line[l++] = *pfx++;
        l += utoa10(win.width, line + l); line[l++] = 'x';
        l += utoa10(win.height, line + l); line[l++] = '\n';
        jt_write(1, line, (unsigned)l);
    }

    lx_round = 0; lx_streak = 0; lx_best = 0; lx_pick = -1;
    lx_draw();

    unsigned flags = JT_POLL_PRESENT;
    for (;;) {
        struct jt_event ev;
        int r = jt_window_poll(&ev, flags);
        flags = 0;
        if (r == -11 /* -EAGAIN */) { jt_sched_yield(); continue; }
        if (r != 1) break;

        int slot = -1;
        if (ev.kind == JT_EV_CLICK) {
            if (ev.b >= LX_OPT_Y0 && ev.b < LX_OPT_Y0 + 4 * LX_OPT_H && ev.a >= 20 && ev.a < (int)win.width - 20)
                slot = (ev.b - LX_OPT_Y0) / LX_OPT_H;
            else
                break; /* the titlebar X, or anywhere off the answers, same as every other app */
        } else if (ev.kind == JT_EV_KEY) {
            if (ev.a == JT_KEY_ESC) break;
            if (ev.a == '`') {
                jt_write(1, "lexly: crashing on purpose\n", 28);
                *(volatile int *)0 = 1;
            }
            if (ev.a >= '1' && ev.a <= '4') slot = ev.a - '1';
        } else {
            flags = JT_POLL_PRESENT;
            continue;
        }

        if (lx_pick >= 0) {
            lx_pick = -1; lx_round++;
            /* One line, one write: the marker tools/checks/ring3quotes-check.py
               reads to confirm the round advanced past a picked answer. */
            char line[16]; int l = 0;
            const char *pfx = "lexly: next\n"; while (*pfx) line[l++] = *pfx++;
            jt_write(1, line, (unsigned)l);
            lx_draw(); flags = JT_POLL_PRESENT;
            continue;
        }
        if (slot < 0) { flags = JT_POLL_PRESENT; continue; }
        lx_pick = slot;
        int right = lx_round % 4; /* the in-kernel scorer, kept as it was */
        if (slot == right) { lx_streak++; if (lx_streak > lx_best) lx_best = lx_streak; }
        else lx_streak = 0;
        /* One line, one write: what tools/checks/ring3quotes-check.py reads
           to confirm the real parser/answer logic ran, not just the draw. */
        char line[32]; int l = 0;
        const char *pfx = "lexly: pick "; while (*pfx) line[l++] = *pfx++;
        line[l++] = (char)('1' + slot);
        const char *sfx = slot == right ? " right\n" : " miss\n";
        while (*sfx) line[l++] = *sfx++;
        jt_write(1, line, (unsigned)l);
        lx_draw(); flags = JT_POLL_PRESENT;
    }
    jt_write(1, "lexly: closed\n", 15);
    jt_exit(0);
}
