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
#include "libjt/text.h"

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
#define SJ_TOP    40
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
static int text(const char *s, int x, int y, unsigned fg) { /* returns the x after the last glyph */
    return jt_text_draw(&win, JT_FACE_BODY, x, y, fg, s);
}
static void etext(int face, const char *s, int x, int y, int maxw, unsigned fg) {
    char t[72]; int n = 0;
    while (s[n] && n < 64) { t[n] = s[n]; n++; }
    t[n] = 0;
    if (jt_text_width(face, t) > maxw) {
        while (n > 0) {
            t[n] = '.'; t[n + 1] = '.'; t[n + 2] = '.'; t[n + 3] = 0;
            if (jt_text_width(face, t) <= maxw) break;
            t[--n] = 0;
        }
        if (n == 0) t[0] = 0;
    }
    jt_text_draw(&win, face, x, y, fg, t);
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

/* Word-wrap the plan into the info column by pixel width, 20 px a line. */
static void sj_wrap(const char *s, int x, int y, int max_w, int max_h) {
    char line[120]; int ll = 0, y0 = y;
    if (max_w < 60) return;
    while (*s && y + 18 <= y0 + max_h) {
        int we = 0;
        while (s[we] && s[we] != ' ') we++;
        char trial[120]; int tl = 0;
        for (int i = 0; i < ll; i++) trial[tl++] = line[i];
        if (ll) trial[tl++] = ' ';
        for (int i = 0; i < we && tl < 118; i++) trial[tl++] = s[i];
        trial[tl] = 0;
        if (ll && jt_text_width(JT_FACE_BODY, trial) > max_w) {
            line[ll] = 0; text(line, x, y, INK); y += 20; ll = 0;
            continue;
        }
        for (int i = 0; i <= tl; i++) line[i] = trial[i];
        ll = tl; s += we;
        while (*s == ' ') s++;
    }
    if (ll && y + 18 <= y0 + max_h) { line[ll] = 0; text(line, x, y, INK); }
}

static void sj_draw(void) {
    int w = (int)win.width, h = (int)win.height;
    rect(0, 0, w, h, BG);

    int room_rows = (h - SJ_TOP - 44) / SJ_ROW_H;
    int shown = SJ_COUNT < room_rows ? SJ_COUNT : room_rows;
    for (int i = 0; i < shown; i++) {
        int y = SJ_TOP + i * SJ_ROW_H, idea = sj_order[i];
        unsigned bg = (i == sj_sel) ? SELBG : ROWBG;
        unsigned fg = (i == sj_sel) ? INK : HINT;
        rect(SJ_LIST_X, y, SJ_LIST_W, SJ_ROW_H - 2, bg);
        char rank[4]; int r = 0, n = i + 1;
        if (n >= 10) rank[r++] = (char)('0' + n / 10);
        rank[r++] = (char)('0' + n % 10); rank[r++] = '.'; rank[r] = 0;
        text(rank, SJ_LIST_X + 6, y + 5, fg);
        char vs[12]; int vl = utoa10((unsigned)sj_votes[idea], vs);
        int vw = jt_text_width(JT_FACE_BODY, vs); (void)vl;
        etext(JT_FACE_BODY, SJ_IDEAS[idea].name, SJ_LIST_X + 30, y + 5, SJ_LIST_W - 30 - vw - 20, fg);
        text(vs, SJ_LIST_X + SJ_LIST_W - vw - 8, y + 5, fg);
    }

    const sj_idea_t *idea = &SJ_IDEAS[sj_order[sj_sel]];
    etext(JT_FACE_BOLD, idea->name, SJ_INFO_X, SJ_TOP, w - SJ_INFO_X - 24, INK);
    etext(JT_FACE_BODY, idea->pitch, SJ_INFO_X, SJ_TOP + 24, w - SJ_INFO_X - 24, HINT);
    sj_wrap(idea->plan, SJ_INFO_X, SJ_TOP + 52, w - SJ_INFO_X - 24, h - SJ_TOP - 52 - 44);
    etext(JT_FACE_BODY, "up/down or click to select   u upvotes   esc closes", 20, h - 30, w - 40, HINT);
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
        if (r == 1 && jt_window_resized(&ev, &win)) { sj_draw(); flags = JT_POLL_PRESENT; continue; } /* JT_EV_RESIZE: remapped, repaint at the new size */
        flags = 0;
        if (r == -11 /* -EAGAIN */) { jt_sched_yield(); continue; }
        if (r != 1) break;

        int old = sj_sel;
        if (ev.kind == JT_EV_CLICK) {
            if (ev.a < 0 || ev.b < 0 || ev.a >= (int)win.width || ev.b >= (int)win.height) break; /* chrome X or dock */
            if (ev.b >= SJ_TOP && ev.a >= SJ_LIST_X && ev.a < SJ_LIST_X + SJ_LIST_W) {
                int sel = (ev.b - SJ_TOP) / SJ_ROW_H;
                if (sel < SJ_COUNT) sj_sel = sel;
            }
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
