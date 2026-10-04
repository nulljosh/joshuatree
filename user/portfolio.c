/* portfolio: Joshua's app catalog, as a real ring-3 program.
 *
 * The eleventh app to leave the kernel (roadmap 2.0), done the way
 * user/clock.c was. Same About block and the same fleet catalog kernel/portfolio.h
 * showed in ring 0, grouped Life, Read, Make, Play and Dev. Up and down (or the
 * wheel) walk the app rows and skip the headers, a click selects, and the
 * bottom line shows the selected app's address. Built with no kernel include
 * path, linked flat, loaded off the VFS by exec_user, and it reaches the
 * machine only through int 0x80: SYS_WINDOW_OPEN, SYS_WINDOW_POLL, SYS_EXIT.
 *
 * Glyphs: the kernel's 8x16 VGA fallback font. The backquote key (`) is the
 * deliberate crash: a write through a null pointer, a page fault at ring 3,
 * reaped by the kernel. tools/checks/ring3portfolio-check.py presses it.
 */
#include "jtsys.h"
#include "libjt/text.h"

#define BG    0x00FAF8F6 /* GUI_BG */
#define INK   0x001C1C1E
#define HINT  0x0075726E
#define HEAD  0x00807468
#define PLAIN 0x00403439
#define LINK  0x00234A78
#define SELBG 0x00EDE6DC

#define PF_KIND_APP    0
#define PF_KIND_HEADER 1
#define PF_KIND_TEXT   2

typedef struct {
    const char *name;   /* 0 for a header or text row */
    const char *desc;   /* header/text content when name is 0, else one-liner */
    const char *url;    /* "" when the app has no web app */
    int kind;
} pf_row_t;

/* Names follow the fleet today. Block Frame still answers at its old wiretext host (DNS never moved). */
static const pf_row_t PF_ROWS[] = {
    {"Joshua Trommel", "", "", PF_KIND_TEXT},
    {0, "I build apps, and the operating system this is running on.", "", PF_KIND_TEXT},
    {0, "github.com/nulljosh", "github.com/nulljosh", PF_KIND_TEXT},
    {0, "journal.heyitsmejosh.com", "journal.heyitsmejosh.com", PF_KIND_TEXT},
    {0, "heyitsmejosh.com/docs.html (Docs)", "heyitsmejosh.com/docs.html", PF_KIND_TEXT},

    {0, "Life", "", PF_KIND_HEADER},
    {"Epiphany",  "finance dashboard",         "epiphany.heyitsmejosh.com", PF_KIND_APP},
    {"Healstack", "health and supplement tracker", "healstack.heyitsmejosh.com", PF_KIND_APP},
    {"Windgate",  "guided breathing",          "windgate.heyitsmejosh.com", PF_KIND_APP},
    {"Talli",     "benefits admin",            "talli.heyitsmejosh.com", PF_KIND_APP},
    {"Homeward",  "lost and found pets",       "homeward.heyitsmejosh.com", PF_KIND_APP},
    {"Brick",     "real estate browsing",      "brick.heyitsmejosh.com", PF_KIND_APP},
    {"HomeQi",    "feng shui home check",      "homeqi.heyitsmejosh.com", PF_KIND_APP},
    {"Weather",   "forecast and live conditions", "weather.heyitsmejosh.com", PF_KIND_APP},

    {0, "Read", "", PF_KIND_HEADER},
    {"Bookrank",  "book summaries",            "bookrank.heyitsmejosh.com", PF_KIND_APP},
    {"Inkpress",  "RSS reader",                "", PF_KIND_APP},
    {"Sidewise",  "news bias reader",          "sidewise.heyitsmejosh.com", PF_KIND_APP},
    {"Wordroot",  "etymology",                 "wordroot.heyitsmejosh.com", PF_KIND_APP},
    {"Fieldbook", "science explained plainly", "fieldbook.heyitsmejosh.com", PF_KIND_APP},
    {"Tonchi",    "language learning",         "lexly.heyitsmejosh.com", PF_KIND_APP}, /* was Lexly; tonchi.heyitsmejosh.com does not exist yet, so the address stays */

    {0, "Make", "", PF_KIND_HEADER},
    {"Block Frame", "wireframes in text",      "wiretext.heyitsmejosh.com", PF_KIND_APP},
    {"Curvely",   "equation grapher",          "curvely.heyitsmejosh.com", PF_KIND_APP},
    {"Numen",     "calculator canvas",         "numen.heyitsmejosh.com", PF_KIND_APP},
    {"Plain",     "text editor",               "", PF_KIND_APP},
    {"Notate",    "on-device transcription",   "notate.heyitsmejosh.com", PF_KIND_APP},
    {"Dream",     "dream journal",             "dream.heyitsmejosh.com", PF_KIND_APP},
    {"Costanza",  "poetry",                    "costanza.heyitsmejosh.com", PF_KIND_APP},
    {"Hikko",  "idea forum",                "hikko.heyitsmejosh.com", PF_KIND_APP},

    {0, "Play", "", PF_KIND_HEADER},
    {"Quotestreak", "quote guessing",          "quotestreak.heyitsmejosh.com", PF_KIND_APP},
    {"Keyrate",   "typing test",               "keyrate.heyitsmejosh.com", PF_KIND_APP},
    {"NYC",       "Times Square sim",          "nyc.heyitsmejosh.com", PF_KIND_APP},
    {"Conway",    "Game of Life on a torus",   "toroid.heyitsmejosh.com", PF_KIND_APP},
    {"Swing",     "random video chat",         "swing.heyitsmejosh.com", PF_KIND_APP},

    {0, "Dev", "", PF_KIND_HEADER},
    {"Nimble",    "instant answers",           "nimble.heyitsmejosh.com", PF_KIND_APP},
    {"Cadence",   "commit tracker",            "cadence.heyitsmejosh.com", PF_KIND_APP},
    {"Tripwire",  "API drift watcher",         "tripwire.heyitsmejosh.com", PF_KIND_APP},
    {"Seamark",   "read values off charts",    "seamark.heyitsmejosh.com", PF_KIND_APP},
    {"Hagaki",    "inbox triage",              "hagaki.heyitsmejosh.com", PF_KIND_APP},
    {"Curbfind",  "Craigslist browser",        "curbfind.heyitsmejosh.com", PF_KIND_APP},
    {"Turing",    "local LLM",                 "", PF_KIND_APP},
    {"Conveyer",  "AI plays Factorio",         "", PF_KIND_APP},
    {"Madobe",    "small web browser",         "madobe.heyitsmejosh.com", PF_KIND_APP},
    {"Plank",     "one-file language",         "plank.heyitsmejosh.com", PF_KIND_APP},
};
#define PF_ROW_COUNT (int)(sizeof(PF_ROWS) / sizeof(PF_ROWS[0]))
#define PF_ROW_H 22
#define PF_TOP   40

static struct jt_window_info win JT_DATA = {0, 0, 0, 0};
static int pf_sel JT_DATA = 0;    /* index into PF_ROWS, always an app row */
static int pf_scroll JT_DATA = 0; /* first visible row */

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
static void etext(const char *s, int x, int y, int maxw, unsigned fg) {
    char t[72]; int n = 0;
    while (s[n] && n < 64) { t[n] = s[n]; n++; }
    t[n] = 0;
    if (jt_text_width(JT_FACE_BODY, t) > maxw) {
        while (n > 0) {
            t[n] = '.'; t[n + 1] = '.'; t[n + 2] = '.'; t[n + 3] = 0;
            if (jt_text_width(JT_FACE_BODY, t) <= maxw) break;
            t[--n] = 0;
        }
        if (n == 0) t[0] = 0;
    }
    jt_text_draw(&win, JT_FACE_BODY, x, y, fg, t);
}
static int utoa10(unsigned v, char *buf) {
    char tmp[12]; int tn = 0, n = 0;
    do { tmp[tn++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (tn) buf[n++] = tmp[--tn];
    buf[n] = 0;
    return n;
}

static int pf_first_app_row(void) {
    for (int i = 0; i < PF_ROW_COUNT; i++) if (PF_ROWS[i].kind == PF_KIND_APP) return i;
    return 0;
}
static int pf_vis_rows(void) {
    int list_h = (int)win.height - 60 - PF_TOP;
    if (list_h < 0) list_h = 0;
    int v = list_h / PF_ROW_H;
    return v < 1 ? 1 : v;
}
static void pf_clamp_scroll(int vis_rows) {
    if (pf_sel < pf_scroll) pf_scroll = pf_sel;
    if (pf_sel >= pf_scroll + vis_rows) pf_scroll = pf_sel - vis_rows + 1;
    if (pf_scroll < 0) pf_scroll = 0;
    int max_scroll = PF_ROW_COUNT - vis_rows;
    if (max_scroll < 0) max_scroll = 0;
    if (pf_scroll > max_scroll) pf_scroll = max_scroll;
}

static void pf_draw(void) {
    int w = (int)win.width, h = (int)win.height;
    int vis_rows = pf_vis_rows();
    pf_clamp_scroll(vis_rows);
    rect(0, 0, w, h, BG);
        for (int r = 0; r < vis_rows; r++) {
        int i = pf_scroll + r;
        if (i >= PF_ROW_COUNT) break;
        int y = PF_TOP + r * PF_ROW_H;
        const pf_row_t *row = &PF_ROWS[i];
        if (row->kind == PF_KIND_HEADER) { text(row->desc, 20, y + 2, HEAD); continue; }
        if (row->kind == PF_KIND_TEXT) {
            if (row->name) etext(row->name, 20, y + 2, w - 40, INK);
            else if (row->url[0]) etext(row->desc, 20, y + 2, w - 40, LINK);
            else etext(row->desc, 20, y + 2, w - 40, PLAIN);
            continue;
        }
        if (i == pf_sel) rect(20, y - 2, w - 40, PF_ROW_H - 2, SELBG);
        int nx = text(row->name, 32, y + 2, INK) + 14;
        etext(row->desc, nx, y + 2, w - 32 - nx, HINT);
    }
    const pf_row_t *sel = &PF_ROWS[pf_sel];
    text("up/down or scroll to browse   esc closes", 20, h - 50, HINT);
    if (sel->url[0]) etext(sel->url, 20, h - 26, w - 40, LINK);
    else text("no web app", 20, h - 26, HEAD);
}

__attribute__((section(".text.start"), used))
void _start(int argc, char **argv) {
    (void)argc; (void)argv;
    if (jt_window_open(&win) != 0 || !win.pixels) {
        jt_write(2, "portfolio: no window\n", 21);
        jt_exit(1);
    }
    {   /* one line, one write: what tools/checks/ring3portfolio-check.py asserts on */
        char line[60]; int l = 0;
        const char *pfx = "portfolio: ring-3 window ";
        while (*pfx) line[l++] = *pfx++;
        l += utoa10(win.width, line + l); line[l++] = 'x';
        l += utoa10(win.height, line + l); line[l++] = '\n';
        jt_write(1, line, (unsigned)l);
    }

    pf_sel = pf_first_app_row();
    pf_scroll = 0;
    pf_draw();

    unsigned flags = JT_POLL_PRESENT;
    for (;;) {
        struct jt_event ev;
        int r = jt_window_poll(&ev, flags);
        if (r == 1 && jt_window_resized(&ev, &win)) { pf_draw(); flags = JT_POLL_PRESENT; continue; } /* JT_EV_RESIZE: remapped, repaint at the new size */
        flags = 0;
        if (r == -11 /* -EAGAIN */) { jt_sched_yield(); continue; }
        if (r != 1) break;

        int old = pf_sel;
        int up = 0, down = 0;
        if (ev.kind == JT_EV_CLICK) {
            int vis_rows = pf_vis_rows(), hit = -1;
            for (int rr = 0; rr < vis_rows; rr++) {
                int i = pf_scroll + rr;
                if (i >= PF_ROW_COUNT) break;
                if (PF_ROWS[i].kind != PF_KIND_APP) continue;
                int y = PF_TOP + rr * PF_ROW_H;
                if (ev.a >= 20 && ev.a < (int)win.width - 20 && ev.b >= y - 2 && ev.b < y + PF_ROW_H - 4) { hit = i; break; }
            }
            if (hit < 0) break; /* the titlebar X, or anywhere off an app row */
            pf_sel = hit;
        } else if (ev.kind == JT_EV_KEY) {
            if (ev.a == JT_KEY_ESC) break;
            if (ev.a == '`') {
                jt_write(1, "portfolio: crashing on purpose\n", 31);
                *(volatile int *)0 = 1;
            }
            if (ev.a == JT_KEY_UP) up = 1;
            else if (ev.a == JT_KEY_DOWN) down = 1;
        } else if (ev.kind == JT_EV_WHEEL) {
            if (ev.a > 0) up = 1; else if (ev.a < 0) down = 1;
        } else {
            flags = JT_POLL_PRESENT;
            continue;
        }
        if (up) { for (int i = pf_sel - 1; i >= 0; i--) if (PF_ROWS[i].kind == PF_KIND_APP) { pf_sel = i; break; } }
        if (down) { for (int i = pf_sel + 1; i < PF_ROW_COUNT; i++) if (PF_ROWS[i].kind == PF_KIND_APP) { pf_sel = i; break; } }
        if (pf_sel != old) {
            /* One line, one write: the marker tools/checks/ring3portfolio-check.py
               reads to confirm the real selection logic ran, not just the draw. */
            char line[32]; int l = 0;
            const char *pfx = "portfolio: sel "; while (*pfx) line[l++] = *pfx++;
            l += utoa10((unsigned)pf_sel, line + l); line[l++] = '\n';
            jt_write(1, line, (unsigned)l);
        }
        pf_draw(); flags = JT_POLL_PRESENT;
    }
    jt_write(1, "portfolio: closed\n", 18);
    jt_exit(0);
}
