/* solitaire: Klondike, draw one, as a ring-3 program (roadmap "Apps after 2.2").
 *
 * Same shape as user/mines.c: SYS_WINDOW_OPEN for a framebuffer,
 * SYS_WINDOW_POLL for input and the present, SYS_EXIT to leave.
 *
 * Keys: d or space draws (and turns the waste back over when the stock is
 * empty). Pick a source, then a place: w is the waste, 1 to 7 the columns,
 * f the foundations. a sends every card that can go up to the foundations.
 * r deals again, esc closes. A click on a pile does what its key does.
 *
 * The first deal of every run comes from a fixed seed, so
 * tools/checks/ring3-solitaire-check.py deals the same cards with the same
 * xorshift, plays the same game by keyboard and expects the same moves.
 */
#include "jtsys.h"
#include "libjt/text.h"
#include "libjt/hires.h"

#define TABLE  0x00E9E3D9
#define FACE   0x00FFFFFF
#define EDGE   0x00C9C1B5
#define BACK   0x00B5502C
#define BACK2  0x00C9673F
#define INK    0x001C1C1E
#define RED    0x00C0362C
#define HINT   0x0075726E
#define SEL    0x00B5502C

#define CW 54
#define CH 72
#define GAP 10
#define PAD 16
#define TOPY 12
#define TABY (TOPY + CH + 14)
#define UP 0x80

/* Piles: 0 stock, 1 waste, 2..5 foundations (by suit), 6..12 the columns. A card is suit * 13 + rank - 1;
   suit 0 spades, 1 hearts, 2 diamonds, 3 clubs. UP marks a face-up card. */
#define STOCK 0
#define WASTE 1
#define FOUND 2
#define TAB   6
struct pile { unsigned char c[52]; int n; };

static struct jt_window_info win JT_DATA = {0, 0, 0, 0};
static struct pile p[13] JT_DATA = {{{0}, 0}};
static int sel JT_DATA = -1, won JT_DATA = 0;
static unsigned seed JT_DATA = 0x2545F491u;

static unsigned rnd(void) { seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5; return seed; }
static void say(const char *s) { unsigned n = 0; while (s[n]) n++; jt_write(1, s, n); }
static int utoa10(unsigned v, char *buf) {
    char tmp[12]; int tn = 0, n = 0;
    do { tmp[tn++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (tn) buf[n++] = tmp[--tn];
    buf[n] = 0;
    return n;
}

static int rank(int c) { return (c & 0x7F) % 13 + 1; }
static int suit(int c) { return (c & 0x7F) / 13; }
static int red(int c) { return suit(c) == 1 || suit(c) == 2; }
static int top(int i) { return p[i].n ? p[i].c[p[i].n - 1] : -1; }
static void push(int i, int c) { p[i].c[p[i].n++] = (unsigned char)c; }
static int pop(int i) { return p[i].c[--p[i].n]; }
static void name(int c, char *s) { s[0] = "A23456789TJQK"[rank(c) - 1]; s[1] = "SHDC"[suit(c)]; s[2] = 0; }

static void deal(void) {
    unsigned char deck[52];
    for (int i = 0; i < 52; i++) deck[i] = (unsigned char)i;
    for (int i = 51; i > 0; i--) { int j = (int)(rnd() % (unsigned)(i + 1)); unsigned char t = deck[i]; deck[i] = deck[j]; deck[j] = t; }
    for (int i = 0; i < 13; i++) p[i].n = 0;
    int k = 0;
    for (int col = 0; col < 7; col++)
        for (int row = 0; row <= col; row++) push(TAB + col, deck[k++] | (row == col ? UP : 0));
    while (k < 52) push(STOCK, deck[k++]);
    sel = -1; won = 0;
    char line[64] = "solitaire: tops"; int l = 15;
    for (int col = 0; col < 7; col++) { line[l++] = ' '; name(top(TAB + col), line + l); l += 2; }
    line[l++] = '\n'; line[l] = 0;
    say(line);
}

static void log_move(const char *src, const char *dst, int c) {
    char line[48] = "solitaire: move "; int l = 16;
    while (*src) line[l++] = *src++;
    line[l++] = ' ';
    while (*dst) line[l++] = *dst++;
    line[l++] = ' '; name(c, line + l); l += 2;
    line[l++] = '\n'; line[l] = 0;
    say(line);
}
static void check_state(void) {   /* every card once, 52 in all */
    unsigned char seen[52] = {0}; int n = 0;
    for (int i = 0; i < 13; i++) for (int j = 0; j < p[i].n; j++) { int c = p[i].c[j] & 0x7F; if (c < 52 && !seen[c]) { seen[c] = 1; n++; } }
    int total = 0; for (int i = 0; i < 13; i++) total += p[i].n;
    if (n != 52 || total != 52) say("solitaire: bad state\n");
    if (!won && p[FOUND].n + p[FOUND + 1].n + p[FOUND + 2].n + p[FOUND + 3].n == 52) { won = 1; say("solitaire: won\n"); }
}

static void draw_card(void) {
    if (p[STOCK].n) { push(WASTE, pop(STOCK) | UP); }
    else if (p[WASTE].n) { while (p[WASTE].n) push(STOCK, pop(WASTE) & 0x7F); say("solitaire: recycled\n"); }
    sel = -1;
}
static int fits_found(int c) { return (c & UP) && rank(c) == p[FOUND + suit(c)].n + 1; }
static int fits_col(int c, int col) {
    int t = top(TAB + col);
    if (t < 0) return rank(c) == 13;
    return (t & UP) && red(t) != red(c) && rank(t) == rank(c) + 1;
}
static void flip(int col) { int i = TAB + col; if (p[i].n && !(p[i].c[p[i].n - 1] & UP)) p[i].c[p[i].n - 1] |= UP; }

/* Move from a source (WASTE or a column pile) to dst: -1 the foundations, else a column 0..6. Returns 1 on a move. */
static int move(int src, int dst) {
    char s[2] = {src == WASTE ? 'w' : (char)('1' + src - TAB), 0}, d[2] = {dst < 0 ? 'f' : (char)('1' + dst), 0};
    if (!p[src].n || src == TAB + dst) return 0;
    if (dst < 0) {
        int c = top(src);
        if (!fits_found(c)) return 0;
        push(FOUND + suit(c), pop(src));
        log_move(s, d, c);
    } else if (src == WASTE) {
        int c = top(src);
        if (!fits_col(c, dst)) return 0;
        push(TAB + dst, pop(src));
        log_move(s, d, c);
    } else {
        int i = 0;
        while (i < p[src].n && !(p[src].c[i] & UP)) i++;
        for (; i < p[src].n && !fits_col(p[src].c[i], dst); i++) {}
        if (i >= p[src].n) return 0;
        int c = p[src].c[i], n = p[src].n - i;
        for (int k = 0; k < n; k++) push(TAB + dst, p[src].c[i + k]);
        p[src].n = i;
        log_move(s, d, c);
    }
    if (src >= TAB) flip(src - TAB);
    check_state();
    return 1;
}
static void auto_up(void) {
    int moved = 0, again = 1;
    while (again) {
        again = 0;
        if (move(WASTE, -1)) { moved++; again = 1; }
        for (int col = 0; col < 7; col++) if (move(TAB + col, -1)) { moved++; again = 1; }
    }
    char line[32] = "solitaire: auto "; int l = 16;
    l += utoa10((unsigned)moved, line + l); line[l++] = '\n'; line[l] = 0;
    say(line);
}
/* One pick: a source first, then where it goes. pick is WASTE, a column pile, or -1 for the foundations. */
static void pick(int what) {
    if (sel < 0) {
        if (what >= WASTE && p[what].n && (top(what) & UP)) sel = what;
        return;
    }
    if (what != sel && (what < 0 || what >= TAB)) move(sel, what < 0 ? -1 : what - TAB);   /* the waste is never a place: it just cancels */
    sel = -1;
}

static void rect(int x, int y, int w, int h, unsigned c) { jt_fill(&win, x, y, w, h, c); }
static void frame(int x, int y, int w, int h, int t, unsigned c) { rect(x, y, w, t, c); rect(x, y + h - t, w, t, c); rect(x, y, t, h, c); rect(x + w - t, y, t, h, c); }
static int text(const char *s, int x, int y, unsigned fg) { return jt_text_draw(&win, JT_FACE_BODY, x, y, fg, s); }
static int bold(const char *s, int x, int y, unsigned fg) { return jt_text_draw(&win, JT_FACE_BOLD, x, y, fg, s); }
static int colx(int i) { return PAD + i * (CW + GAP); }

/* A suit, centred on cx, cy and about 2r + 1 tall, from discs and rows so it stays a shape at any size. */
static void suit_at(int st, int cx, int cy, int r, unsigned c) {
    int h = r / 2;
    if (st == 2) { for (int dy = -r; dy <= r; dy++) { int w = (r - (dy < 0 ? -dy : dy)) * 3 / 4; rect(cx - w, cy + dy, 2 * w + 1, 1, c); } return; }
    if (st == 1) {   /* heart: two lobes over a point */
        jt_disc(&win, cx - h, cy - h + 1, h, c); jt_disc(&win, cx + h, cy - h + 1, h, c);
        for (int dy = 0; dy <= r; dy++) { int w = r - dy; rect(cx - w, cy - h / 2 + dy, 2 * w + 1, 1, c); }
        return;
    }
    if (st == 0) {   /* spade: a point over two lobes and a stem */
        for (int dy = 0; dy <= r; dy++) { int w = dy; rect(cx - w, cy - r + dy, 2 * w + 1, 1, c); }
        jt_disc(&win, cx - h, cy + h / 2, h, c); jt_disc(&win, cx + h, cy + h / 2, h, c);
    } else {         /* club: three leaves and a stem */
        jt_disc(&win, cx, cy - h - 1, h, c); jt_disc(&win, cx - h - 1, cy + h / 2, h, c); jt_disc(&win, cx + h + 1, cy + h / 2, h, c);
    }
    rect(cx - (r > 5 ? 1 : 0), cy, r > 5 ? 3 : 1, r, c);
    rect(cx - h, cy + r - (r > 5 ? 2 : 1), 2 * h + 1, r > 5 ? 2 : 1, c);
}
static void slot(int x, int y) { frame(x, y, CW, CH, 1, EDGE); }
static void card(int c, int x, int y, int h) {   /* h is how much of the card shows, CH for a whole one */
    if (!(c & UP)) { rect(x, y, CW, h, BACK); if (h > 8) rect(x + 4, y + 4, CW - 8, h - 8, BACK2); frame(x, y, CW, h, 1, EDGE); return; }
    unsigned ink = red(c) ? RED : INK;
    rect(x, y, CW, h, FACE); frame(x, y, CW, h, 1, EDGE);
    static const char *R[13] = {"A", "2", "3", "4", "5", "6", "7", "8", "9", "10", "J", "Q", "K"};
    int x2 = bold(R[rank(c) - 1], x + 5, y + 2, ink);
    suit_at(suit(c), x2 + 7, y + 10, 4, ink);
    if (h >= CH) suit_at(suit(c), x + CW / 2, y + CH / 2 + 6, 10, ink);
}

static void draw(void) {
    rect(0, 0, (int)win.width, (int)win.height, TABLE);
    /* the top row: stock, waste, then the four foundations */
    if (p[STOCK].n) card(p[STOCK].c[p[STOCK].n - 1], colx(0), TOPY, CH); else slot(colx(0), TOPY);
    if (p[WASTE].n) card(top(WASTE), colx(1), TOPY, CH); else slot(colx(1), TOPY);
    if (sel == WASTE) frame(colx(1) - 2, TOPY - 2, CW + 4, CH + 4, 2, SEL);
    for (int s = 0; s < 4; s++) {
        int x = colx(3 + s);
        if (p[FOUND + s].n) card(top(FOUND + s), x, TOPY, CH);
        else { slot(x, TOPY); suit_at(s, x + CW / 2, TOPY + CH / 2, 10, EDGE); }
    }
    /* the columns, fanned to fit the window */
    int avail = (int)win.height - TABY - 8;
    for (int col = 0; col < 7; col++) {
        struct pile *q = &p[TAB + col];
        int x = colx(col), y = TABY, down = 0, ups = 0;
        for (int i = 0; i < q->n; i++) { if (q->c[i] & UP) ups++; else down++; }
        int fan = 16;
        if (ups > 1 && down * 6 + (ups - 1) * fan + CH > avail) fan = (avail - CH - down * 6) / (ups - 1);
        if (fan < 6) fan = 6;
        if (!q->n) slot(x, y);
        int sely = -1;
        for (int i = 0; i < q->n; i++) {
            int last = i == q->n - 1, step = (q->c[i] & UP) ? fan : 6;
            if (sel == TAB + col && sely < 0 && (q->c[i] & UP)) sely = y;
            card(q->c[i], x, y, last ? CH : step);
            if (!last) y += step;
        }
        if (sely >= 0) frame(x - 2, sely - 2, CW + 4, y + CH - sely + 4, 2, SEL);
    }
    /* the side panel */
    int tx = colx(7) + 14, ty = TOPY + 4;
    char num[12];
    bold("Solitaire", tx, ty, INK);
    text(won ? "You won." : sel >= 0 ? "Now pick where it goes." : "Klondike, draw one", tx, ty + 26, won ? 0x002F7B4F : HINT);
    utoa10((unsigned)(p[FOUND].n + p[FOUND + 1].n + p[FOUND + 2].n + p[FOUND + 3].n), num);
    int x2 = text(num, tx, ty + 48, HINT); text(" of 52 up", x2, ty + 48, HINT);
    text("d or space draws", tx, ty + 86, HINT);
    text("w, 1 to 7 picks a pile", tx, ty + 108, HINT);
    text("then 1 to 7 or f moves it", tx, ty + 130, HINT);
    text("a sends cards up", tx, ty + 152, HINT);
    text("r deals again   esc closes", tx, ty + 174, HINT);
}

/* A click is the same as the key for the pile under it. */
static void click(int x, int y) {
    int col = (x - PAD) / (CW + GAP);
    if (x < PAD || col > 6 || x - colx(col) >= CW) return;
    if (y >= TOPY && y < TOPY + CH) {
        if (col == 0) draw_card();
        else if (col == 1) pick(WASTE);
        else if (col >= 3) pick(-1);
    } else if (y >= TABY) pick(TAB + col);
}

__attribute__((section(".text.start"), used))
void _start(int argc, char **argv) {
    (void)argc; (void)argv;
    if (jt_window_open_hires(&win) != 0 || !win.pixels) { say("solitaire: no window\n"); jt_exit(1); }
    {   char line[48]; int l = 0;
        const char *pfx = "solitaire: ring-3 window ";
        while (*pfx) line[l++] = *pfx++;
        l += utoa10(win.width, line + l); line[l++] = 'x';
        l += utoa10(win.height, line + l); line[l++] = '\n';
        jt_write(1, line, (unsigned)l);
    }
    deal();
    draw();
    unsigned flags = JT_POLL_PRESENT;
    for (;;) {
        struct jt_event ev;
        int r = jt_window_poll(&ev, flags);
        if (r == 1 && jt_window_resized(&ev, &win)) { draw(); flags = JT_POLL_PRESENT; continue; }
        flags = 0;
        if (r == 1) {
            if (ev.kind == JT_EV_CLICK) click(ev.a, ev.b);
            else if (ev.kind == JT_EV_KEY) {
                int k = ev.a;
                if (k == '`') { say("solitaire: crashing on purpose\n"); *(volatile unsigned *)0 = 1; }
                if (k == JT_KEY_ESC) break;
                else if (k == 'r') { unsigned t = 0; jt_time(&t); seed ^= t * 2654435761u; if (!seed) seed = 1; deal(); }
                else if (k == 'd' || k == ' ') draw_card();
                else if (k == 'w') pick(WASTE);
                else if (k == 'f') pick(-1);
                else if (k >= '1' && k <= '7') pick(TAB + k - '1');
                else if (k == 'a') { sel = -1; auto_up(); }
            }
            draw(); flags = JT_POLL_PRESENT;
            continue;
        }
        if (r != -11 /* -EAGAIN */) break;
        jt_sched_yield();
    }
    say("solitaire: closed\n");
    jt_exit(0);
}
