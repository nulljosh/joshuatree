/* bookrank: curated non-fiction book ranking, as a real ring-3 program.
 *
 * The fifth app to leave the kernel (roadmap 2.0), done exactly the way
 * user/keyrate.c, user/toroid.c, user/calculator.c and user/quotes.c
 * were. Same book list and two-pane layout kernel/bookrank.h ran in ring
 * 0: a ranked list on the left, selected row highlighted; the right pane
 * shows that book's title, author and a wrapped summary. Up/down or a
 * click on a row selects; esc closes. Built with no kernel include path,
 * linked flat, loaded off the VFS by exec_user, and it reaches the
 * machine only through int 0x80: SYS_WINDOW_OPEN for a framebuffer,
 * SYS_WINDOW_POLL for input and the present, SYS_EXIT to leave.
 *
 * Glyphs: the kernel's 8x16 VGA fallback font, same as Keyrate, Toroid,
 * Calculator and Quotes. Monospaced, so a summary wraps at a fixed
 * character count rather than a real pixel-width measure.
 *
 * The backquote key (`) is the deliberate crash, same as the other four:
 * a write through a null pointer, a page fault at ring 3, reaped by the
 * kernel. tools/checks/ring3bookrank-check.py presses it on purpose.
 */
#include "jtsys.h"
#include "../drivers/vgafont.h"

#define BG    0x00FAF8F6 /* GUI_BG */
#define ROW   0x00F1EDE7
#define SEL   0x00E2D8CC
#define INK   0x001C1C1E
#define HINT  0x0075726E

#define BR_LIST_X  20
#define BR_LIST_W  220
#define BR_INFO_X  260
#define BR_ITEM_H  28
#define BR_LIST_TOP 56

typedef struct { const char *title; const char *author; const char *summary; } BrBook;
static const BrBook BR_BOOKS[] = {
    {"Thinking, Fast and Slow", "Daniel Kahneman", "Explores how our minds make decisions through fast intuitive thinking and slow deliberate reasoning. Reveals systematic biases and heuristics that shape human judgment."},
    {"Sapiens", "Yuval Noah Harari", "Chronicles humanity's rise from hunter-gatherers to modern civilization. Examines how myths and shared beliefs shaped society."},
    {"The Selfish Gene", "Richard Dawkins", "Proposes that genes, not organisms, are the primary units of evolution and self-interest. Challenges how we understand natural selection and behavior."},
    {"Educated", "Tara Westover", "Memoir of a woman who grew up in an isolated survivalist family with no formal education. Recounts her journey to escape and eventually earn a PhD."},
    {"Atomic Habits", "James Clear", "Breaks down habit formation into tiny incremental changes that compound over time. Practical framework for building better routines and breaking bad ones."},
    {"The Lean Startup", "Eric Ries", "Introduces rapid iteration and validated learning for building businesses efficiently. Challenges traditional business planning with a startup methodology."},
    {"Freakonomics", "Steven Levitt, Stephen Dubner", "Applies economic thinking to everyday life and hidden incentives. Reveals surprising connections between seemingly unrelated phenomena."},
    {"The Art of War", "Sun Tzu", "Ancient military treatise on strategy, tactics, and the nature of conflict. Principles apply to business, negotiation, and competition."},
    {"Grit", "Angela Duckworth", "Argues that passion and perseverance matter more than raw talent for success. Research shows sustained effort and resilience predict achievement."},
    {"The Structure of Scientific Revolutions", "Thomas Kuhn", "Explains how science progresses through paradigm shifts rather than linear accumulation. Challenges the idea that science simply discovers pre-existing truth."},
};
#define BR_COUNT ((int)(sizeof(BR_BOOKS) / sizeof(BR_BOOKS[0])))

static struct jt_window_info win JT_DATA = {0, 0, 0, 0};
static int br_sel JT_DATA = 0;

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
/* Word-wrap at a fixed character count, the monospace stand-in for
   render_wrapped_text's real pixel measure. max_lines caps how much of
   the summary is shown so it never runs off the bottom of the window. */
static void wrap_text(const char *s, int x, int y, int max_chars, int max_lines, unsigned fg) {
    int line = 0;
    while (*s && line < max_lines) {
        int len = 0, last_space = -1;
        while (s[len] && len < max_chars) {
            if (s[len] == ' ') last_space = len;
            len++;
        }
        int cut = (s[len] == 0 || s[len] == ' ') ? len : (last_space >= 0 ? last_space : len);
        char buf[96]; int n = cut < 95 ? cut : 95;
        for (int i = 0; i < n; i++) buf[i] = s[i];
        buf[n] = 0;
        text(buf, x, y + line * 20, fg);
        line++;
        s += cut;
        while (*s == ' ') s++;
    }
}

static void br_draw(void) {
    rect(0, 0, (int)win.width, (int)win.height, BG);

    int items_shown = BR_COUNT;
    int max_items = ((int)win.height - BR_LIST_TOP - 40) / BR_ITEM_H;
    if (items_shown > max_items) items_shown = max_items;

    for (int i = 0; i < items_shown; i++) {
        int y = BR_LIST_TOP + i * BR_ITEM_H;
        unsigned bg = (i == br_sel) ? SEL : ROW;
        unsigned fg = (i == br_sel) ? INK : HINT;
        rect(BR_LIST_X, y, BR_LIST_W, BR_ITEM_H - 2, bg);
        char rank[5]; int r = 0, n = i + 1;
        if (n >= 10) rank[r++] = (char)('0' + n / 10);
        rank[r++] = (char)('0' + n % 10);
        rank[r++] = '.'; rank[r] = 0;
        text(rank, BR_LIST_X + 6, y + 6, fg);

        char short_title[40]; int len = 0;
        const char *title = BR_BOOKS[i].title;
        while (title[len] && len < 26) { short_title[len] = title[len]; len++; }
        if (title[len]) { short_title[len] = '.'; short_title[len+1] = '.'; short_title[len+2] = '.'; len += 3; }
        short_title[len] = 0;
        text(short_title, BR_LIST_X + 30, y + 6, fg);
    }

    if (br_sel < BR_COUNT) {
        const BrBook *b = &BR_BOOKS[br_sel];
        int info_top = BR_LIST_TOP;
        text(b->title, BR_INFO_X, info_top, INK);
        text(b->author, BR_INFO_X, info_top + 20, HINT);
        int info_w_chars = ((int)win.width - BR_INFO_X - 20) / 8;
        if (info_w_chars > 60) info_w_chars = 60;
        int max_lines = ((int)win.height - info_top - 44 - 40) / 20;
        wrap_text(b->summary, BR_INFO_X, info_top + 44, info_w_chars, max_lines, INK);
    }

    text("up/down or click to select   esc closes", 20, (int)win.height - 30, HINT);
}

__attribute__((section(".text.start"), used))
void _start(int argc, char **argv) {
    (void)argc; (void)argv;
    if (jt_window_open(&win) != 0 || !win.pixels) {
        jt_write(2, "bookrank: no window\n", 21);
        jt_exit(1);
    }
    {   /* one line, one write: what tools/checks/ring3bookrank-check.py
           asserts on */
        char line[48]; int l = 0;
        const char *pfx = "bookrank: ring-3 window ";
        while (*pfx) line[l++] = *pfx++;
        unsigned v = win.width; char tmp[12]; int tn = 0;
        do { tmp[tn++] = (char)('0' + v % 10); v /= 10; } while (v);
        while (tn) line[l++] = tmp[--tn];
        line[l++] = 'x';
        v = win.height; tn = 0;
        do { tmp[tn++] = (char)('0' + v % 10); v /= 10; } while (v);
        while (tn) line[l++] = tmp[--tn];
        line[l++] = '\n';
        jt_write(1, line, (unsigned)l);
    }

    br_sel = 0;
    br_draw();

    unsigned flags = JT_POLL_PRESENT;
    for (;;) {
        struct jt_event ev;
        int r = jt_window_poll(&ev, flags);
        if (r == 1 && jt_window_resized(&ev, &win)) { br_draw(); flags = JT_POLL_PRESENT; continue; } /* JT_EV_RESIZE: remapped, repaint at the new size */
        flags = 0;
        if (r == -11 /* -EAGAIN */) { jt_sched_yield(); continue; }
        if (r != 1) break;

        if (ev.kind == JT_EV_KEY) {
            if (ev.a == JT_KEY_ESC) break;
            if (ev.a == '`') {
                jt_write(1, "bookrank: crashing on purpose\n", 31);
                *(volatile int *)0 = 1;
            }
            if (ev.a == JT_KEY_UP && br_sel > 0) br_sel--;
            else if (ev.a == JT_KEY_DOWN && br_sel < BR_COUNT - 1) br_sel++;
            else { flags = JT_POLL_PRESENT; continue; }
        } else if (ev.kind == JT_EV_CLICK) {
            int vx = ev.a, vy = ev.b;
            if (vy >= BR_LIST_TOP && vx >= BR_LIST_X && vx < BR_LIST_X + BR_LIST_W) {
                int sel = (vy - BR_LIST_TOP) / BR_ITEM_H;
                if (sel < BR_COUNT) br_sel = sel;
                else break;
            } else break; /* titlebar X or off the app */
        } else {
            flags = JT_POLL_PRESENT;
            continue;
        }

        /* One line, one write: what tools/checks/ring3bookrank-check.py
           reads to confirm a real selection ran, not just the draw. */
        char line[40]; int l = 0;
        const char *pfx = "bookrank: sel "; while (*pfx) line[l++] = *pfx++;
        int n = br_sel + 1, dn = 0; char tmp[4];
        do { tmp[dn++] = (char)('0' + n % 10); n /= 10; } while (n);
        while (dn) line[l++] = tmp[--dn];
        line[l++] = '\n';
        jt_write(1, line, (unsigned)l);

        br_draw();
        flags = JT_POLL_PRESENT;
    }
    jt_write(1, "bookrank: closed\n", 18);
    jt_exit(0);
}
