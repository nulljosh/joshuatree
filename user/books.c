/* books: a plain-text reader as a ring-3 program (user/books.c), same skeleton as user/notes.c.

   Library: every .TXT in BOOKS/ (on the card that is JT/BOOKS/). When BOOKS/ is missing the
   library is the Files root instead. Up and down pick a book, Enter opens it, Esc closes the app.

   Reader: one page of word-wrapped text in the body face, at a time. Right or Space goes to the
   next page, Left back one, Esc returns to the library. Every page turn saves the page number to
   BOOKS/<NAME>.POS as one decimal number, and opening the book reads it back. The kernel does not
   send PageUp or PageDown to a ring-3 app yet, so paging is Right, Space and Left.

   A book is read whole into the heap with SYS_READFILE (SYS_OPEN stops at 8 KB and SYS_READ moves
   255 bytes a call). Over 3 MB it is refused with a message, the way Music refuses a song that big.
   Line breaks become spaces and any byte the body face cannot draw becomes '?'.

   Serial markers: books: books=N, books: open FILE page N, books: page N, books: back,
   books: too big FILE, books: closed. */
#include "jtsys.h"
#include "libjt/text.h"
#include "libjt/stdlib.h"
#ifdef __aarch64__
#include "libjt/mascot.h"   /* the Pi build draws the mascot on the empty library */
#endif

#define BG    0x00FAF8F6
#define INK   0x001C1C1E
#define DIM   0x0075726E
#define FADE  0x00A29A90
#define SEL   0x00EDE6DC

#define MAX_BOOKS 32
#define MAX_PAGES 16384
#define MAX_BYTES (3u << 20)   /* a bigger book is refused, not truncated */
#define MARGIN 56
#define HEAD_Y 22
#define BODY_Y 60
#define LINE_GAP 6
#define LIST_Y 64
#define ROW_H 26
#define FOOT_H 44
#define WORD_MAX 80            /* one word, at most this many bytes (a longer one is split) */
#define LINE_MAX 200           /* one wrapped line, at most this many bytes */

struct book { char file[13]; char title[40]; unsigned size; };

static struct jt_window_info win JT_DATA = {0, 0, 0, 0};
static struct jt_dirent ents[JT_READDIR_MAX] JT_DATA;
static struct book bk[MAX_BOOKS] JT_DATA;
static int nbk JT_DATA = 0, sel JT_DATA = 0, top JT_DATA = 0;   /* top: the first row on screen */
static int in_books JT_DATA = 0;     /* 1 when the library is BOOKS/, 0 for the Files root */
static int reading JT_DATA = 0;
static char *text JT_DATA = 0;       /* the open book, whole, on the heap */
static unsigned tlen JT_DATA = 0;
static char cur_file[13] JT_DATA = {0};
static char cur_title[40] JT_DATA = {0};
static unsigned *pages JT_DATA = 0;  /* byte offset where each page starts, on the heap */
static int np JT_DATA = 0;           /* page starts known so far */
static int cur JT_DATA = 0;          /* the page on screen, 0 based */
static int lines_pp JT_DATA = 1, body_w JT_DATA = 120, pitch JT_DATA = 20, space_w JT_DATA = 4;
static char msg[80] JT_DATA = {0};

static void rect(int x, int y, int w, int h, unsigned c) {
    int W = (int)win.width, H = (int)win.height;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > W) w = W - x;
    if (y + h > H) h = H - y;
    for (int yy = 0; yy < h; yy++) {
        unsigned *row = win.pixels + (unsigned)(y + yy) * win.width + (unsigned)x;
        for (int xx = 0; xx < w; xx++) row[xx] = c;
    }
}
static int slen(const char *s) { int n = 0; while (s[n]) n++; return n; }
static void scopy(char *d, const char *s, int max) {
    int i = 0;
    while (s[i] && i < max - 1) { d[i] = s[i]; i++; }
    d[i] = 0;
}

/* Decimal digits of v into b (no terminator); returns how many. */
static int udec(unsigned v, char *b) {
    char d[12]; int nd = 0;
    if (!v) d[nd++] = '0';
    while (v) { d[nd++] = (char)('0' + v % 10); v /= 10; }
    int n = nd;
    while (nd) *b++ = d[--nd];
    return n;
}

/* A serial line built in one write, so the kernel's log keeps it on one line. */
struct sb { char b[120]; int l; };
static void sb_s(struct sb *o, const char *s) { while (*s && o->l < 110) o->b[o->l++] = *s++; }
static void sb_n(struct sb *o, unsigned v) { o->l += udec(v, o->b + o->l); }
static void sb_out(struct sb *o) { o->b[o->l++] = '\n'; jt_write(1, o->b, (unsigned)o->l); }

/* Paths. A book is BOOKS/NAME.TXT (or NAME.TXT in the root); its place is BOOKS/NAME.POS. */
static void book_path(char *out, const char *file) {
    int l = 0;
    if (in_books) { const char *d = "BOOKS/"; while (*d) out[l++] = *d++; }
    for (int i = 0; file[i] && l < 36; i++) out[l++] = file[i];
    out[l] = 0;
}
static void pos_path(char *out, const char *file) {
    int l = 0;
    if (in_books) { const char *d = "BOOKS/"; while (*d) out[l++] = *d++; }
    for (int i = 0; file[i] && file[i] != '.' && l < 36; i++) out[l++] = file[i];
    const char *x = ".POS";
    while (*x) out[l++] = *x++;
    out[l] = 0;
}

static int is_txt(const char *n) {
    int l = slen(n);
    if (l < 5) return 0;
    return n[l - 4] == '.' && (n[l - 3] | 32) == 't' && (n[l - 2] | 32) == 'x' && (n[l - 1] | 32) == 't';
}

/* The library: BOOKS/ when it exists (even empty), else the Files root. */
static void scan(void) {
    int n = jt_readdir("BOOKS", ents, JT_READDIR_MAX);
    in_books = n >= 0;
    if (n < 0) n = jt_readdir(".", ents, JT_READDIR_MAX);
    if (n < 0) n = 0;
    if (n > JT_READDIR_MAX) n = JT_READDIR_MAX;
    nbk = 0;
    for (int i = 0; i < n && nbk < MAX_BOOKS; i++) {
        if (ents[i].is_dir || !is_txt(ents[i].name)) continue;
        struct book *b = &bk[nbk++];
        scopy(b->file, ents[i].name, sizeof b->file);
        int k = 0;
        while (b->file[k] && b->file[k] != '.' && k < 39) { b->title[k] = b->file[k]; k++; }
        b->title[k] = 0;
        b->size = ents[i].size;
    }
    sel = 0; top = 0;
}

/* The page metrics for the window as it is now. */
static void layout(void) {
    pitch = jt_text_height(JT_FACE_BODY) + LINE_GAP;
    space_w = jt_text_width(JT_FACE_BODY, " ");
    body_w = (int)win.width - 2 * MARGIN;
    if (body_w < 120) body_w = 120;
    lines_pp = ((int)win.height - BODY_Y - FOOT_H) / pitch;
    if (lines_pp < 1) lines_pp = 1;
}

/* One wrapped line from s (left bytes of text). Breaks at spaces, like jt_wrap, never wider
   than body_w. Copies the line into out (single spaces between words) and returns how many
   bytes of s it used, spaces included, so the next line starts there. */
static unsigned line_at(const char *s, unsigned left, char *out, unsigned *n) {
    unsigned p = 0, ol = 0;
    int width = 0;
    while (p < left && s[p] == ' ') p++;
    while (p < left) {
        unsigned w = p;
        while (w < left && s[w] != ' ' && w - p < WORD_MAX) w++;
        char word[WORD_MAX + 1];
        unsigned wl = w - p;
        for (unsigned k = 0; k < wl; k++) word[k] = s[p + k];
        word[wl] = 0;
        int ww = jt_text_width(JT_FACE_BODY, word);
        int add = ol ? space_w + ww : ww;
        if (ol && (width + add > body_w || ol + 1 + wl > LINE_MAX)) break;
        if (ol) out[ol++] = ' ';
        for (unsigned k = 0; k < wl; k++) out[ol++] = word[k];
        width += add;
        p = w;
        while (p < left && s[p] == ' ') p++;
    }
    out[ol] = 0;
    *n = ol;
    return p;
}

/* Lays one page from byte off. Draws it when draw is set. Returns where the next page starts. */
static unsigned page_from(unsigned off, int draw) {
    char line[LINE_MAX + 1];
    int y = BODY_Y;
    for (int k = 0; k < lines_pp && off < tlen; k++) {
        unsigned n = 0;
        off += line_at(text + off, tlen - off, line, &n);
        if (draw && n) jt_text_draw(&win, JT_FACE_BODY, MARGIN, y, INK, line);
        y += pitch;
    }
    return off;
}

/* Is there a page after cur? The start of the next page is worked out the first time it is asked. */
static int have_next(void) {
    if (cur + 1 < np) return 1;
    if (np >= MAX_PAGES) return 0;
    unsigned nx = page_from(pages[np - 1], 0);
    while (nx < tlen && text[nx] == ' ') nx++;
    if (nx >= tlen) return 0;
    pages[np++] = nx;
    return 1;
}

/* Walks from page 0 to target, or to the last page when the book is shorter. */
static void go_to(int target) {
    np = 1; pages[0] = 0; cur = 0;
    while (cur < target && have_next()) cur++;
}

static void save_pos(void) {
    char p[40], b[12];
    pos_path(p, cur_file);
    int l = udec((unsigned)cur + 1, b);
    int fd = jt_open(p, JT_O_WRONLY | JT_O_CREAT | JT_O_TRUNC);
    if (fd < 0) return;
    jt_write(fd, b, (unsigned)l);
    jt_close(fd);
}

/* The page number saved for file, 1 based; 0 when there is none. */
static int read_pos(const char *file) {
    char p[40], b[16];
    pos_path(p, file);
    int n = jt_readfile(p, b, sizeof b - 1);
    if (n <= 0) return 0;
    int v = 0;
    for (int i = 0; i < n && b[i] >= '0' && b[i] <= '9' && v < 100000; i++) v = v * 10 + (b[i] - '0');
    return v;
}

/* Every byte the body face cannot draw is made plain: breaks and tabs become spaces, the rest
   of the non-ASCII bytes become '?'. */
static void clean(void) {
    for (unsigned i = 0; i < tlen; i++) {
        unsigned char c = (unsigned char)text[i];
        if (c < 0x20) text[i] = ' ';
        else if (c >= 0x7F) text[i] = '?';
    }
}

static void turn(int d) {
    if (d > 0) { if (!have_next()) return; cur++; }
    else { if (cur == 0) return; cur--; }
    save_pos();
    struct sb o; o.l = 0;
    sb_s(&o, "books: page "); sb_n(&o, (unsigned)cur + 1); sb_out(&o);
}

static void open_book(int i) {
    struct book *b = &bk[i];
    struct sb o; o.l = 0;
    if (b->size > MAX_BYTES) {
        scopy(msg, b->file, 40);
        int l = slen(msg);
        const char *m = " is over 3 MB, Books cannot open it";
        for (int k = 0; m[k] && l < 79; k++) msg[l++] = m[k];
        msg[l] = 0;
        sb_s(&o, "books: too big "); sb_s(&o, b->file); sb_out(&o);
        return;
    }
    if (b->size == 0) { scopy(msg, "That book is empty.", sizeof msg); return; }
    if (!pages) pages = (unsigned *)malloc(MAX_PAGES * sizeof(unsigned));
    if (text) { free(text); text = 0; }
    char *buf = pages ? (char *)malloc(b->size + 1) : 0;
    if (!buf) { scopy(msg, "Books ran out of memory.", sizeof msg); return; }
    char p[40];
    book_path(p, b->file);
    int n = jt_readfile(p, buf, b->size);
    if (n <= 0) { free(buf); scopy(msg, "Books could not read that file.", sizeof msg); return; }
    text = buf;
    tlen = (unsigned)n;
    clean();
    scopy(cur_file, b->file, sizeof cur_file);
    scopy(cur_title, b->title, sizeof cur_title);
    layout();
    int want = read_pos(b->file);
    if (want < 1) want = 1;
    msg[0] = 0;
    reading = 1;
    go_to(want - 1);
    sb_s(&o, "books: open "); sb_s(&o, b->file); sb_s(&o, " page "); sb_n(&o, (unsigned)cur + 1); sb_out(&o);
}

static int rows_visible(void) {
    int v = ((int)win.height - LIST_Y - FOOT_H) / ROW_H;
    return v < 1 ? 1 : v;
}
static void keep_sel_visible(void) {
    int v = rows_visible();
    if (sel < top) top = sel;
    if (sel >= top + v) top = sel - v + 1;
    if (top < 0) top = 0;
}

static void draw_library(void) {
    int W = (int)win.width, H = (int)win.height;
    rect(0, 0, W, H, BG);
    jt_text_draw(&win, JT_FACE_BOLD, MARGIN, HEAD_Y, INK, "Books");
    if (nbk == 0) {
#ifdef __aarch64__
        const char *cap = "No books yet", *hint = "Put a .TXT file in JT/BOOKS.";
        jt_mascot_draw(&win, W / 2, LIST_Y, 96);
        jt_text_draw(&win, JT_FACE_BODY, (W - jt_text_width(JT_FACE_BODY, cap)) / 2, LIST_Y + 108, INK, cap);
        jt_text_draw(&win, JT_FACE_BODY, (W - jt_text_width(JT_FACE_BODY, hint)) / 2, LIST_Y + 134, DIM, hint);
#else
        jt_text_draw(&win, JT_FACE_BODY, MARGIN, LIST_Y, DIM, "No books yet. Put a .TXT file in JT/BOOKS.");
#endif
    }
    int v = rows_visible();
    for (int i = top; i < nbk && i < top + v; i++) {
        int y = LIST_Y + (i - top) * ROW_H;
        if (i == sel) rect(MARGIN - 12, y - 4, W - 2 * (MARGIN - 12), ROW_H, SEL);
        jt_text_draw(&win, JT_FACE_BODY, MARGIN, y, INK, bk[i].title);
        char sz[16];
        int l = udec((bk[i].size + 1023) / 1024, sz);
        const char *k = " KB";
        while (*k) sz[l++] = *k++;
        sz[l] = 0;
        jt_text_draw(&win, JT_FACE_BODY, W - MARGIN - 90, y, DIM, sz);
    }
    jt_text_draw(&win, JT_FACE_BODY, MARGIN, H - 30, msg[0] ? INK : DIM,
                 msg[0] ? msg : "Enter opens   Esc closes");
}

static void draw_reader(void) {
    int W = (int)win.width, H = (int)win.height;
    rect(0, 0, W, H, BG);
    jt_text_draw(&win, JT_FACE_BOLD, MARGIN, HEAD_Y, INK, cur_title);
    char pg[24];
    const char *pre = "page ";
    int l = 0;
    while (*pre) pg[l++] = *pre++;
    l += udec((unsigned)cur + 1, pg + l);
    pg[l] = 0;
    jt_text_draw(&win, JT_FACE_BODY, W - MARGIN - 120, HEAD_Y + 4, DIM, pg);
    page_from(pages[cur], 1);
    jt_text_draw(&win, JT_FACE_BODY, MARGIN, H - 30, FADE, "Right or Space: next    Left: back    Esc: library");
}

static void draw(void) { if (reading) draw_reader(); else draw_library(); }

static void say_fail(const char *s) { struct sb o; o.l = 0; sb_s(&o, s); sb_out(&o); }

/* Returns 1 when the key closes the app. */
static int handle_key(int k) {
    msg[0] = 0;
    if (k == '`') { say_fail("books: crashing on purpose"); *(volatile unsigned *)0 = 1; }
    if (reading) {
        if (k == JT_KEY_ESC) { reading = 0; say_fail("books: back"); }
        else if (k == JT_KEY_RIGHT || k == ' ') turn(1);
        else if (k == JT_KEY_LEFT) turn(-1);
        return 0;
    }
    if (k == JT_KEY_ESC) return 1;
    if (nbk == 0) return 0;
    if (k == JT_KEY_UP && sel > 0) sel--;
    else if (k == JT_KEY_DOWN && sel < nbk - 1) sel++;
    else if (k == JT_KEY_ENTER) open_book(sel);
    keep_sel_visible();
    return 0;
}

void _start(int argc, char **argv) {
    (void)argc; (void)argv;
    if (jt_window_open(&win) != 0 || !win.pixels) { say_fail("books: no window"); jt_exit(1); }
    layout();
    scan();
    struct sb o; o.l = 0;
    sb_s(&o, "books: books="); sb_n(&o, (unsigned)nbk); sb_out(&o);
    draw();
    unsigned flags = JT_POLL_PRESENT;
    for (;;) {
        struct jt_event ev;
        int r = jt_window_poll(&ev, flags);
        if (r == 1 && jt_window_resized(&ev, &win)) {
            int keep = cur;
            layout();
            if (reading) go_to(keep);
            draw(); flags = JT_POLL_PRESENT; continue;
        }
        flags = 0;
        if (r == 1) {
            if (ev.kind == JT_EV_KEY && handle_key(ev.a)) break;
            draw(); flags = JT_POLL_PRESENT;
            continue;
        }
        if (r != -11 /* -EAGAIN */) break;
        jt_sched_yield();
    }
    say_fail("books: closed");
    jt_exit(0);
}
