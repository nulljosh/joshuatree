/* homeqi: eight yes/no feng shui questions about your home, one at a
 * time, condensed from Eva Wong's "Good Fengshui" -- as a real ring-3
 * program.
 *
 * The fourth app to leave the kernel (roadmap 2.0), done the way
 * user/keyrate.c, user/toroid.c and user/calculator.c were. Same eight
 * questions and scoring as kernel/homeqi.h's hq_draw/gui_launch_homeqi
 * (which had gone dead: APPS[] actually opened Homeqi through the
 * generic gui_launch_html static-page viewer, a duplicate wiring bug
 * noted in that file's own comment), now the one real Homeqi and the
 * only copy. This file is compiled with no kernel include path, linked
 * flat, loaded off the VFS by exec_user, and reaches the machine only
 * through int 0x80: SYS_WINDOW_OPEN for a framebuffer, SYS_WINDOW_POLL
 * for input and the present, SYS_EXIT to leave.
 *
 * Glyphs: the kernel's 8x16 VGA fallback font, same as Keyrate, Toroid
 * and Calculator. Word-wrap is a small local helper since render_wrapped_
 * text is a kernel.c static this binary cannot reach. The backquote key
 * (`) is the deliberate crash, same as the other three: a write through a
 * null pointer, a page fault at ring 3, reaped by the kernel.
 * tools/checks/ring3homeqi-check.py presses it on purpose.
 */
#include "jtsys.h"
#include "../drivers/vgafont.h"

#define BG    0x00FAF8F6 /* GUI_BG */
#define INK   0x001C1C1E
#define HINT  0x0075726E

typedef struct { const char *question; const char *reasoning; int yes_is_good; } HqQuestion;

static const HqQuestion HQ_QUESTIONS[] = {
    {"Does your front door face a busy road head-on?",
     "A direct road creates rushing energy. Ideally, the entrance protects you.", 0},
    {"Is there natural light in your main living room?",
     "Light brings clarity, warmth, and good chi. Rooms without it feel heavy.", 1},
    {"Is your bed against a solid wall?",
     "A solid wall behind supports rest. Open sides make sleep restless.", 1},
    {"Is there clutter at your entry or main pathway?",
     "Blocked pathways block opportunities. Clear spaces welcome good energy.", 0},
    {"Can you see water (stream, pond, or fountain) from your home?",
     "Water attracts wealth and calm. Still water near home is auspicious.", 1},
    {"Are there high-voltage power lines or towers nearby?",
     "Electric fields disrupt chi. Distance and barriers help protect health.", 0},
    {"Does your neighborhood feel safe and well-maintained?",
     "Neglected areas have stagnant chi. Care in public spaces lifts the whole block.", 1},
    {"Can you open windows for fresh air and views?",
     "Open windows bring life and connection. Blocked views trap you in a box.", 1},
};
#define HQ_COUNT ((int)(sizeof(HQ_QUESTIONS) / sizeof(HQ_QUESTIONS[0])))

static struct jt_window_info win JT_DATA = {0, 0, 0, 0};
static int hq_q JT_DATA = 0;
static int hq_score JT_DATA = 0;
static int hq_show_reasoning JT_DATA = 0;

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

/* Word-wraps s into 8px-wide glyphs across max_w px, top-left at (x, y),
 * one 16px line per row, same shape as kernel.c's render_wrapped_text but
 * self-contained: this binary has no kernel statics to call into. */
static void wrap_text(const char *s, int x, int y, int max_w, unsigned fg) {
    int cols = max_w / 8;
    if (cols < 1) cols = 1;
    int cx = 0, cy = y;
    while (*s) {
        int wlen = 0;
        while (s[wlen] && s[wlen] != ' ') wlen++;
        if (cx > 0 && cx + wlen > cols) { cx = 0; cy += 20; }
        for (int i = 0; i < wlen; i++) { glyph((unsigned char)s[i], x + cx * 8, cy, fg); cx++; }
        s += wlen;
        if (*s == ' ') { s++; if (cx + 1 <= cols) cx++; else { cx = 0; cy += 20; } }
    }
}

static void hq_draw(void) {
    rect(0, 0, (int)win.width, (int)win.height, BG);

    int ctr_y = (int)win.height / 2 - 60;
    int ctr_x = 40;
    int max_w = (int)win.width - 80;

    if (hq_q < HQ_COUNT) {
        if (!hq_show_reasoning) {
            char q_num[16]; int qn = hq_q + 1, qlen = 0;
            if (qn >= 10) q_num[qlen++] = (char)('0' + qn / 10);
            q_num[qlen++] = (char)('0' + qn % 10);
            q_num[qlen++] = '.'; q_num[qlen] = 0;
            text(q_num, ctr_x, ctr_y, HINT);

            wrap_text(HQ_QUESTIONS[hq_q].question, ctr_x + 24, ctr_y, max_w - 24, INK);

            char score_txt[32]; int slen = 0;
            slen += utoa10((unsigned)hq_score, score_txt);
            score_txt[slen++] = '/'; score_txt[slen++] = '8'; score_txt[slen] = 0;
            text(score_txt, ctr_x, ctr_y + 100, HINT);

            text("1 yes   2 no", ctr_x, ctr_y + 130, INK);
        } else {
            wrap_text(HQ_QUESTIONS[hq_q].reasoning, ctr_x, ctr_y, max_w, INK);
            text("any key for next", ctr_x, ctr_y + 140, HINT);
        }
    } else {
        const char *msg;
        if (hq_score <= 3) msg = "Your home has work to do.";
        else if (hq_score <= 6) msg = "Your home is pleasant and balanced.";
        else msg = "Your home is excellent. Well done.";

        text("Your home assessment:", ctr_x, ctr_y, INK);
        char score_out[32]; int slen = 0;
        slen += utoa10((unsigned)hq_score, score_out);
        score_out[slen++] = '/'; score_out[slen++] = '8'; score_out[slen] = 0;
        text(score_out, ctr_x, ctr_y + 30, INK);
        text(msg, ctr_x, ctr_y + 70, HINT);
        text("any key to restart", ctr_x, ctr_y + 140, HINT);
    }
}

__attribute__((section(".text.start"), used))
void _start(int argc, char **argv) {
    (void)argc; (void)argv;
    if (jt_window_open(&win) != 0 || !win.pixels) {
        jt_write(2, "homeqi: no window\n", 19);
        jt_exit(1);
    }
    {   /* one line, one write: what tools/checks/ring3homeqi-check.py asserts on */
        char line[48]; int l = 0;
        const char *pfx = "homeqi: ring-3 window ";
        while (*pfx) line[l++] = *pfx++;
        l += utoa10(win.width, line + l); line[l++] = 'x';
        l += utoa10(win.height, line + l); line[l++] = '\n';
        jt_write(1, line, (unsigned)l);
    }

    hq_q = 0; hq_score = 0; hq_show_reasoning = 0;
    hq_draw();

    unsigned flags = JT_POLL_PRESENT;
    for (;;) {
        struct jt_event ev;
        int r = jt_window_poll(&ev, flags);
        flags = 0;
        if (r == -11 /* -EAGAIN */) { jt_sched_yield(); continue; }
        if (r != 1) break;
        if (ev.kind != JT_EV_KEY) { flags = JT_POLL_PRESENT; continue; }

        if (ev.a == JT_KEY_ESC) break;
        if (ev.a == '`') {
            jt_write(1, "homeqi: crashing on purpose\n", 29);
            *(volatile int *)0 = 1;
        }

        if (hq_q < HQ_COUNT) {
            if (!hq_show_reasoning) {
                if (ev.a == '1' || ev.a == '2') {
                    if ((ev.a == '1') == HQ_QUESTIONS[hq_q].yes_is_good) hq_score++;
                    hq_show_reasoning = 1;
                    /* One line, one write: the discriminating marker
                       tools/checks/ring3homeqi-check.py reads to confirm
                       the ring-3 quiz actually scored the answer. */
                    char line[48]; int l = 0;
                    const char *pfx = "homeqi: answered q"; while (*pfx) line[l++] = *pfx++;
                    l += utoa10((unsigned)(hq_q + 1), line + l);
                    const char *sep = " score="; while (*sep) line[l++] = *sep++;
                    l += utoa10((unsigned)hq_score, line + l);
                    line[l++] = '\n';
                    jt_write(1, line, (unsigned)l);
                } else {
                    flags = JT_POLL_PRESENT;
                    continue;
                }
            } else {
                hq_show_reasoning = 0;
                hq_q++;
                if (hq_q == HQ_COUNT) {
                    char line[48]; int l = 0;
                    const char *pfx = "homeqi: done score="; while (*pfx) line[l++] = *pfx++;
                    l += utoa10((unsigned)hq_score, line + l);
                    line[l++] = '\n';
                    jt_write(1, line, (unsigned)l);
                }
            }
        } else {
            hq_q = 0; hq_score = 0; hq_show_reasoning = 0;
        }
        hq_draw();
        flags = JT_POLL_PRESENT;
    }
    jt_write(1, "homeqi: closed\n", 16);
    jt_exit(0);
}
