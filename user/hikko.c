/* hikko: the idea forum, ranked by votes, as a real ring-3 program.
 *
 * Hikko was Sparkjar, then Hotaru. It was the fourteenth app to leave the
 * kernel (roadmap 2.0), done the way user/portfolio.c was. The screen is a
 * ranked list on the left and the selected idea's text and plan on the right.
 * Up and down (or a click) select, u upvotes the selected idea and re-sorts the
 * list with the selection following the idea, Esc closes.
 *
 * The ideas are the real forum's. At start the program asks the Worker for
 * /api/hikko through SYS_HTTP_GET and shows the top twelve (title, votes, the
 * idea's text, its plan). The reply is text: a first line with the row count
 * (digits only), then votes|category|title|text|plan. It is parsed in place and
 * trusted for nothing: bytes are forced printable, rows past twelve are
 * ignored, a short row or one without a title is dropped. With no network, an
 * HTTP error, junk, an oversize body or no usable row, the ten demo ideas
 * stay. Voting is local either way: a vote lives for the run and is never
 * sent anywhere, because the real forum needs an account to vote.
 *
 * Built with no kernel include path, linked flat, loaded off the VFS by
 * exec_user, and it reaches the machine only through int 0x80:
 * SYS_WINDOW_OPEN, SYS_WINDOW_POLL, SYS_HTTP_GET, SYS_EXIT. No new syscall.
 *
 * Glyphs: the antialiased libjt face. Every state change writes one serial
 * line, which tools/checks/ring3hikko-check.py (the demo ideas) and
 * tools/checks/ring3hikko-live-check.py (the live and fallback paths) read.
 */
#include "jtsys.h"
#include "libjt/text.h"

#define BG    0x00FAF8F6 /* GUI_BG */
#define INK   0x001C1C1E
#define HINT  0x0075726E
#define ROWBG 0x00F1EDE7
#define SELBG 0x00E2D8CC

typedef struct { const char *name, *pitch, *plan, *cat; } sj_idea_t; /* cat is empty for a demo idea */

static const sj_idea_t SJ_IDEAS[] = {
    {"Weather Dashboard", "Display temperature, wind, precipitation for your area", "1. Fetch Open-Meteo data. 2. Parse hourly forecast. 3. Draw graph widget.", ""},
    {"Timer App", "Simple task timer with audio alerts and session logging", "1. Build countdown UI. 2. Emit beep on complete. 3. Track history per session.", ""},
    {"Calculator with Memory", "Basic calculator with M+ M- MR buttons", "1. Parse infix expressions. 2. Add memory stack. 3. Wire buttons to operations.", ""},
    {"Password Generator", "Create memorable and strong passwords", "1. Pattern templates (CVC, leet). 2. Entropy slider. 3. Clipboard copy.", ""},
    {"QR Code Scanner", "Read codes from camera or file", "1. Barcode library integration. 2. Camera capture. 3. Deep link routing.", ""},
    {"Mini Pomodoro", "25 minute focus timer with 5 minute breaks", "1. Strict timing loop. 2. Desktop notifications. 3. Session stats.", ""},
    {"Markdown Preview", "Live HTML rendering from markdown text", "1. Marked.js parser. 2. CSS reset template. 3. Syntax highlight code blocks.", ""},
    {"Expense Logger", "Quick spend log with categories and tags", "1. Local storage DB. 2. Filter by date/tag. 3. CSV export.", ""},
    {"Habit Tracker", "Daily checklist with streak count", "1. IDB for persistence. 2. Calendar view. 3. Fire streak notifications.", ""},
    {"Dice Roller", "RPG-style dice with history and export", "1. Parse xdy notation. 2. Fair randomness. 3. Tape/export results.", ""},
};
#define SJ_COUNT ((int)(sizeof(SJ_IDEAS) / sizeof(SJ_IDEAS[0])))
#define SJ_LIVE_MAX 12
#define SJ_PATH "/api/hikko"
#define SJ_LIST_X 20
#define SJ_LIST_W 220
#define SJ_INFO_X 260
#define SJ_TOP    40
#define SJ_ROW_H  28

static struct jt_window_info win JT_DATA = {0, 0, 0, 0};
static char sj_body[6144] JT_DATA;                 /* the reply, parsed in place */
static sj_idea_t sj_live[SJ_LIVE_MAX] JT_DATA;
static const sj_idea_t *sj_rows JT_DATA = SJ_IDEAS;
static int sj_n JT_DATA = SJ_COUNT;
static int sj_is_live JT_DATA = 0;                 /* 1 when the rows came from the forum */
static int sj_votes[SJ_LIVE_MAX] JT_DATA;          /* by original idea index; local, never sent */
static int sj_order[SJ_LIVE_MAX] JT_DATA;          /* sj_order[0] is the top-voted idea */
static int sj_sel JT_DATA = 0;                     /* selected position in sj_order */
static int sj_top JT_DATA = 0;                     /* first row shown in the list */

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
    if (s[n] || jt_text_width(face, t) > maxw) {
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

/* Stable insertion sort by votes, descending: equal votes keep the forum's
   order. The selection follows its idea. */
static void sj_sort(void) {
    int sel_idea = sj_order[sj_sel];
    for (int i = 1; i < sj_n; i++) {
        int key = sj_order[i], j = i;
        while (j > 0 && sj_votes[sj_order[j - 1]] < sj_votes[key]) { sj_order[j] = sj_order[j - 1]; j--; }
        sj_order[j] = key;
    }
    for (int i = 0; i < sj_n; i++) if (sj_order[i] == sel_idea) { sj_sel = i; break; }
}

static int sj_visible(void) {
    int room = ((int)win.height - SJ_TOP - 44) / SJ_ROW_H;
    if (room < 1) room = 1;
    return sj_n < room ? sj_n : room;
}
static void sj_scroll(void) { /* keep the selection inside the visible rows */
    int vis = sj_visible();
    if (sj_sel < sj_top) sj_top = sj_sel;
    if (sj_sel >= sj_top + vis) sj_top = sj_sel - vis + 1;
    if (sj_top < 0) sj_top = 0;
}

/* Word-wrap into the info column by pixel width, 20 px a line. Returns the y
   below the last line, so a second paragraph can follow. */
static int sj_wrap(const char *s, int x, int y, int max_w, int max_h, unsigned fg) {
    char line[120]; int ll = 0, y0 = y;
    if (max_w < 60) return y;
    while (*s && y + 18 <= y0 + max_h) {
        int we = 0;
        while (s[we] && s[we] != ' ') we++;
        char trial[120]; int tl = 0;
        for (int i = 0; i < ll; i++) trial[tl++] = line[i];
        if (ll) trial[tl++] = ' ';
        for (int i = 0; i < we && tl < 118; i++) trial[tl++] = s[i];
        trial[tl] = 0;
        if (ll && jt_text_width(JT_FACE_BODY, trial) > max_w) {
            line[ll] = 0; text(line, x, y, fg); y += 20; ll = 0;
            continue;
        }
        for (int i = 0; i <= tl; i++) line[i] = trial[i];
        ll = tl; s += we;
        while (*s == ' ') s++;
    }
    if (ll && y + 18 <= y0 + max_h) { line[ll] = 0; text(line, x, y, fg); y += 20; }
    return y;
}

static void sj_draw(void) {
    int w = (int)win.width, h = (int)win.height;
    rect(0, 0, w, h, BG);
    sj_scroll();

    if (sj_is_live) {
        char hdr[48]; int l = 0; const char *t = "Top ";
        while (*t) hdr[l++] = *t++;
        l += utoa10((unsigned)sj_n, hdr + l);
        t = " ideas from the forum"; while (*t) hdr[l++] = *t++;
        hdr[l] = 0;
        text(hdr, SJ_LIST_X, 12, HINT);
    } else {
        text("Offline, demo ideas", SJ_LIST_X, 12, HINT);
    }

    int shown = sj_visible();
    for (int k = 0; k < shown; k++) {
        int i = sj_top + k, y = SJ_TOP + k * SJ_ROW_H, idea = sj_order[i];
        unsigned bg = (i == sj_sel) ? SELBG : ROWBG;
        unsigned fg = (i == sj_sel) ? INK : HINT;
        rect(SJ_LIST_X, y, SJ_LIST_W, SJ_ROW_H - 2, bg);
        char rank[4]; int r = 0, n = i + 1;
        if (n >= 10) rank[r++] = (char)('0' + n / 10);
        rank[r++] = (char)('0' + n % 10); rank[r++] = '.'; rank[r] = 0;
        text(rank, SJ_LIST_X + 6, y + 5, fg);
        char vs[12]; utoa10((unsigned)sj_votes[idea], vs);
        int vw = jt_text_width(JT_FACE_BODY, vs);
        etext(JT_FACE_BODY, sj_rows[idea].name, SJ_LIST_X + 30, y + 5, SJ_LIST_W - 30 - vw - 20, fg);
        text(vs, SJ_LIST_X + SJ_LIST_W - vw - 8, y + 5, fg);
    }

    const sj_idea_t *idea = &sj_rows[sj_order[sj_sel]];
    int info_w = w - SJ_INFO_X - 24, room = h - SJ_TOP - 52 - 44;
    etext(JT_FACE_BOLD, idea->name, SJ_INFO_X, SJ_TOP, info_w, INK);
    if (!sj_is_live) { /* demo idea: one pitch line, then the plan */
        etext(JT_FACE_BODY, idea->pitch, SJ_INFO_X, SJ_TOP + 24, info_w, HINT);
        sj_wrap(idea->plan, SJ_INFO_X, SJ_TOP + 52, info_w, room, INK);
    } else {           /* forum idea: category, its text, then its plan */
        etext(JT_FACE_BODY, idea->cat, SJ_INFO_X, SJ_TOP + 24, info_w, HINT);
        int y = SJ_TOP + 52;
        y = sj_wrap(idea->pitch, SJ_INFO_X, y, info_w, room, INK);
        if (idea->plan[0] && y + 10 - (SJ_TOP + 52) < room)
            sj_wrap(idea->plan, SJ_INFO_X, y + 10, info_w, room - (y + 10 - (SJ_TOP + 52)), HINT);
    }
    etext(JT_FACE_BODY, "up/down or click to select   u upvotes   esc closes", 20, h - 30, w - 40, HINT);
}

static void say(const char *pfx, int a, int b) {
    char line[48]; int l = 0;
    while (*pfx) line[l++] = *pfx++;
    if (a < 0) { line[l++] = '-'; a = -a; } /* a fetch error is negative */
    l += utoa10((unsigned)a, line + l);
    if (b >= 0) { line[l++] = ' '; l += utoa10((unsigned)b, line + l); }
    line[l++] = '\n';
    jt_write(1, line, (unsigned)l);
}
/* "hikko: shown <name>": the idea the right pane is drawing, for the live check. */
static void say_shown(void) {
    char line[96]; int l = 0;
    const char *pfx = "hikko: shown ", *t = sj_rows[sj_order[sj_sel]].name;
    while (*pfx) line[l++] = *pfx++;
    for (; *t && l < 90; t++) line[l++] = (*t >= 0x20 && *t <= 0x7e) ? *t : '?';
    line[l++] = '\n';
    jt_write(1, line, (unsigned)l);
}

static int digits(const char *s, int cap) { /* leading decimal digits, at most 5, clamped to cap */
    int v = 0, k = 0;
    while (*s >= '0' && *s <= '9' && k < 5) { v = v * 10 + (*s++ - '0'); k++; }
    return v > cap ? cap : v;
}

/* Parse the Worker's text in place. Line 1 is the row count (digits only, or
   the reply is not ours); then votes|category|title|text|plan. Returns the rows
   kept; on 0 the demo ideas stay. Bytes are forced printable, the scan stops at
   the NUL, rows past the cap are ignored. */
static int sj_parse(char *body, int n) {
    body[n] = 0;
    for (int i = 0; i < n; i++) {
        unsigned char c = (unsigned char)body[i];
        if (c == '\r') body[i] = ' ';
        else if (c != '\n' && (c < 0x20 || c > 0x7e)) body[i] = '?';
    }
    char *p = body;
    if (*p < '0' || *p > '9') return 0;
    while (*p && *p != '\n') { if (*p < '0' || *p > '9') return 0; p++; }
    if (*p) p++;
    int live = 0;
    while (*p && live < SJ_LIVE_MAX) {
        char *line = p, *f[5]; int k = 1;
        f[0] = line;
        while (*p && *p != '\n') {
            if (*p == '|' && k < 5) { *p = 0; f[k++] = p + 1; }
            p++;
        }
        if (*p) *p++ = 0;
        if (k < 5 || !f[2][0]) continue;
        sj_votes[live] = digits(f[0], 99999);
        sj_live[live].cat = f[1]; sj_live[live].name = f[2];
        sj_live[live].pitch = f[3]; sj_live[live].plan = f[4];
        live++;
    }
    return live;
}

/* One GET through the kernel; on anything but a 200 with a body that fits and
   parses, the demo ideas stay. */
static void sj_fetch(void) {
    static const int seed[SJ_COUNT] = {24, 19, 15, 12, 11, 9, 7, 5, 3, 2};
    for (int i = 0; i < SJ_COUNT; i++) sj_votes[i] = seed[i];
    sj_rows = SJ_IDEAS; sj_n = SJ_COUNT; sj_is_live = 0;
    int n = jt_http_get(SJ_PATH, sj_body, sizeof(sj_body) - 1);
    say("hikko: fetch ", n, -1);
    if (n <= 0 || n >= (int)sizeof(sj_body) - 1) return;
    int live = sj_parse(sj_body, n);
    if (live) { sj_rows = sj_live; sj_n = live; sj_is_live = 1; }
    else for (int i = 0; i < SJ_COUNT; i++) sj_votes[i] = seed[i]; /* a half-parsed reply must not leak its votes */
}

__attribute__((section(".text.start"), used))
void _start(int argc, char **argv) {
    (void)argc; (void)argv;
    if (jt_window_open(&win) != 0 || !win.pixels) {
        jt_write(2, "hikko: no window\n", 17);
        jt_exit(1);
    }
    {   /* one line, one write: what the checks assert on */
        char line[60]; int l = 0;
        const char *pfx = "hikko: ring-3 window ";
        while (*pfx) line[l++] = *pfx++;
        l += utoa10(win.width, line + l); line[l++] = 'x';
        l += utoa10(win.height, line + l); line[l++] = '\n';
        jt_write(1, line, (unsigned)l);
    }

    sj_fetch();
    for (int i = 0; i < sj_n; i++) sj_order[i] = i;
    sj_sel = 0; sj_top = 0;
    sj_sort();   /* the forum lists pinned posts first: rank by votes */
    sj_sel = 0;
    say(sj_is_live ? "hikko: live " : "hikko: samples ", sj_n, -1);
    sj_draw();
    say("hikko: sel ", 0, -1);
    say_shown();

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
                int sel = sj_top + (ev.b - SJ_TOP) / SJ_ROW_H;
                if (sel < sj_n && sel < sj_top + sj_visible()) sj_sel = sel;
            }
        } else if (ev.kind == JT_EV_KEY) {
            if (ev.a == JT_KEY_ESC) break;
            if (ev.a == '`') { jt_write(1, "hikko: crashing on purpose\n", 27); *(volatile int *)0 = 1; } /* deliberate crash, as in every ring-3 app */
            if (ev.a == JT_KEY_UP && sj_sel > 0) sj_sel--;
            else if (ev.a == JT_KEY_DOWN && sj_sel < sj_n - 1) sj_sel++;
            else if (ev.a == 'u' || ev.a == 'U') {
                int idea = sj_order[sj_sel];
                sj_votes[idea]++;
                sj_sort();
                say("hikko: vote ", idea, sj_votes[idea]);
                say("hikko: pos ", sj_sel, -1);
            }
        } else {
            flags = JT_POLL_PRESENT;
            continue;
        }
        if (sj_sel != old) { say("hikko: sel ", sj_sel, -1); say_shown(); }
        sj_draw(); flags = JT_POLL_PRESENT;
    }
    jt_write(1, "hikko: closed\n", 14);
    jt_exit(0);
}
