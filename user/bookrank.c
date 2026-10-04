/* bookrank: Joshua's ranked non-fiction shelf, as a real ring-3 program.
 *
 * The fifth app to leave the kernel (roadmap 2.0), done exactly the way
 * user/keyrate.c, user/toroid.c, user/calculator.c and user/quotes.c
 * were. Two panes: the ranked list on the left, the selected book on the
 * right (title, author, rating bar, review count, badge, and the one-line
 * summary). Up/down or a click selects; esc closes. Built with no kernel
 * include path, linked flat, loaded off the VFS by exec_user, and it
 * reaches the machine only through int 0x80: SYS_WINDOW_OPEN for a
 * framebuffer, SYS_WINDOW_POLL for input and the present, SYS_EXIT to
 * leave.
 *
 * Live shelf (2.6.26): the top twelve books come from the real Bookrank
 * API through SYS_HTTP_GET (the way user/curbfind.c gets its deals). The
 * kernel fixes the host; the Worker's /api/books turns the real app's JSON
 * into plain text: one total line, then "rank|rating x100|reviews|badge|
 * title|author|notes" rows. When the fetch fails or the reply does not
 * parse, the ten compiled-in samples show, as before. The reply is
 * untrusted: every byte is forced to printable ASCII, every copy is
 * bounded by the buffer, and a row that is short or has no title is
 * dropped. Chapter summaries are per-account in the real app, so they are
 * not here; the notes line is the summary.
 *
 * Type: the antialiased libjt face. A summary wraps at word boundaries by
 * real pixel width, and long titles end in an ellipsis.
 *
 * The backquote key (`) is the deliberate crash, same as the other four:
 * a write through a null pointer, a page fault at ring 3, reaped by the
 * kernel. tools/checks/ring3bookrank-check.py presses it on purpose;
 * tools/checks/ring3bookrank-live-check.py proves the live and offline
 * paths.
 */
#include "jtsys.h"
#include "libjt/text.h"

#define BG    0x00FAF8F6 /* GUI_BG */
#define ROW   0x00F1EDE7
#define SEL   0x00E2D8CC
#define INK   0x001C1C1E
#define HINT  0x0075726E

#define BR_LIST_X  20
#define BR_LIST_W  220
#define BR_INFO_X  260
#define BR_ITEM_H  28
#define BR_LIST_TOP 40

typedef struct { const char *title, *author, *summary, *reviews, *badge; int rank, rating; } BrBook; /* rank 0 = by position; rating in hundredths, 0 = none */
static const BrBook BR_BOOKS[] = {
    {"Thinking, Fast and Slow", "Daniel Kahneman", "Explores how our minds make decisions through fast intuitive thinking and slow deliberate reasoning. Reveals systematic biases and heuristics that shape human judgment.", "", "", 0, 0},
    {"Sapiens", "Yuval Noah Harari", "Chronicles humanity's rise from hunter-gatherers to modern civilization. Examines how myths and shared beliefs shaped society.", "", "", 0, 0},
    {"The Selfish Gene", "Richard Dawkins", "Proposes that genes, not organisms, are the primary units of evolution and self-interest. Challenges how we understand natural selection and behavior.", "", "", 0, 0},
    {"Educated", "Tara Westover", "Memoir of a woman who grew up in an isolated survivalist family with no formal education. Recounts her journey to escape and eventually earn a PhD.", "", "", 0, 0},
    {"Atomic Habits", "James Clear", "Breaks down habit formation into tiny incremental changes that compound over time. Practical framework for building better routines and breaking bad ones.", "", "", 0, 0},
    {"The Lean Startup", "Eric Ries", "Introduces rapid iteration and validated learning for building businesses efficiently. Challenges traditional business planning with a startup methodology.", "", "", 0, 0},
    {"Freakonomics", "Steven Levitt, Stephen Dubner", "Applies economic thinking to everyday life and hidden incentives. Reveals surprising connections between seemingly unrelated phenomena.", "", "", 0, 0},
    {"The Art of War", "Sun Tzu", "Ancient military treatise on strategy, tactics, and the nature of conflict. Principles apply to business, negotiation, and competition.", "", "", 0, 0},
    {"Grit", "Angela Duckworth", "Argues that passion and perseverance matter more than raw talent for success. Research shows sustained effort and resilience predict achievement.", "", "", 0, 0},
    {"The Structure of Scientific Revolutions", "Thomas Kuhn", "Explains how science progresses through paradigm shifts rather than linear accumulation. Challenges the idea that science simply discovers pre-existing truth.", "", "", 0, 0},
};
#define BR_COUNT ((int)(sizeof(BR_BOOKS) / sizeof(BR_BOOKS[0])))
#define BR_LIVE_MAX 12
#define BR_PATH "/api/books"
#define BARFULL  0x002F7B4F
#define BAREMPTY 0x00E8E6E1

static struct jt_window_info win JT_DATA = {0, 0, 0, 0};
static char br_body[4096] JT_DATA;                 /* the reply, parsed in place */
static BrBook br_live[BR_LIVE_MAX] JT_DATA;
static const BrBook *br_rows JT_DATA = BR_BOOKS;
static int br_n JT_DATA = BR_COUNT;
static int br_total JT_DATA = 0;                   /* the real shelf's size; 0 = offline */
static int br_sel JT_DATA = 0;
static int br_top JT_DATA = 0;                     /* first row shown in the list */

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
static void text(const char *s, int x, int y, unsigned fg) { jt_text_draw(&win, JT_FACE_BODY, x, y, fg, s); }

/* Copy s into out (cap bytes), cutting with "..." so it fits maxw pixels. */
static void fit(char *out, int cap, const char *s, int maxw) {
    int n = 0;
    while (s[n] && n < cap - 4) { out[n] = s[n]; n++; }
    out[n] = 0;
    if (jt_text_width(JT_FACE_BODY, out) <= maxw && !s[n]) return;
    while (n > 0) {
        out[n] = '.'; out[n + 1] = '.'; out[n + 2] = '.'; out[n + 3] = 0;
        if (jt_text_width(JT_FACE_BODY, out) <= maxw) return;
        out[--n] = 0;
    }
    out[0] = 0;
}
/* Word-boundary wrap by real glyph widths into at most max_lines lines of
   width w; if text is left over the last line ends in an ellipsis. */
static void wrap_text(const char *s, int x, int y, int w, int max_lines, int lh, unsigned fg) {
    char buf[128];
    for (int line = 0; *s && line < max_lines; line++) {
        while (*s == ' ') s++;
        if (!*s) break;
        int n = 0, brk = -1;
        while (s[n] && n < (int)sizeof buf - 1) {
            buf[n] = s[n]; buf[n + 1] = 0;
            if (jt_text_width(JT_FACE_BODY, buf) > w) break;
            if (s[n] == ' ') brk = n;
            n++;
        }
        if (s[n]) { if (brk > 0) n = brk; else if (n == 0) n = 1; }
        if (line == max_lines - 1 && s[n]) {
            fit(buf, sizeof buf, s, w);
            text(buf, x, y + line * lh, fg);
            return;
        }
        for (int i = 0; i < n; i++) buf[i] = s[i];
        buf[n] = 0;
        text(buf, x, y + line * lh, fg);
        s += n;
    }
}

static int itoa10(int v, char *buf) {
    char tmp[12]; int tn = 0, n = 0; unsigned u = v < 0 ? (unsigned)(-v) : (unsigned)v;
    if (v < 0) buf[n++] = '-';
    do { tmp[tn++] = (char)('0' + u % 10); u /= 10; } while (u);
    while (tn) buf[n++] = tmp[--tn];
    buf[n] = 0;
    return n;
}
static void say(const char *pfx, int a) {
    char line[64]; int l = 0;
    while (*pfx && l < 40) line[l++] = *pfx++;
    l += itoa10(a, line + l);
    line[l++] = '\n';
    jt_write(1, line, (unsigned)l);
}
/* "bookrank: shown <title>": what the right pane is drawing, for the check. */
static void say_shown(const BrBook *b) {
    char line[96]; int l = 0;
    const char *pfx = "bookrank: shown ";
    while (*pfx) line[l++] = *pfx++;
    for (const char *t = b->title; *t && l < 90; t++) line[l++] = (*t >= 0x20 && *t <= 0x7e) ? *t : '?';
    line[l++] = '\n';
    jt_write(1, line, (unsigned)l);
}

static int digits(const char *s, int cap) { /* leading decimal digits, at most 5, clamped to cap */
    int v = 0, k = 0;
    while (*s >= '0' && *s <= '9' && k < 5) { v = v * 10 + (*s++ - '0'); k++; }
    return v > cap ? cap : v;
}

/* Parse the Worker's text in place. Line 1 is the shelf total (digits only,
   or the reply is not ours); then rank|rating|reviews|badge|title|author|notes.
   Returns the rows kept; on 0 the samples stay. Nothing is trusted: bytes are
   forced printable, the scan stops at the NUL, rows past the cap are ignored. */
static int br_parse(char *body, int n) {
    body[n] = 0;
    for (int i = 0; i < n; i++) {
        unsigned char c = (unsigned char)body[i];
        if (c == '\r') body[i] = ' ';
        else if (c != '\n' && (c < 0x20 || c > 0x7e)) body[i] = '?';
    }
    char *p = body;
    if (*p < '0' || *p > '9') return 0;
    int total = digits(p, 99999);
    while (*p && *p != '\n') { if (*p < '0' || *p > '9') return 0; p++; }
    if (*p) p++;
    int live = 0;
    while (*p && live < BR_LIVE_MAX) {
        char *line = p, *f[7]; int k = 1;
        f[0] = line;
        while (*p && *p != '\n') {
            if (*p == '|' && k < 7) { *p = 0; f[k++] = p + 1; }
            p++;
        }
        if (*p) *p++ = 0;
        if (k < 7 || !f[4][0]) continue;
        BrBook *b = &br_live[live];
        b->rank = digits(f[0], 999);
        b->rating = digits(f[1], 500);
        b->reviews = f[2]; b->badge = f[3];
        b->title = f[4]; b->author = f[5]; b->summary = f[6];
        live++;
    }
    if (live) { br_total = total < live ? live : total; }
    return live;
}

/* One GET through the kernel; on anything but a 200 with a body that fits and
   parses, the samples stay. */
static void br_fetch(void) {
    br_rows = BR_BOOKS; br_n = BR_COUNT; br_total = 0;
    int n = jt_http_get(BR_PATH, br_body, sizeof(br_body) - 1);
    say("bookrank: fetch ", n);
    if (n <= 0 || n >= (int)sizeof(br_body) - 1) return;
    int live = br_parse(br_body, n);
    if (live) { br_rows = br_live; br_n = live; }
}

static int br_visible(void) {
    int max_items = ((int)win.height - BR_LIST_TOP - 40) / BR_ITEM_H;
    if (max_items < 1) max_items = 1;
    return br_n < max_items ? br_n : max_items;
}
static void br_scroll(void) { /* keep the selection inside the visible rows */
    int vis = br_visible();
    if (br_sel < br_top) br_top = br_sel;
    if (br_sel >= br_top + vis) br_top = br_sel - vis + 1;
    if (br_top < 0) br_top = 0;
}

static void br_draw(void) {
    rect(0, 0, (int)win.width, (int)win.height, BG);
    br_scroll();

    if (br_total) {
        char hdr[64]; int l = 0; const char *t = "Ranked shelf, top ";
        while (*t) hdr[l++] = *t++;
        l += itoa10(br_n, hdr + l);
        t = " of "; while (*t) hdr[l++] = *t++;
        l += itoa10(br_total, hdr + l);
        hdr[l] = 0;
        text(hdr, BR_LIST_X, 12, HINT);
    } else {
        text("Offline, sample shelf", BR_LIST_X, 12, HINT);
    }

    int shown = br_visible();
    for (int k = 0; k < shown; k++) {
        int i = br_top + k;
        int y = BR_LIST_TOP + k * BR_ITEM_H;
        unsigned bg = (i == br_sel) ? SEL : ROW;
        unsigned fg = (i == br_sel) ? INK : HINT;
        rect(BR_LIST_X, y, BR_LIST_W, BR_ITEM_H - 2, bg);
        char rank[8];
        int r = itoa10(br_rows[i].rank ? br_rows[i].rank : i + 1, rank);
        rank[r++] = '.'; rank[r] = 0;
        text(rank, BR_LIST_X + 6, y + 6, fg);

        char short_title[64];
        fit(short_title, sizeof short_title, br_rows[i].title, BR_LIST_W - 30 - 8);
        text(short_title, BR_LIST_X + 30, y + 6, fg);
    }

    if (br_sel < br_n) {
        const BrBook *b = &br_rows[br_sel];
        int info_top = BR_LIST_TOP;
        int info_w = (int)win.width - BR_INFO_X - 20;
        char head[80];
        fit(head, sizeof head, b->title, info_w);
        text(head, BR_INFO_X, info_top, INK);
        fit(head, sizeof head, b->author, info_w);
        text(head, BR_INFO_X, info_top + 22, HINT);
        int ny = info_top + 48;
        int lh = jt_text_height(JT_FACE_BODY) + 4;
        if (b->rating > 0) {   /* "4.38", a five-segment bar, the review count */
            char rt[8]; int l = 0;
            rt[l++] = (char)('0' + b->rating / 100); rt[l++] = '.';
            rt[l++] = (char)('0' + b->rating / 10 % 10); rt[l++] = (char)('0' + b->rating % 10); rt[l] = 0;
            int x = jt_text_draw(&win, JT_FACE_BODY, BR_INFO_X, ny, INK, rt) + 8;
            int fill = (b->rating * 80 + 250) / 500;
            rect(x, ny + 6, 80, 8, BAREMPTY);
            rect(x, ny + 6, fill, 8, BARFULL);
            if (b->reviews[0]) {
                char rv[48];
                fit(rv, sizeof rv, b->reviews, info_w - (x + 80 + 8 - BR_INFO_X));
                text(rv, x + 80 + 8, ny, HINT);
            }
            ny += lh;
            if (b->badge[0]) { fit(head, sizeof head, b->badge, info_w); text(head, BR_INFO_X, ny, BARFULL); ny += lh; }
            ny += 6;
        }
        int max_lines = ((int)win.height - ny - 40) / lh;
        wrap_text(b->summary, BR_INFO_X, ny, info_w, max_lines, lh, INK);
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
        l += itoa10((int)win.width, line + l); line[l++] = 'x';
        l += itoa10((int)win.height, line + l); line[l++] = '\n';
        jt_write(1, line, (unsigned)l);
    }

    br_sel = 0; br_top = 0;
    br_draw(); /* first frame before the network round trip, so the window never opens blank */
    struct jt_event ev;
    int pending = jt_window_poll(&ev, JT_POLL_PRESENT) == 1; /* presents; an event this early is kept, not lost */
    br_fetch();
    say(br_total ? "bookrank: live " : "bookrank: samples ", br_n);
    say_shown(&br_rows[0]);
    br_draw();

    unsigned flags = JT_POLL_PRESENT;
    for (;;) {
        if (!pending) {
            int r = jt_window_poll(&ev, flags);
            if (r == 1 && jt_window_resized(&ev, &win)) { br_draw(); flags = JT_POLL_PRESENT; continue; } /* JT_EV_RESIZE: remapped, repaint at the new size */
            flags = 0;
            if (r == -11 /* -EAGAIN */) { jt_sched_yield(); continue; }
            if (r != 1) break;
        }
        pending = 0;

        if (ev.kind == JT_EV_KEY) {
            if (ev.a == JT_KEY_ESC) break;
            if (ev.a == '`') {
                jt_write(1, "bookrank: crashing on purpose\n", 31);
                *(volatile int *)0 = 1;
            }
            if (ev.a == JT_KEY_UP && br_sel > 0) br_sel--;
            else if (ev.a == JT_KEY_DOWN && br_sel < br_n - 1) br_sel++;
            else { flags = JT_POLL_PRESENT; continue; }
        } else if (ev.kind == JT_EV_CLICK) {
            int vx = ev.a, vy = ev.b;
            if (vy >= BR_LIST_TOP && vx >= BR_LIST_X && vx < BR_LIST_X + BR_LIST_W) {
                int k = (vy - BR_LIST_TOP) / BR_ITEM_H;
                if (k < br_visible()) br_sel = br_top + k;
                else break;
            } else break; /* titlebar X or off the app */
        } else {
            flags = JT_POLL_PRESENT;
            continue;
        }

        /* One line, one write: what tools/checks/ring3bookrank-check.py
           reads to confirm a real selection ran, not just the draw. */
        say("bookrank: sel ", br_sel + 1);
        say_shown(&br_rows[br_sel]);

        br_draw();
        flags = JT_POLL_PRESENT;
    }
    jt_write(1, "bookrank: closed\n", 18);
    jt_exit(0);
}
