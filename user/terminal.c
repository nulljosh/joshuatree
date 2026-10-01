/* terminal: the desktop Terminal as a real ring-3 program and compositor window.
 *
 * Same look as the old in-kernel terminal: warm near-black page, scrollback
 * in sand, a "> " prompt pinned to the bottom in amber, a block cursor and a
 * hint line. Text is libjt's antialiased DejaVu Sans (libjt has no mono face,
 * so this uses the body face, drawn one glyph per fixed 8 px cell so columns
 * still line up like a terminal).
 *
 * Commands go to the kernel through the one syscall SYS_SHELL_RUN, which
 * answers from a short allowlist (help echo uptime mem ps ls cat, see
 * kernel/shellsys.c); anything else comes back as a one-line refusal. "clear"
 * is local: it empties the scrollback.
 *
 * Keys: typing, Backspace, Enter runs the line, copy/cut take the whole input
 * line into an in-app clipboard and paste inserts it at the end (the same
 * one-line contract as before), Esc closes, backquote is the deliberate
 * crash. Clicks never close. Serial markers: terminal: ring-3 window,
 * terminal: ran=N (bytes of output).
 */
#include "jtsys.h"
#include "libjt/text.h"

#define BG     0x001A1512
#define INK    0x00D8CFC4
#define TYPED  0x00F2E9D8
#define AMBER  0x00C98A3E
#define HINT   0x00807468
#define CELL   8
#define LH     17
#define MARGIN 16
#define LINE_MAX 95
#define SCROLL 8192
#define OUT_MAX 2048
#define KEY_COPY 302
#define KEY_CUT 303
#define KEY_PASTE 304

static struct jt_window_info win JT_DATA = {0, 0, 0, 0};
extern char _user_end[];
struct arena { char sb[SCROLL + 1]; char out[OUT_MAX]; char in[LINE_MAX + 1]; char clip[LINE_MAX + 1]; unsigned starts[80]; };
static struct arena *ar JT_DATA = 0;
static unsigned slen JT_DATA = 0, inlen JT_DATA = 0, cliplen JT_DATA = 0;

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
/* One glyph per fixed cell, so a column is a column. */
static void cell(int x, int y, char c, unsigned fg) {
    if (c == ' ') return;
    char g[2] = {c, 0};
    int gw = jt_text_width(JT_FACE_BODY, g);
    jt_text_draw(&win, JT_FACE_BODY, x + (CELL - gw) / 2, y, fg, g);
}
static void sputc(char c) {
    if (slen + 1 >= SCROLL) {
        /* Drop the oldest half in one move rather than a byte per character. */
        unsigned keep = SCROLL / 2;
        for (unsigned i = 0; i < keep; i++) ar->sb[i] = ar->sb[slen - keep + i];
        slen = keep;
    }
    ar->sb[slen++] = c;
    ar->sb[slen] = 0;
}
static void sputs(const char *s) { while (*s) sputc(*s++); }
static int seq(const char *a, const char *b) { while (*a && *a == *b) { a++; b++; } return *a == *b; }

static void draw(void) {
    int cols = ((int)win.width - 2 * MARGIN) / CELL;
    if (cols < 8) cols = 8;
    int prompt_y = (int)win.height - 52;
    int hint_y = (int)win.height - 26;
    int rows = (prompt_y - 12 - MARGIN) / LH;
    if (rows < 1) rows = 1;
    if (rows > 79) rows = 79;
    rect(0, 0, (int)win.width, (int)win.height, BG);

    /* One pass: where every wrapped line starts; keep the last `rows` of them. */
    unsigned total = 0, col = 0, ls = 0;
    for (unsigned i = 0; i <= slen; i++) {
        int wrapped = col == (unsigned)cols;
        int nl = i < slen && ar->sb[i] == '\n';
        if (wrapped || nl || i == slen) {
            ar->starts[total % 80] = ls;
            total++;
            ls = nl ? i + 1 : i;
            col = 0;
            if (nl) continue;
            if (i == slen) break;
        }
        col++;
    }
    unsigned first = total > (unsigned)rows ? total - (unsigned)rows : 0;
    int y = MARGIN;
    for (unsigned ln = first; ln < total && y < prompt_y - 8; ln++) {
        unsigned p = ar->starts[ln % 80];
        int x = MARGIN;
        for (int c = 0; c < cols && p < slen; c++, p++) {
            if (ar->sb[p] == '\n') break;
            cell(x, y, ar->sb[p], INK);
            x += CELL;
        }
        y += LH;
    }

    /* Prompt, pinned to the bottom so typing never scrolls out of view. */
    cell(MARGIN, prompt_y, '>', AMBER);
    int x = MARGIN + 2 * CELL;
    for (unsigned i = 0; i < inlen && x < (int)win.width - MARGIN - CELL; i++, x += CELL) cell(x, prompt_y, ar->in[i], TYPED);
    rect(x, prompt_y + 1, CELL, LH - 3, AMBER); /* block cursor */
    jt_text_draw(&win, JT_FACE_BODY, MARGIN, hint_y, HINT, "esc closes   |   allowlisted shell: help echo uptime mem ps ls cat");
}

static void run_line(void) {
    ar->in[inlen] = 0;
    sputs("> "); sputs(ar->in); sputc('\n');
    if (inlen) {
        if (seq(ar->in, "clear")) { slen = 0; ar->sb[0] = 0; }
        else {
            int n = jt_shell_run(ar->in, ar->out, OUT_MAX);
            if (n < 0) sputs("shell: refused\n");
            else for (int i = 0; i < n; i++) sputc(ar->out[i]);
            char m[32] = "terminal: ran=";
            int l = 14, d[8], dn = 0;
            unsigned v = n < 0 ? 0 : (unsigned)n;
            if (!v) d[dn++] = 0;
            while (v && dn < 8) { d[dn++] = (int)(v % 10); v /= 10; }
            while (dn) m[l++] = (char)('0' + d[--dn]);
            m[l++] = '\n';
            jt_write(1, m, (unsigned)l);
        }
    }
    inlen = 0;
}

__attribute__((section(".text.start"), used))
void _start(int argc, char **argv) {
    (void)argc; (void)argv;
    if (jt_window_open(&win) != 0 || !win.pixels) { jt_write(2, "terminal: no window\n", 20); jt_exit(1); }
    ar = (struct arena *)(((unsigned)_user_end + 15u) & ~15u);
    sputs("Joshua Tree terminal. Type help.\n");
    draw();
    jt_write(1, "terminal: ring-3 window\n", 24);
    unsigned flags = JT_POLL_PRESENT;
    for (;;) {
        struct jt_event ev;
        int r = jt_window_poll(&ev, flags);
        flags = 0;
        if (r == -11) { jt_sched_yield(); continue; }
        if (r != 1) break;
        flags = JT_POLL_PRESENT;
        if (ev.kind != JT_EV_KEY) continue; /* clicks never close; nothing to hit either */
        int k = ev.a;
        if (k == '`') { jt_write(1, "terminal: crashing on purpose\n", 30); *(volatile int *)0 = 1; }
        if (k == JT_KEY_ESC) break;
        if (k == JT_KEY_ENTER) run_line();
        else if (k == 8) { if (inlen) inlen--; }
        else if (k == KEY_COPY || k == KEY_CUT) {
            for (unsigned i = 0; i < inlen; i++) ar->clip[i] = ar->in[i];
            cliplen = inlen;
            if (k == KEY_CUT) inlen = 0;
        }
        else if (k == KEY_PASTE) {
            for (unsigned i = 0; i < cliplen && inlen < LINE_MAX; i++) {
                char pc = ar->clip[i];
                if (pc >= 32 && pc < 127) ar->in[inlen++] = pc;
            }
        }
        else if (k >= 32 && k < 127 && inlen < LINE_MAX) ar->in[inlen++] = (char)k;
        else continue;
        draw();
    }
    jt_write(1, "terminal: closed\n", 17);
    jt_exit(0);
}
