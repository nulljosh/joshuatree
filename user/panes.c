/* panes: a cmux-style terminal multiplexer, a second app beside the Terminal (2.11.0, working name Panes).
 * A tab rail on the left, each tab its own workspace
 * with its own shell, and up to two panes per tab (side by side or stacked).
 *
 * Every pane is its own shell: its own scrollback, its own input line and
 * history (Up/Down), its own working directory. A tab is one or two panes. The
 * rail lists the tabs in sans (name = the last command or folder, a number for
 * Ctrl+n); a tab that is not on screen shows an accent dot when any of its panes
 * printed something since you last looked, and so does an unfocused pane in the
 * tab you are on. Only background work prints on its own: `sleep N [text]` is a
 * builtin that starts a timer in that pane and prints when it ends, which is
 * what makes the dot demonstrable. Everything else runs when you press Enter.
 *
 * Keys (the kernel delivers Ctrl chords to ring-3 windows as JT_KEY_CTL_*):
 *   Ctrl+T new tab         Ctrl+W close pane, then tab (the last tab resets)
 *   Ctrl+1..9 go to tab    Ctrl+Left/Right previous/next tab (Ctrl+Tab too
 *                          while this is the only window; with two windows
 *                          open the app switcher owns it)
 *   Ctrl+D split side by side    Ctrl+E split stacked    Ctrl+O next pane
 * Mouse: click a tab, the "New tab" row, or a pane.
 *
 * Look: warm near-black page, scrollback in sand, a "> " prompt pinned to the
 * bottom of each pane in amber, block cursor in the focused pane. The grid is
 * libjt's antialiased DejaVu Sans Mono (jt_mono_draw, 8 px advance); the rail,
 * labels and hint are the proportional body face. Accent #b5502c.
 *
 * Commands go to the kernel through the one syscall SYS_SHELL_RUN, which
 * answers from a short allowlist (help echo uptime mem ps ls cat, see
 * kernel/shellsys.c); anything else comes back as a one-line refusal. "clear",
 * "cd" and "sleep" are local. The pane keeps its own cwd (a relative path, ""
 * is the root), validates a target with jt_readdir, and sends "<cwd>\n<line>"
 * with every command so the kernel resolves ls and cat there without ever
 * moving the desktop's directory.
 *
 * Also: copy/cut take the whole input line into the system clipboard and paste
 * inserts it at the end, Esc closes the window, backquote is the deliberate
 * crash. Serial markers: panes: ring-3 window, terminal: ran=N (from the shared engine),
 * panes: tabs a=<n> b=<current, 1-based>, panes: panes a=<1|2> b=<layout*10+focus>,
 * panes: job done tab a=<tab, 1-based> b=<unseen>.
 */
#include "jtsys.h"
#include "libjt/text.h"
#include "shellcore.h" /* the Terminal's shell engine: cd, clear, SYS_SHELL_RUN */

#define BG     0x001A1512
#define DIMBG  0x00161110
#define RAILBG 0x00110E0C
#define RULE   0x002A221D
#define SELBG  0x002B211B
#define INK    0x00D8CFC4
#define TYPED  0x00F2E9D8
#define AMBER  0x00C98A3E
#define HINT   0x00807468
#define TABINK 0x00A89C8E
#define ACCENT 0x00B5502C
#define CELL   8
#define LH     17
#define MARGIN 16
#define RAIL_W 148
#define TAB_H  30
#define TAB_TOP 14
#define LINE_MAX SH_LINE_MAX
#define CWD_MAX SH_CWD_MAX
#define OUT_MAX SH_OUT_MAX
#define PSB 3072
#define HIST 5
#define MAX_TABS 9
#define POOL 12
#define NAME_MAX 24
#define KEY_COPY 302
/* 2.11.0 Ctrl chords, delivered to ring-3 windows by the kernel (kernel/app.h KEY_CTL_*); kept here so the
   shared jtsys.h, and with it every landing tile's source hash, stays as it was. */
#define JT_KEY_CTL_TAB   320
#define JT_KEY_CTL_LEFT  321
#define JT_KEY_CTL_RIGHT 322
#define JT_KEY_CTL_T     323
#define JT_KEY_CTL_W     324
#define JT_KEY_CTL_D     325
#define JT_KEY_CTL_E     326
#define JT_KEY_CTL_O     327
#define JT_KEY_CTL_1     330 /* Ctrl+n is JT_KEY_CTL_1 + n - 1 */
#define KEY_CUT 303
#define KEY_PASTE 304

static struct jt_window_info win JT_DATA = {0, 0, 0, 0};
extern char _user_end[];

struct pane {
    char sb[PSB + 1];
    char in[LINE_MAX + 1];
    char cwd[CWD_MAX + 1];
    char hist[HIST][LINE_MAX + 1];
    char jobmsg[40];
    unsigned slen, inlen, due;
    int hn, hsel;
    unsigned char used, unseen, job;
};
struct tab { signed char p[2]; unsigned char np, layout, focus; char name[NAME_MAX + 1]; };
struct arena {
    struct pane pane[POOL];
    struct tab tab[MAX_TABS];
    char out[OUT_MAX]; char clip[LINE_MAX + 1]; char req[CWD_MAX + LINE_MAX + 3];
    unsigned starts[80];
};
_Static_assert(sizeof(struct arena) < 60 * 1024, "panes arena must leave the 128KB window room for the image and stack");
static struct arena *ar JT_DATA = 0;
static int ntabs JT_DATA = 0, cur JT_DATA = 0, njobs JT_DATA = 0;

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
static unsigned mix(unsigned bg, unsigned fg, int a16) {
    unsigned r = 0;
    for (int sh = 0; sh <= 16; sh += 8) {
        int b = (int)((bg >> sh) & 255), f = (int)((fg >> sh) & 255);
        r |= (unsigned)(b + (f - b) * a16 / 16) << sh;
    }
    return r;
}
/* A filled disc of radius r (in quarter pixels) at centre (cx, cy), edge coverage by 4x4 supersampling. */
static void disc(int cx, int cy, int r4, unsigned c) {
    int rr = r4 / 4 + 1;
    for (int y = cy - rr; y <= cy + rr; y++) for (int x = cx - rr; x <= cx + rr; x++) {
        if (x < 0 || y < 0 || x >= (int)win.width || y >= (int)win.height) continue;
        int n = 0;
        for (int sy = 0; sy < 4; sy++) for (int sx = 0; sx < 4; sx++) {
            int dx = (x - cx) * 4 + sx * 1 - 2 + 0, dy = (y - cy) * 4 + sy - 2;
            if (dx * dx + dy * dy <= r4 * r4) n++;
        }
        if (n) { unsigned *px = win.pixels + (unsigned)y * win.width + (unsigned)x; *px = mix(*px, c, n); }
    }
}
/* Mono face: every glyph advances CELL (JT_MONO_ADV), so a column is a column. */
static void cell(int x, int y, char c, unsigned fg) {
    if (c == ' ') return;
    char g[2] = {c, 0};
    jt_mono_draw(&win, x, y, fg, g);
}
static unsigned nowticks(void) { struct jt_tasks t; jt_tasks(&t, 0); return t.ticks; }

static void mark(const char *tag, int a, int b) {
    char m[64]; int l = 0;
    for (const char *s = "panes: "; *s; s++) m[l++] = *s;
    for (; *tag; tag++) m[l++] = *tag;
    for (int pass = 0; pass < 2; pass++) {
        m[l++] = ' '; m[l++] = pass ? 'b' : 'a'; m[l++] = '=';
        int v = pass ? b : a;
        if (v < 0) { m[l++] = '-'; v = -v; }
        char d[10]; int dn = 0;
        if (!v) d[dn++] = '0';
        while (v && dn < 10) { d[dn++] = (char)('0' + v % 10); v /= 10; }
        while (dn) m[l++] = d[--dn];
    }
    m[l++] = '\n';
    jt_write(1, m, (unsigned)l);
}

/* ---- panes and tabs ---- */
static void pputc(struct pane *p, char c) {
    if (p->slen + 1 >= PSB) {
        /* Drop the oldest half in one move rather than a byte per character. */
        unsigned keep = PSB / 2;
        for (unsigned i = 0; i < keep; i++) p->sb[i] = p->sb[p->slen - keep + i];
        p->slen = keep;
    }
    p->sb[p->slen++] = c;
    p->sb[p->slen] = 0;
}
static void pputs(struct pane *p, const char *s) { while (*s) pputc(p, *s++); }
static void pputn(struct pane *p, unsigned v) {
    char d[10]; int n = 0;
    if (!v) d[n++] = '0';
    while (v) { d[n++] = (char)('0' + v % 10); v /= 10; }
    while (n) pputc(p, d[--n]);
}
static void tab_name(struct tab *t, const char *s) {
    int n = 0;
    while (*s == ' ') s++;
    while (*s && n < NAME_MAX) t->name[n++] = *s++;
    t->name[n] = 0;
}
static struct pane *tab_pane(int ti, int slot) { return &ar->pane[ar->tab[ti].p[slot]]; }
static struct pane *fp(void) { struct tab *t = &ar->tab[cur]; return &ar->pane[t->p[t->focus]]; }
static int pane_new(void) {
    for (int i = 0; i < POOL; i++) if (!ar->pane[i].used) {
        struct pane *p = &ar->pane[i];
        p->used = 1; p->unseen = 0; p->job = 0;
        p->slen = 0; p->sb[0] = 0; p->inlen = 0; p->in[0] = 0; p->cwd[0] = 0;
        p->hn = 0; p->hsel = -1;
        pputs(p, "Panes. Type help.\n");
        return i;
    }
    return -1;
}
static void pane_free(int i) {
    if (ar->pane[i].job) njobs--;
    ar->pane[i].used = 0; ar->pane[i].job = 0;
}
static void announce(void) {
    mark("tabs", ntabs, cur + 1);
    struct tab *t = &ar->tab[cur];
    mark("panes", t->np, t->layout * 10 + t->focus);
}
static void goto_tab(int i) {
    if (i < 0 || i >= ntabs) return;
    cur = i;
    fp()->unseen = 0;
    announce();
}
static void new_tab(void) {
    if (ntabs >= MAX_TABS) { pputs(fp(), "tab: nine tabs is the limit\n"); return; }
    int pi = pane_new();
    if (pi < 0) { pputs(fp(), "tab: no free panes\n"); return; }
    struct tab *t = &ar->tab[ntabs];
    t->p[0] = (signed char)pi; t->p[1] = -1; t->np = 1; t->layout = 0; t->focus = 0;
    tab_name(t, "shell");
    cur = ntabs++;
    announce();
}
static void close_tab_at(int i) {
    struct tab *t = &ar->tab[i];
    for (int s = 0; s < t->np; s++) pane_free(t->p[s]);
    for (int k = i; k + 1 < ntabs; k++) ar->tab[k] = ar->tab[k + 1];
    ntabs--;
    if (cur > i || cur >= ntabs) cur--;
    if (cur < 0) cur = 0;
}
static void close_focus(void) {
    struct tab *t = &ar->tab[cur];
    if (t->np == 2) {
        pane_free(t->p[t->focus]);
        t->p[0] = t->p[1 - t->focus]; t->p[1] = -1;
        t->np = 1; t->layout = 0; t->focus = 0;
    } else if (ntabs > 1) {
        close_tab_at(cur);
    } else {
        /* The last tab never disappears; it starts over. */
        pane_free(t->p[0]);
        int pi = pane_new();
        t->p[0] = (signed char)pi; tab_name(t, "shell");
    }
    fp()->unseen = 0;
    announce();
}
static void split(int layout) {
    struct tab *t = &ar->tab[cur];
    if (t->np == 2) { t->layout = (unsigned char)layout; announce(); return; }
    int pi = pane_new();
    if (pi < 0) { pputs(fp(), "split: no free panes\n"); return; }
    ar->pane[pi].cwd[0] = 0;
    for (int k = 0; fp()->cwd[k] && k < CWD_MAX; k++) { ar->pane[pi].cwd[k] = fp()->cwd[k]; ar->pane[pi].cwd[k + 1] = 0; }
    t->p[1] = (signed char)pi; t->np = 2; t->layout = (unsigned char)layout; t->focus = 1;
    announce();
}
static void focus_pane(int slot) {
    struct tab *t = &ar->tab[cur];
    if (slot < 0 || slot >= t->np) return;
    t->focus = (unsigned char)slot;
    fp()->unseen = 0;
    announce();
}

/* ---- drawing ---- */
static int tab_unseen(int ti) {
    struct tab *t = &ar->tab[ti];
    for (int s = 0; s < t->np; s++) if (ar->pane[t->p[s]].unseen) return 1;
    return 0;
}
static void label(int x, int y, int maxw, unsigned col, const char *s) {
    char b[NAME_MAX + 3];
    int n = 0;
    while (s[n] && n < NAME_MAX) { b[n] = s[n]; n++; }
    b[n] = 0;
    if (jt_text_width(JT_FACE_BODY, b) > maxw) {
        while (n > 1 && jt_text_width(JT_FACE_BODY, b) > maxw) { n--; b[n] = 0; }
        while (n > 1 && jt_text_width(JT_FACE_BODY, b) + jt_text_width(JT_FACE_BODY, "..") > maxw) { n--; b[n] = 0; }
        b[n] = '.'; b[n + 1] = '.'; b[n + 2] = 0;
    }
    jt_text_draw(&win, JT_FACE_BODY, x, y, col, b);
}
static void draw_rail(void) {
    rect(0, 0, RAIL_W, (int)win.height, RAILBG);
    rect(RAIL_W, 0, 1, (int)win.height, RULE);
    int th = jt_text_height(JT_FACE_BODY);
    for (int i = 0; i < ntabs; i++) {
        int y = TAB_TOP + i * TAB_H;
        if (y + TAB_H > (int)win.height - 30) break;
        if (i == cur) rect(8, y, RAIL_W - 16, TAB_H - 4, SELBG);
        int ty = y + (TAB_H - 4 - th) / 2;
        if (i != cur && tab_unseen(i)) disc(20, y + (TAB_H - 4) / 2, 14, ACCENT);
        char num[2] = {(char)('1' + i), 0};
        label(32, ty, RAIL_W - 32 - 34, i == cur ? TYPED : TABINK, ar->tab[i].name);
        jt_text_draw(&win, JT_FACE_BODY, RAIL_W - 24, ty, HINT, num);
    }
    int y = TAB_TOP + ntabs * TAB_H;
    if (ntabs < MAX_TABS && y + TAB_H <= (int)win.height - 30) {
        int ty = y + (TAB_H - 4 - th) / 2;
        jt_text_draw(&win, JT_FACE_BODY, 18, ty, HINT, "+");
        jt_text_draw(&win, JT_FACE_BODY, 32, ty, HINT, "New tab");
    }
}
static void draw_pane(struct pane *p, int px, int py, int pw, int ph, int focused) {
    int cols = (pw - 2 * MARGIN) / CELL;
    if (cols < 8) cols = 8;
    int prompt_y = py + ph - 26;
    int rows = (prompt_y - 12 - MARGIN - py) / LH;
    if (rows < 1) rows = 1;
    if (rows > 79) rows = 79;
    if (!focused) rect(px, py, pw, ph, DIMBG);

    /* One pass: where every wrapped line starts; keep the last `rows` of them. */
    unsigned total = 0, col = 0, ls = 0;
    for (unsigned i = 0; i <= p->slen; i++) {
        int wrapped = col == (unsigned)cols;
        int nl = i < p->slen && p->sb[i] == '\n';
        if (wrapped || nl || i == p->slen) {
            ar->starts[total % 80] = ls;
            total++;
            ls = nl ? i + 1 : i;
            col = 0;
            if (nl) continue;
            if (i == p->slen) break;
        }
        col++;
    }
    unsigned first = total > (unsigned)rows ? total - (unsigned)rows : 0;
    int y = py + MARGIN;
    unsigned ink = focused ? INK : mix(DIMBG, INK, 11);
    for (unsigned ln = first; ln < total && y < prompt_y - 8; ln++) {
        unsigned q = ar->starts[ln % 80];
        int x = px + MARGIN;
        for (int c = 0; c < cols && q < p->slen; c++, q++) {
            if (p->sb[q] == '\n') break;
            cell(x, y, p->sb[q], ink);
            x += CELL;
        }
        y += LH;
    }

    /* Prompt, pinned to the bottom so typing never scrolls out of view. */
    int x = px + MARGIN;
    unsigned pc = focused ? AMBER : mix(DIMBG, AMBER, 9);
    cell(x, prompt_y, '~', pc); x += CELL;
    for (const char *c = p->cwd; *c; c++, x += CELL) cell(x, prompt_y, *c, pc);
    cell(x, prompt_y, '>', pc); x += 2 * CELL;
    for (unsigned i = 0; i < p->inlen && x < px + pw - MARGIN - CELL; i++, x += CELL) cell(x, prompt_y, p->in[i], focused ? TYPED : mix(DIMBG, TYPED, 11));
    if (focused) rect(x, prompt_y + 1, CELL, LH - 3, AMBER); /* block cursor */
    if (p->unseen) disc(px + pw - 14, py + 14, 14, ACCENT);
}
/* Where the pane in `slot` of the current tab sits; shared by draw and click. */
static void pane_rect(int slot, int *x, int *y, int *w, int *h) {
    struct tab *t = &ar->tab[cur];
    int ax = RAIL_W + 1, aw = (int)win.width - ax, ah = (int)win.height - 26;
    *x = ax; *y = 0; *w = aw; *h = ah;
    if (t->np < 2) return;
    if (t->layout == 1) { int half = aw / 2; *w = half - 1; if (slot) { *x = ax + half; *w = aw - half; } }
    else { int half = ah / 2; *h = half - 1; if (slot) { *y = half; *h = ah - half; } }
}
static void draw(void) {
    rect(0, 0, (int)win.width, (int)win.height, BG);
    draw_rail();
    struct tab *t = &ar->tab[cur];
    for (int s = 0; s < t->np; s++) {
        int x, y, w, h;
        pane_rect(s, &x, &y, &w, &h);
        draw_pane(&ar->pane[t->p[s]], x, y, w, h, s == t->focus);
    }
    if (t->np == 2) {
        int x, y, w, h;
        pane_rect(1, &x, &y, &w, &h);
        if (t->layout == 1) rect(x - 1, 0, 1, (int)win.height - 26, RULE);
        else rect(x, y - 1, w, 1, RULE);
    }
    jt_text_draw(&win, JT_FACE_BODY, RAIL_W + 1 + MARGIN, (int)win.height - 26, HINT,
                 "ctrl t new tab   ctrl w close   ctrl 1-9 tabs   ctrl d split   ctrl e stack   ctrl o pane   esc quits");
}

/* ---- the shell ---- */
static void hist_add(struct pane *p) {
    if (p->hn && sh_seq(p->hist[0], p->in)) return;
    int top = p->hn < HIST ? p->hn : HIST - 1;
    for (int k = top; k > 0; k--) for (int i = 0; i <= LINE_MAX; i++) p->hist[k][i] = p->hist[k - 1][i];
    for (unsigned i = 0; i <= p->inlen; i++) p->hist[0][i] = p->in[i];
    if (p->hn < HIST) p->hn++;
}
static void hist_move(struct pane *p, int dir) {
    if (dir > 0) { if (p->hsel + 1 >= p->hn) return; p->hsel++; }
    else if (p->hsel > 0) p->hsel--;
    else { p->hsel = -1; p->inlen = 0; return; }
    unsigned n = 0;
    while (p->hist[p->hsel][n]) { p->in[n] = p->hist[p->hsel][n]; n++; }
    p->inlen = n;
}
static unsigned parse_uint(const char *s) {
    unsigned v = 0;
    while (*s >= '0' && *s <= '9' && v < 100000) v = v * 10 + (unsigned)(*s++ - '0');
    return v;
}
/* sleep N [text]: the one background job. It returns at once; the pane prints
   when the timer runs out, whichever tab you are looking at. */
static void do_sleep(struct pane *p, const char *arg) {
    unsigned n = parse_uint(arg);
    if (!n || n > 600) { pputs(p, "usage: sleep <1-600> [text]\n"); return; }
    if (p->job) { pputs(p, "sleep: one job per pane\n"); return; }
    while (*arg >= '0' && *arg <= '9') arg++;
    while (*arg == ' ') arg++;
    unsigned k = 0;
    if (*arg) while (*arg && k < sizeof p->jobmsg - 1) p->jobmsg[k++] = *arg++;
    else { const char *d = "sleep done"; while (*d) p->jobmsg[k++] = *d++; }
    p->jobmsg[k] = 0;
    p->due = nowticks() + n * 100; p->job = 1; njobs++;
    pputs(p, "[1] sleep "); pputn(p, n); pputs(p, " started\n");
}
/* Fires finished timers. Returns 1 when anything was printed so the caller repaints. */
static int jobs_tick(void) {
    if (!njobs) return 0;
    unsigned now = nowticks();
    int fired = 0;
    for (int ti = 0; ti < ntabs; ti++) for (int s = 0; s < ar->tab[ti].np; s++) {
        struct pane *p = tab_pane(ti, s);
        if (!p->job || (int)(now - p->due) < 0) continue;
        p->job = 0; njobs--;
        pputs(p, "[1] done: "); pputs(p, p->jobmsg); pputc(p, '\n');
        if (!(ti == cur && s == ar->tab[ti].focus)) p->unseen = 1;
        mark("job done tab", ti + 1, p->unseen);
        fired = 1;
    }
    return fired;
}

static struct pane *cur_out JT_DATA = 0;
static void out_put(char c) { pputc(cur_out, c); }
static void run_line(void) {
    struct pane *p = fp();
    struct tab *t = &ar->tab[cur];
    cur_out = p;
    p->in[p->inlen] = 0;
    p->hsel = -1;
    sh_echo(out_put, p->cwd, p->in);
    if (p->inlen) {
        const char *a = p->in;
        while (*a == ' ') a++;
        hist_add(p);
        tab_name(t, a);
        if (a[0] == 's' && a[1] == 'l' && a[2] == 'e' && a[3] == 'e' && a[4] == 'p' && (!a[5] || a[5] == ' ')) {
            a += 5;
            while (*a == ' ') a++;
            do_sleep(p, a);
        } else {
            int r = sh_dispatch(p->cwd, p->in, p->inlen, ar->req, ar->out, out_put);
            if (r == SH_CLEAR) { p->slen = 0; p->sb[0] = 0; }
            else if (r == SH_CD) {
                const char *base = p->cwd, *c;
                for (c = p->cwd; *c; c++) if (*c == '/') base = c + 1;
                tab_name(t, *p->cwd ? base : "shell");
            } else if (sh_seq(a, "help")) pputs(p, "also here: cd clear sleep <secs> [text]\nkeys: ctrl t w d e o, ctrl 1-9, ctrl left/right\n");
        }
    }
    p->inlen = 0;
}

static void click(int x, int y) {
    if (x < RAIL_W) {
        if (y < TAB_TOP) return;
        int i = (y - TAB_TOP) / TAB_H;
        if (i < ntabs) goto_tab(i);
        else if (i == ntabs) new_tab();
        return;
    }
    struct tab *t = &ar->tab[cur];
    for (int s = 0; s < t->np; s++) {
        int px, py, pw, ph;
        pane_rect(s, &px, &py, &pw, &ph);
        if (x >= px && x < px + pw && y >= py && y < py + ph + 26) { if (s != t->focus) focus_pane(s); return; }
    }
}

__attribute__((section(".text.start"), used))
void _start(int argc, char **argv) {
    (void)argc; (void)argv;
    if (jt_window_open(&win) != 0 || !win.pixels) { jt_write(2, "panes: no window\n", 17); jt_exit(1); }
    ar = (struct arena *)(((unsigned)_user_end + 15u) & ~15u); /* past the image, inside the 128KB window */
    for (int i = 0; i < POOL; i++) ar->pane[i].used = 0;
    new_tab();
    draw();
    jt_write(1, "panes: ring-3 window\n", 21);
    unsigned flags = JT_POLL_PRESENT;
    for (;;) {
        struct jt_event ev;
        int r = jt_window_poll(&ev, flags);
        if (r == 1 && jt_window_resized(&ev, &win)) { draw(); flags = JT_POLL_PRESENT; continue; } /* JT_EV_RESIZE: remapped, repaint at the new size */
        flags = 0;
        if (r == -11) {
            if (jobs_tick()) { draw(); flags = JT_POLL_PRESENT; }
            else jt_sched_yield();
            continue;
        }
        if (r != 1) break;
        flags = JT_POLL_PRESENT;
        if (ev.kind == JT_EV_CLICK) { click(ev.a, ev.b); draw(); continue; }
        if (ev.kind != JT_EV_KEY) continue;
        int k = ev.a;
        struct pane *p = fp();
        if (k == '`') { jt_write(1, "panes: crashing on purpose\n", 27); *(volatile int *)0 = 1; }
        if (k == JT_KEY_ESC) break;
        if (k == JT_KEY_CTL_T) new_tab();
        else if (k == JT_KEY_CTL_W) close_focus();
        else if (k == JT_KEY_CTL_D) split(1);
        else if (k == JT_KEY_CTL_E) split(2);
        else if (k == JT_KEY_CTL_O) focus_pane(ar->tab[cur].np == 2 ? 1 - ar->tab[cur].focus : 0);
        else if (k == JT_KEY_CTL_TAB || k == JT_KEY_CTL_RIGHT) goto_tab((cur + 1) % ntabs);
        else if (k == JT_KEY_CTL_LEFT) goto_tab((cur + ntabs - 1) % ntabs);
        else if (k >= JT_KEY_CTL_1 && k < JT_KEY_CTL_1 + 9) goto_tab(k - JT_KEY_CTL_1);
        else if (k == JT_KEY_ENTER) run_line();
        else if (k == 8) { if (p->inlen) p->inlen--; }
        else if (k == JT_KEY_UP) hist_move(p, 1);
        else if (k == JT_KEY_DOWN) hist_move(p, -1);
        else if (k == KEY_COPY || k == KEY_CUT) {
            jt_clip_set(p->in, p->inlen); /* the system clipboard, shared with Notes, Mail and Samantha */
            if (k == KEY_CUT) p->inlen = 0;
        }
        else if (k == KEY_PASTE) {
            int n = LINE_MAX > p->inlen ? jt_clip_get(ar->clip, LINE_MAX - p->inlen) : 0;
            for (int i = 0; i < n; i++) {
                char pc = ar->clip[i];
                if (pc >= 32 && pc < 127) p->in[p->inlen++] = pc;
            }
        }
        else if (k >= 32 && k < 127 && p->inlen < LINE_MAX) p->in[p->inlen++] = (char)k;
        else continue;
        draw();
    }
    jt_write(1, "panes: closed\n", 14);
    jt_exit(0);
}
