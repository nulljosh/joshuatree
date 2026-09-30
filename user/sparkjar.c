/* sparkjar: a jar of ideas ranked by votes, as a real ring-3 program.
 *
 * The fourteenth app to leave the kernel (roadmap 2.0), done the way
 * user/portfolio.c was. Same ten ideas, same seeded votes and the same
 * screen as kernel/sparkjar.h showed in ring 0: a ranked list on the left,
 * the selected idea's pitch and three-step plan on the right. Up and down
 * (or a click) select, u upvotes the selected idea and re-sorts the list
 * with the selection following the idea, Esc closes. Votes live for the
 * run only, as before. Built with no kernel include path, linked flat,
 * loaded off the VFS by exec_user, and it reaches the machine only through
 * int 0x80: SYS_WINDOW_OPEN, SYS_WINDOW_POLL, SYS_EXIT. No new syscall.
 *
 * Glyphs: the kernel's 8x16 VGA fallback font. Every state change writes
 * one serial line, which tools/checks/ring3sparkjar-check.py reads.
 */
#include "jtsys.h"
#include "../drivers/vgafont.h"

#define BG    0x00FAF8F6 /* GUI_BG */
#define INK   0x001C1C1E
#define HINT  0x0075726E
#define ROWBG 0x00F1EDE7
#define SELBG 0x00E2D8CC

typedef struct { const char *name, *pitch, *plan; } sj_idea_t;

static const sj_idea_t SJ_IDEAS[] = {
    {"Weather Dashboard", "Display temperature, wind, precipitation for your area", "1. Fetch Open-Meteo data. 2. Parse hourly forecast. 3. Draw graph widget."},
    {"Timer App", "Simple task timer with audio alerts and session logging", "1. Build countdown UI. 2. Emit beep on complete. 3. Track history per session."},
    {"Calculator with Memory", "Basic calculator with M+ M- MR buttons", "1. Parse infix expressions. 2. Add memory stack. 3. Wire buttons to operations."},
    {"Password Generator", "Create memorable and strong passwords", "1. Pattern templates (CVC, leet). 2. Entropy slider. 3. Clipboard copy."},
    {"QR Code Scanner", "Read codes from camera or file", "1. Barcode library integration. 2. Camera capture. 3. Deep link routing."},
    {"Mini Pomodoro", "25 minute focus timer with 5 minute breaks", "1. Strict timing loop. 2. Desktop notifications. 3. Session stats."},
    {"Markdown Preview", "Live HTML rendering from markdown text", "1. Marked.js parser. 2. CSS reset template. 3. Syntax highlight code blocks."},
    {"Expense Logger", "Quick spend log with categories and tags", "1. Local storage DB. 2. Filter by date/tag. 3. CSV export."},
    {"Habit Tracker", "Daily checklist with streak count", "1. IDB for persistence. 2. Calendar view. 3. Fire streak notifications."},
    {"Dice Roller", "RPG-style dice with history and export", "1. Parse xdy notation. 2. Fair randomness. 3. Tape/export results."},
};
#define SJ_COUNT ((int)(sizeof(SJ_IDEAS) / sizeof(SJ_IDEAS[0])))
#define SJ_LIST_X 20
#define SJ_LIST_W 220
#define SJ_INFO_X 260
#define SJ_TOP    48
#define SJ_ROW_H  28

static struct jt_window_info win JT_DATA = {0, 0, 0, 0};
static int sj_votes[SJ_COUNT] JT_DATA; /* by original idea index */
static int sj_order[SJ_COUNT] JT_DATA; /* sj_order[0] is the top-voted idea */
static int sj_sel JT_DATA = 0;         /* selected position in sj_order */

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
            if (!(g[r] & (0x80 >> c)) || px < 0 || px >= (int)win.width) continue;
            win.pixels[(unsigned)py * win.width + (unsigned)px] = fg;
        }
    }
}
static int text(const char *s, int x, int y, unsigned fg) {
    for (; *s; s++, x += 8) glyph((unsigned char)*s, x, y, fg);
    return x;
}
static int utoa10(unsigned v, char *buf) {
    char tmp[12]; int tn = 0, n = 0;
    do { tmp[tn++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (tn) buf[n++] = tmp[--tn];
    buf[n] = 0;
    return n;
}

/* Bubble sort by votes, descending, keeping sj_sel on the same idea. */
static void sj_sort(void) {
    int sel_idea = sj_order[sj_sel];
    for (int i = 0; i < SJ_COUNT - 1; i++)
        for (int j = i + 1; j < SJ_COUNT; j++)
            if (sj_votes[sj_order[i]] < sj_votes[sj_order[j]]) {
                int t = sj_order[i]; sj_order[i] = sj_order[j]; sj_order[j] = t;
            }
    for (int i = 0; i < SJ_COUNT; i++) if (sj_order[i] == sel_idea) { sj_sel = i; break; }
}

/* Word-wrap the plan into the info column, one glyph row per line. */
static void sj_wrap(const char *s, int x, int y, int max_w, int max_h) {
    int cols = max_w / 8, col = 0, y0 = y;
    if (cols < 8) return;
    while (*s && y + 16 <= y0 + max_h) {
        int wl = 0;
        while (s[wl] && s[wl] != ' ') wl++;
        if (col > 0 && col + 1 + wl > cols) { col = 0; y += 18; if (y + 16 > win.height) return; }
        if (col > 0) col++;
        for (int i = 0; i < wl; i++) { glyph((unsigned char)s[i], x + (col + i) * 8, y, INK); }
        col += wl; s += wl;
        while (*s == ' ') s++;
    }
}

static void sj_draw(void) {
    int w = (int)win.width, h = (int)win.height;
    rect(0, 0, w, h, BG);
    text("up/down or click to select   u upvotes   esc closes", 20, 20, HINT);

    int room_rows = (h - SJ_TOP - 20) / SJ_ROW_H;
    int shown = SJ_COUNT < room_rows ? SJ_COUNT : room_rows;
    for (int i = 0; i < shown; i++) {
        int y = SJ_TOP + i * SJ_ROW_H, idea = sj_order[i];
        unsigned bg = (i == sj_sel) ? SELBG : ROWBG;
        unsigned fg = (i == sj_sel) ? INK : HINT;
        rect(SJ_LIST_X, y, SJ_LIST_W, SJ_ROW_H - 2, bg);
        char rank[4]; int r = 0, n = i + 1;
        if (n >= 10) rank[r++] = (char)('0' + n / 10);
        rank[r++] = (char)('0' + n % 10); rank[r++] = '.'; rank[r] = 0;
        text(rank, SJ_LIST_X + 6, y + 6, fg);
        /* name, trimmed to the room left of the vote count (8 px per glyph) */
        const char *name = SJ_IDEAS[idea].name;
        int room = (SJ_LIST_W - 70) / 8, len = 0;
        while (name[len]) len++;
        char nm[32]; int k = 0;
        if (len <= room) { while (k < len) { nm[k] = name[k]; k++; } }
        else { while (k < room - 3) { nm[k] = name[k]; k++; } while (k > 0 && nm[k - 1] == ' ') k--; nm[k++] = '.'; nm[k++] = '.'; nm[k++] = '.'; }
        nm[k] = 0;
        text(nm, SJ_LIST_X + 30, y + 6, fg);
        char vs[12]; int vl = utoa10((unsigned)sj_votes[idea], vs);
        text(vs, SJ_LIST_X + SJ_LIST_W - vl * 8 - 8, y + 6, fg);
    }

    const sj_idea_t *idea = &SJ_IDEAS[sj_order[sj_sel]];
    text(idea->name, SJ_INFO_X, SJ_TOP, INK);
    text(idea->pitch, SJ_INFO_X, SJ_TOP + 20, HINT);
    sj_wrap(idea->plan, SJ_INFO_X, SJ_TOP + 44, w - SJ_INFO_X - 24, h - SJ_TOP - 44 - 20);
}

static void say(const char *pfx, int a, int b) {
    char line[48]; int l = 0;
    while (*pfx) line[l++] = *pfx++;
    l += utoa10((unsigned)a, line + l);
    if (b >= 0) { line[l++] = ' '; l += utoa10((unsigned)b, line + l); }
    line[l++] = '\n';
    jt_write(1, line, (unsigned)l);
}

__attribute__((section(".text.start"), used))
void _start(int argc, char **argv) {
    (void)argc; (void)argv;
    if (jt_window_open(&win) != 0 || !win.pixels) {
        jt_write(2, "sparkjar: no window\n", 20);
        jt_exit(1);
    }
    {   /* one line, one write: what tools/checks/ring3sparkjar-check.py asserts on */
        char line[60]; int l = 0;
        const char *pfx = "sparkjar: ring-3 window ";
        while (*pfx) line[l++] = *pfx++;
        l += utoa10(win.width, line + l); line[l++] = 'x';
        l += utoa10(win.height, line + l); line[l++] = '\n';
        jt_write(1, line, (unsigned)l);
    }

    static const int seed[SJ_COUNT] = {24, 19, 15, 12, 11, 9, 7, 5, 3, 2};
    for (int i = 0; i < SJ_COUNT; i++) { sj_votes[i] = seed[i]; sj_order[i] = i; }
    sj_sel = 0;
    sj_draw();
    say("sparkjar: sel ", 0, -1);

    unsigned flags = JT_POLL_PRESENT;
    for (;;) {
        struct jt_event ev;
        int r = jt_window_poll(&ev, flags);
        flags = 0;
        if (r == -11 /* -EAGAIN */) { jt_sched_yield(); continue; }
        if (r != 1) break;

        int old = sj_sel;
        if (ev.kind == JT_EV_CLICK) {
            if (ev.b >= SJ_TOP && ev.a >= SJ_LIST_X && ev.a < SJ_LIST_X + SJ_LIST_W) {
                int sel = (ev.b - SJ_TOP) / SJ_ROW_H;
                if (sel >= SJ_COUNT) break;
                sj_sel = sel;
            } else break; /* the titlebar X, or anywhere off the list */
        } else if (ev.kind == JT_EV_KEY) {
            if (ev.a == JT_KEY_ESC) break;
            if (ev.a == '`') { jt_write(1, "sparkjar: crashing on purpose\n", 30); *(volatile int *)0 = 1; } /* deliberate crash, as in every ring-3 app */
            if (ev.a == JT_KEY_UP && sj_sel > 0) sj_sel--;
            else if (ev.a == JT_KEY_DOWN && sj_sel < SJ_COUNT - 1) sj_sel++;
            else if (ev.a == 'u' || ev.a == 'U') {
                int idea = sj_order[sj_sel];
                sj_votes[idea]++;
                sj_sort();
                say("sparkjar: vote ", idea, sj_votes[idea]);
                say("sparkjar: pos ", sj_sel, -1);
            }
        } else {
            flags = JT_POLL_PRESENT;
            continue;
        }
        if (sj_sel != old) say("sparkjar: sel ", sj_sel, -1);
        sj_draw(); flags = JT_POLL_PRESENT;
    }
    jt_write(1, "sparkjar: closed\n", 17);
    jt_exit(0);
}
