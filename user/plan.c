/* plan: Joshua's ten-year education and career roadmap, as a real ring-3 program.
 *
 * The eighth app to leave the kernel (roadmap 2.0), done the way
 * user/lexly.c was. Same five milestones and two-pane layout
 * kernel/plan.h ran in ring 0: milestone years on the left, up/down or a
 * click selects, the detail wraps on the right. Built with no kernel
 * include path, linked flat, loaded off the VFS by exec_user, and it
 * reaches the machine only through int 0x80: SYS_WINDOW_OPEN for a
 * framebuffer, SYS_WINDOW_POLL for input and the present, SYS_EXIT to leave.
 *
 * Glyphs: the kernel's 8x16 VGA fallback font, same as the other ring-3
 * apps. The backquote key (`) is the deliberate crash: a write through a
 * null pointer, a page fault at ring 3, reaped by the kernel.
 * tools/checks/ring3plan-check.py presses it on purpose.
 */
#include "jtsys.h"
#include "../drivers/vgafont.h"

#define BG    0x00FAF8F6 /* GUI_BG */
#define ROW   0x00F1EDE7
#define SELBG 0x00E2D8CC
#define INK   0x001C1C1E
#define HINT  0x0075726E

#define PL_LIST_X 20
#define PL_LIST_W 220
#define PL_INFO_X 260
#define PL_TOP    56
#define PL_ITEM_H 28

typedef struct { const char *when; const char *what; } PlanMilestone;
static const PlanMilestone PL_MS[] = {
    {"2026 to 27", "Pre-Calculus 12 online through LECSS. The one admission gap."},
    {"2027 to 31", "SFU Computing Science BSc, Burnaby. Calc and physics in year 1, Beedie Business minor declared end of year 2, co-op terms."},
    {"2031 to 33", "Work as a developer, ideally fintech or trading infrastructure. Decide on the MSc Finance."},
    {"2033 to 36", "Engineering: an SFU bridge or a professional master's, aiming at P.Eng eligibility."},
    {"Late 30s", "Optional finance major or MSc Finance. Done before 40."},
};
#define PL_COUNT ((int)(sizeof(PL_MS) / sizeof(PL_MS[0])))

static struct jt_window_info win JT_DATA = {0, 0, 0, 0};
static int pl_sel JT_DATA = 0;

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
    { int w = jt_text(s, x, y, fg, JT_TEXT_DRAW); if (w >= 0) return; } /* SYS_TEXT; bitmap only if its queue is full */
    for (; *s; s++, x += 8) glyph((unsigned char)*s, x, y, fg);
}
static int utoa10(unsigned v, char *buf) {
    char tmp[12]; int tn = 0, n = 0;
    do { tmp[tn++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (tn) buf[n++] = tmp[--tn];
    buf[n] = 0;
    return n;
}

/* Word wrap on the real advance (jt_wrap), breaking only at spaces. */
static void pl_wrap(const char *s, int x, int y, int w, int ymax, unsigned fg) {
    if (y + 16 <= ymax) jt_wrap(s, x, y, w, 20, (ymax - 16 - y) / 20 + 1, fg);
}

static void pl_draw(void) {
    jt_text_clear();
    rect(0, 0, (int)win.width, (int)win.height, BG);
    for (int i = 0; i < PL_COUNT; i++) {
        int y = PL_TOP + i * PL_ITEM_H;
        if (y + PL_ITEM_H > (int)win.height - 50) break;
        rect(PL_LIST_X, y, PL_LIST_W, PL_ITEM_H - 2, i == pl_sel ? SELBG : ROW);
        text(PL_MS[i].when, PL_LIST_X + 6, y + 6, i == pl_sel ? INK : HINT);
    }
    text("The next ten years", PL_INFO_X, PL_TOP, HINT);
    int wy = PL_TOP + 24;
    pl_wrap(PL_MS[pl_sel].what, PL_INFO_X, wy, (int)win.width - PL_INFO_X - 24, (int)win.height - 50, INK);
    text("up/down or click to select   esc closes", 20, (int)win.height - 30, HINT);
}

__attribute__((section(".text.start"), used))
void _start(int argc, char **argv) {
    (void)argc; (void)argv;
    if (jt_window_open(&win) != 0 || !win.pixels) {
        jt_write(2, "plan: no window\n", 17);
        jt_exit(1);
    }
    {   /* one line, one write: what tools/checks/ring3plan-check.py asserts on */
        char line[48]; int l = 0;
        const char *pfx = "plan: ring-3 window ";
        while (*pfx) line[l++] = *pfx++;
        l += utoa10(win.width, line + l); line[l++] = 'x';
        l += utoa10(win.height, line + l); line[l++] = '\n';
        jt_write(1, line, (unsigned)l);
    }

    pl_sel = 0;
    pl_draw();

    unsigned flags = JT_POLL_PRESENT;
    for (;;) {
        struct jt_event ev;
        int r = jt_window_poll(&ev, flags);
        flags = 0;
        if (r == -11 /* -EAGAIN */) { jt_sched_yield(); continue; }
        if (r != 1) break;

        int old = pl_sel;
        if (ev.kind == JT_EV_CLICK) {
            if (ev.b >= PL_TOP && ev.a >= PL_LIST_X && ev.a < PL_LIST_X + PL_LIST_W) {
                int sel = (ev.b - PL_TOP) / PL_ITEM_H;
                if (sel < PL_COUNT) pl_sel = sel;
                else break;
            } else break; /* the titlebar X, or anywhere off the list */
        } else if (ev.kind == JT_EV_KEY) {
            if (ev.a == JT_KEY_ESC) break;
            if (ev.a == '`') {
                jt_write(1, "plan: crashing on purpose\n", 27);
                *(volatile int *)0 = 1;
            }
            if (ev.a == JT_KEY_UP && pl_sel > 0) pl_sel--;
            else if (ev.a == JT_KEY_DOWN && pl_sel < PL_COUNT - 1) pl_sel++;
        } else {
            flags = JT_POLL_PRESENT;
            continue;
        }
        if (pl_sel != old) {
            /* One line, one write: the marker tools/checks/ring3plan-check.py
               reads to confirm the real selection logic ran, not just the draw. */
            char line[24]; int l = 0;
            const char *pfx = "plan: sel "; while (*pfx) line[l++] = *pfx++;
            line[l++] = (char)('0' + pl_sel); line[l++] = '\n';
            jt_write(1, line, (unsigned)l);
        }
        pl_draw(); flags = JT_POLL_PRESENT;
    }
    jt_write(1, "plan: closed\n", 14);
    jt_exit(0);
}
