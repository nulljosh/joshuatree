/* lexly: learn anything, a four-choice drill, as a real ring-3 program.
 *
 * The seventh app to leave the kernel (roadmap 2.0), done the way
 * user/quotes.c was. Same deck and drill kernel/lexly.h ran in ring 0: a
 * Spanish word, four English choices, pick the right one and the streak
 * grows, miss and it resets. Built with no kernel include path, linked
 * flat, loaded off the VFS by exec_user, and it reaches the machine only
 * through int 0x80: SYS_WINDOW_OPEN for a framebuffer, SYS_WINDOW_POLL for
 * input and the present, SYS_EXIT to leave.
 *
 * Live courses (2.6.27): the course list and a chosen course's questions
 * come from the real Lexly packs through SYS_HTTP_GET (the way
 * user/bookrank.c gets its shelf). The kernel fixes the host; the Worker's
 * /api/lexly turns the packs into plain text: "count" then
 * "id|name|category" rows, and for /api/lexly?c=<id> "count" then
 * "answer index|question|choice|choice|choice|choice" rows. The app opens a
 * course picker, then the drill with a score, and Esc goes back to the
 * picker. When the list cannot be had (no network, an HTTP error, junk, an
 * oversize body, no good row) the app opens straight into the Spanish
 * deck below, as before. The reply is untrusted: every byte is forced to
 * printable ASCII, every copy is bounded by its buffer, a course id must be
 * a plain [a-z0-9_] name, an answer index must be 0 to 3, and a row that is
 * short, empty or has repeated choices is dropped.
 *
 * Type: the antialiased libjt face. The backquote key (`) is the deliberate
 * crash: a write through a null pointer, a page fault at ring 3, reaped by
 * the kernel. tools/checks/ring3lexly-check.py presses it on purpose;
 * tools/checks/ring3lexly-live-check.py proves the live and offline paths.
 */
#include "jtsys.h"
#include "libjt/text.h"

#define BG    0x00FAF8F6 /* GUI_BG */
#define OPT   0x00F1EDE7
#define SEL   0x00E2D8CC
#define RIGHT 0x00CFE8D2
#define MISS  0x00F0D0CC
#define INK   0x001C1C1E
#define HINT  0x0075726E

#define LX_OPT_Y0 124
#define LX_OPT_H  34

typedef struct { const char *spanish; const char *english; } LxWord;
static const LxWord LX_DECK[] = {
    {"agua", "water"},
    {"libro", "book"},
    {"casa", "house"},
    {"gato", "cat"},
    {"perro", "dog"},
    {"sol", "sun"},
    {"luna", "moon"},
    {"estrella", "star"},
    {"mesa", "table"},
    {"puerta", "door"},
    {"ventana", "window"},
    {"arbol", "tree"},
    {"flor", "flower"},
    {"pan", "bread"},
    {"queso", "cheese"},
    {"leche", "milk"},
    {"carne", "meat"},
    {"pollo", "chicken"},
    {"pescado", "fish"},
    {"vino", "wine"},
    {"cerveza", "beer"},
    {"cafe", "coffee"},
    {"te", "tea"},
    {"azucar", "sugar"},
    {"sal", "salt"},
    {"pimienta", "pepper"},
    {"mantequilla", "butter"},
    {"aceite", "oil"},
    {"vinagre", "vinegar"},
    {"manzana", "apple"},
};
#define LX_COUNT ((int)(sizeof(LX_DECK) / sizeof(LX_DECK[0])))

static struct jt_window_info win JT_DATA = {0, 0, 0, 0};
static int lx_round JT_DATA = 0, lx_streak JT_DATA = 0, lx_best JT_DATA = 0, lx_pick JT_DATA = -1;

/* Live state. lx_mode: the picker, the drill, or the score screen. lx_live is 0 for the Spanish deck. */
#define LX_PICK 0
#define LX_DRILL 1
#define LX_DONE 2
#define LX_COURSE_MAX 80
#define LX_Q_MAX 24
#define LX_ITEM_H 26
#define LX_LIST_TOP 40
typedef struct { const char *id, *name, *cat; } LxCourse;
typedef struct { const char *q, *c[4]; int ans; } LxQ;
static char lx_cbody[6144] JT_DATA;                /* the course list reply, parsed in place */
static char lx_qbody[6144] JT_DATA;                /* the questions reply, parsed in place */
static LxCourse lx_courses[LX_COURSE_MAX] JT_DATA;
static LxQ lx_qs[LX_Q_MAX] JT_DATA;
static int lx_ncourses JT_DATA = 0, lx_nq JT_DATA = 0, lx_live JT_DATA = 0, lx_mode JT_DATA = LX_DRILL;
static int lx_sel JT_DATA = 0, lx_top JT_DATA = 0, lx_cur_course JT_DATA = 0, lx_score JT_DATA = 0;
static const char *lx_msg JT_DATA = "";

static int lx_cur(void){ return (lx_round * 7) % LX_COUNT; } /* 7 is coprime with 30, so every word comes up once per lap */
/* Option slot -> deck index. The right answer sits in slot (round % 4);
   the other three are the next deck rows that are not the answer. */
static int lx_option(int slot){
    int right = (lx_round * 3 + lx_cur()) % 4, cur = lx_cur();
    if (slot == right) return cur;
    int n = slot < right ? slot : slot - 1;
    return (cur + 3 + n * 5) % LX_COUNT == cur ? (cur + 1) % LX_COUNT : (cur + 3 + n * 5) % LX_COUNT;
}

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
static int utoa10(unsigned v, char *buf) {
    char tmp[12]; int tn = 0, n = 0;
    do { tmp[tn++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (tn) buf[n++] = tmp[--tn];
    buf[n] = 0;
    return n;
}
/* Append s to out at *l, never past cap-1. */
static void cat(char *out, int *l, int cap, const char *s) { while (*s && *l < cap - 1) out[(*l)++] = *s++; out[*l] = 0; }
static void cat_num(char *out, int *l, int cap, int v) {
    char t[12];
    if (v < 0) { cat(out, l, cap, "-"); v = -v; }
    utoa10((unsigned)v, t); cat(out, l, cap, t);
}
#define NONUM 0x7fffffff /* say(): no number after the prefix */

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
/* Word-boundary wrap by real glyph widths into at most max_lines lines of width w; if text is
   left over the last line ends in an ellipsis. */
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
        if (line == max_lines - 1 && s[n]) { fit(buf, sizeof buf, s, w); text(buf, x, y + line * lh, fg); return; }
        for (int i = 0; i < n; i++) buf[i] = s[i];
        buf[n] = 0;
        text(buf, x, y + line * lh, fg);
        s += n;
    }
}

/* One serial line, one write: what the checks read. Printable ASCII only, bounded. */
static void say(const char *pfx, int a, const char *tail) {
    char line[120]; int l = 0;
    cat(line, &l, 100, pfx);
    if (a != NONUM) cat_num(line, &l, 100, a);
    if (tail) { cat(line, &l, 100, " "); for (; *tail && l < 108; tail++) line[l++] = (*tail >= 0x20 && *tail <= 0x7e) ? *tail : '?'; }
    line[l++] = '\n';
    jt_write(1, line, (unsigned)l);
}

/* ---- the Worker's text, parsed in place ---- */
static int lx_streq(const char *a, const char *b) { while (*a && *a == *b) { a++; b++; } return *a == *b; }
/* Force the body printable (CR to space, anything else odd to '?') and end it at n. */
static void lx_clean(char *body, int n) {
    body[n] = 0;
    for (int i = 0; i < n; i++) {
        unsigned char c = (unsigned char)body[i];
        if (c == '\r') body[i] = ' ';
        else if (c != '\n' && (c < 0x20 || c > 0x7e)) body[i] = '?';
    }
}
/* Line 1 must be digits only; returns the number it holds (clamped), or -1 if the reply is not ours. */
static int lx_head(char **pp) {
    char *p = *pp; int v = 0, k = 0;
    if (*p < '0' || *p > '9') return -1;
    for (; *p && *p != '\n'; p++) { if (*p < '0' || *p > '9') return -1; if (k++ < 5) v = v * 10 + (*p - '0'); }
    if (*p) p++;
    *pp = p;
    return v;
}
/* Split one line at pipes into exactly want fields, in place. Returns the field count, or 0 when
   the line has more pipes than fields. */
static int lx_row(char **pp, char **f, int want) {
    char *p = *pp; int k = 1, extra = 0;
    f[0] = p;
    while (*p && *p != '\n') {
        if (*p == '|') { if (k < want) { *p = 0; f[k++] = p + 1; } else extra = 1; }
        p++;
    }
    if (*p) *p++ = 0;
    *pp = p;
    return extra ? 0 : k;
}
static int lx_id_ok(const char *s) {
    int n = 0;
    for (; s[n]; n++) if (!((s[n] >= 'a' && s[n] <= 'z') || (s[n] >= '0' && s[n] <= '9') || s[n] == '_')) return 0;
    return n >= 1 && n <= 24;
}
/* id|name|category rows. Returns the courses kept; 0 means the reply is unusable. */
static int lx_parse_courses(char *body, int n) {
    lx_clean(body, n);
    char *p = body;
    if (lx_head(&p) < 0) return 0;
    int k = 0;
    while (*p && k < LX_COURSE_MAX) {
        char *f[3];
        if (lx_row(&p, f, 3) != 3 || !lx_id_ok(f[0]) || !f[1][0] || !f[2][0]) continue;
        lx_courses[k].id = f[0]; lx_courses[k].name = f[1]; lx_courses[k].cat = f[2];
        k++;
    }
    return k;
}
/* answer|question|c0|c1|c2|c3 rows. A row needs an answer digit 0 to 3, text in every field and
   four different choices. Returns the questions kept. */
static int lx_parse_questions(char *body, int n) {
    lx_clean(body, n);
    char *p = body;
    if (lx_head(&p) < 0) return 0;
    int k = 0;
    while (*p && k < LX_Q_MAX) {
        char *f[6];
        if (lx_row(&p, f, 6) != 6) continue;
        if (f[0][0] < '0' || f[0][0] > '3' || f[0][1] || !f[1][0]) continue;
        int ok = 1;
        for (int i = 2; i < 6; i++) {
            if (!f[i][0]) ok = 0;
            for (int j = 2; j < i; j++) if (lx_streq(f[i], f[j])) ok = 0;
        }
        if (!ok) continue;
        lx_qs[k].ans = f[0][0] - '0'; lx_qs[k].q = f[1];
        for (int i = 0; i < 4; i++) lx_qs[k].c[i] = f[2 + i];
        k++;
    }
    return k;
}
/* One GET through the kernel; the body only if it is a 200 that fits whole. */
static int lx_get(const char *path, char *buf, int cap) {
    int n = jt_http_get(path, buf, (unsigned)(cap - 1));
    say("lexly: fetch ", n, 0);
    return (n <= 0 || n >= cap - 1) ? 0 : n;
}
static void lx_fetch_courses(void) {
    lx_ncourses = 0;
    int n = lx_get("/api/lexly", lx_cbody, (int)sizeof lx_cbody);
    if (n) lx_ncourses = lx_parse_courses(lx_cbody, n);
}
/* Load the chosen course's questions. 1 on success; otherwise the picker stays. */
static int lx_load_course(int idx) {
    char path[64]; int l = 0;
    cat(path, &l, (int)sizeof path, "/api/lexly?c=");
    cat(path, &l, (int)sizeof path, lx_courses[idx].id);
    lx_nq = 0;
    int n = lx_get(path, lx_qbody, (int)sizeof lx_qbody);
    if (n) lx_nq = lx_parse_questions(lx_qbody, n);
    if (lx_nq < 1) return 0;
    say("lexly: course ", lx_nq, lx_courses[idx].id);
    return 1;
}

/* ---- drawing ---- */
static int lx_right(void) { return lx_live ? lx_qs[lx_round].ans : (lx_round * 3 + lx_cur()) % 4; }

static void lx_draw_pick(void) {
    rect(0, 0, (int)win.width, (int)win.height, BG);
    char hdr[80]; int l = 0;
    cat(hdr, &l, (int)sizeof hdr, lx_msg[0] ? lx_msg : "Pick a course (");
    if (!lx_msg[0]) { cat_num(hdr, &l, (int)sizeof hdr, lx_ncourses); cat(hdr, &l, (int)sizeof hdr, ")"); }
    text(hdr, 20, 12, HINT);
    int vis = ((int)win.height - LX_LIST_TOP - 40) / LX_ITEM_H;
    if (vis < 1) vis = 1;
    if (lx_sel < lx_top) lx_top = lx_sel;
    if (lx_sel >= lx_top + vis) lx_top = lx_sel - vis + 1;
    if (lx_top < 0) lx_top = 0;
    int w = (int)win.width - 40;
    for (int k = 0; k < vis && lx_top + k < lx_ncourses; k++) {
        int i = lx_top + k, y = LX_LIST_TOP + k * LX_ITEM_H;
        unsigned bg = i == lx_sel ? SEL : OPT, fg = i == lx_sel ? INK : HINT;
        rect(20, y, w, LX_ITEM_H - 2, bg);
        char nm[64];
        int cw = jt_text_width(JT_FACE_BODY, lx_courses[i].cat);
        fit(nm, sizeof nm, lx_courses[i].name, w - cw - 40);
        text(nm, 30, y + 5, fg);
        text(lx_courses[i].cat, 20 + w - cw - 10, y + 5, HINT);
    }
    text("up/down or click, enter opens   esc closes", 20, (int)win.height - 30, HINT);
}

static void lx_draw_done(void) {
    rect(0, 0, (int)win.width, (int)win.height, BG);
    char line[96]; int l = 0;
    char nm[64];
    fit(nm, sizeof nm, lx_courses[lx_cur_course].name, (int)win.width - 40);
    text(nm, 20, 40, HINT);
    cat(line, &l, (int)sizeof line, "You got ");
    cat_num(line, &l, (int)sizeof line, lx_score);
    cat(line, &l, (int)sizeof line, " of ");
    cat_num(line, &l, (int)sizeof line, lx_nq);
    text(line, 20, 76, INK);
    text("any key or click for the course list   esc closes", 20, (int)win.height - 30, HINT);
}

static void lx_draw(void) {
    if (lx_mode == LX_PICK) { lx_draw_pick(); return; }
    if (lx_mode == LX_DONE) { lx_draw_done(); return; }
    rect(0, 0, (int)win.width, (int)win.height, BG);
    int right = lx_right(), iw = (int)win.width - 40;
    char buf[96];
    if (lx_live) {
        int l = 0;
        cat(buf, &l, (int)sizeof buf, lx_courses[lx_cur_course].name);
        cat(buf, &l, (int)sizeof buf, "   question ");
        cat_num(buf, &l, (int)sizeof buf, lx_round + 1);
        cat(buf, &l, (int)sizeof buf, " of ");
        cat_num(buf, &l, (int)sizeof buf, lx_nq);
        text(buf, 20, 12, HINT);
        wrap_text(lx_qs[lx_round].q, 20, 40, iw, 3, jt_text_height(JT_FACE_BODY) + 4, INK);
    } else {
        text("Offline, Spanish sample", 20, 12, HINT);
        text("What is this Spanish word?", 20, 40, HINT);
        text(LX_DECK[lx_cur()].spanish, 20, 76, INK);
    }
    for (int s = 0; s < 4; s++) {
        int y = LX_OPT_Y0 + s * LX_OPT_H;
        unsigned bg = OPT;
        if (lx_pick >= 0 && s == right) bg = RIGHT;   /* the answer, shown once you have picked */
        else if (lx_pick == s) bg = MISS;              /* your miss */
        rect(20, y, iw, LX_OPT_H - 6, bg);
        char lab[4] = {(char)('1' + s), ' ', ' ', 0};
        text(lab, 30, y + 6, HINT);
        fit(buf, sizeof buf, lx_live ? lx_qs[lx_round].c[s] : LX_DECK[lx_option(s)].english, iw - 46);
        text(buf, 56, y + 6, INK);
    }
    char line[96]; int l = 0;
    cat(line, &l, (int)sizeof line, "streak ");
    cat_num(line, &l, (int)sizeof line, lx_streak);
    cat(line, &l, (int)sizeof line, "   best ");
    cat_num(line, &l, (int)sizeof line, lx_best);
    cat(line, &l, (int)sizeof line, lx_pick < 0 ? (lx_live ? "   1-4 or click   esc courses" : "   1-4 or click to answer   esc closes")
                                    : (lx_pick == right ? "   correct. any key for next" : "   incorrect. any key for next"));
    text(line, 20, (int)win.height - 30, HINT);
}

/* Announce the question on screen: what the live check reads. */
static void lx_say_question(void) { if (lx_live) say("lexly: q ", lx_round + 1, lx_qs[lx_round].q); }

static void lx_start_course(int idx) {
    lx_cur_course = idx; lx_live = 1; lx_round = 0; lx_score = 0; lx_streak = 0; lx_best = 0; lx_pick = -1;
    lx_mode = LX_DRILL; lx_msg = "";
    lx_say_question();
}
static void lx_to_picker(void) {
    lx_mode = LX_PICK; lx_live = 0; lx_pick = -1; lx_msg = "";
    say("lexly: courses ", lx_ncourses, 0);
}
/* Enter or a click on a course: fetch it behind a "Loading" frame; on a bad reply the picker stays. */
static void lx_open_course(int idx) {
    char hdr[80]; int l = 0;
    cat(hdr, &l, (int)sizeof hdr, "Loading ");
    cat(hdr, &l, (int)sizeof hdr, lx_courses[idx].name);
    lx_msg = hdr; lx_draw();
    struct jt_event pe; jt_window_poll(&pe, JT_POLL_PRESENT);
    if (lx_load_course(idx)) lx_start_course(idx);
    else lx_msg = "Could not load that course. Pick another";
}

__attribute__((section(".text.start"), used))
void _start(int argc, char **argv) {
    (void)argc; (void)argv;
    if (jt_window_open(&win) != 0 || !win.pixels) {
        jt_write(2, "lexly: no window\n", 18);
        jt_exit(1);
    }
    {   /* one line, one write: what tools/checks/ring3quotes-check.py asserts on */
        char line[48]; int l = 0;
        const char *pfx = "lexly: ring-3 window ";
        while (*pfx) line[l++] = *pfx++;
        l += utoa10(win.width, line + l); line[l++] = 'x';
        l += utoa10(win.height, line + l); line[l++] = '\n';
        jt_write(1, line, (unsigned)l);
    }

    lx_round = 0; lx_streak = 0; lx_best = 0; lx_pick = -1; lx_sel = 0; lx_top = 0;
    struct jt_event ev;
    int pending = 0;
    {   /* a first frame before the network round trip, so the window never opens blank */
        rect(0, 0, (int)win.width, (int)win.height, BG);
        text("Loading courses...", 20, 12, HINT);
        pending = jt_window_poll(&ev, JT_POLL_PRESENT) == 1; /* an event this early is kept, not lost */
    }
    lx_fetch_courses();
    if (lx_ncourses > 0) { lx_mode = LX_PICK; say("lexly: courses ", lx_ncourses, 0); }
    else { lx_mode = LX_DRILL; lx_live = 0; say("lexly: samples ", LX_COUNT, 0); }
    lx_draw();

    unsigned flags = JT_POLL_PRESENT;
    for (;;) {
        int r = 1;
        if (pending) { pending = 0; if (jt_window_resized(&ev, &win)) { lx_draw(); flags = JT_POLL_PRESENT; continue; } }
        else {
            r = jt_window_poll(&ev, flags);
            if (r == 1 && jt_window_resized(&ev, &win)) { lx_draw(); flags = JT_POLL_PRESENT; continue; } /* JT_EV_RESIZE: remapped, repaint at the new size */
            flags = 0;
            if (r == -11 /* -EAGAIN */) { jt_sched_yield(); continue; }
            if (r != 1) break;
        }

        if (ev.kind == JT_EV_KEY && ev.a == '`') {
            jt_write(1, "lexly: crashing on purpose\n", 28);
            *(volatile int *)0 = 1;
        }

        if (lx_mode == LX_PICK) {
            int open = -1;
            if (ev.kind == JT_EV_KEY) lx_msg = ""; /* any key clears the "could not load" note */
            if (ev.kind == JT_EV_KEY) {
                if (ev.a == JT_KEY_ESC) break;
                if (ev.a == JT_KEY_UP && lx_sel > 0) lx_sel--;
                else if (ev.a == JT_KEY_DOWN && lx_sel < lx_ncourses - 1) lx_sel++;
                else if (ev.a == JT_KEY_ENTER) open = lx_sel;
                else { flags = JT_POLL_PRESENT; continue; }
                if (open < 0) say("lexly: sel ", lx_sel + 1, lx_courses[lx_sel].name);
            } else if (ev.kind == JT_EV_CLICK) {
                int k = (ev.b - LX_LIST_TOP) / LX_ITEM_H;
                if (ev.b >= LX_LIST_TOP && ev.a >= 20 && ev.a < (int)win.width - 20 && lx_top + k < lx_ncourses
                    && k < ((int)win.height - LX_LIST_TOP - 40) / LX_ITEM_H) { lx_sel = lx_top + k; open = lx_sel; }
                else break; /* the titlebar X, or anywhere off the rows */
            } else { flags = JT_POLL_PRESENT; continue; }
            if (open >= 0) lx_open_course(open);
            lx_draw(); flags = JT_POLL_PRESENT;
            continue;
        }

        if (lx_mode == LX_DONE) {
            if (ev.kind == JT_EV_KEY && ev.a == JT_KEY_ESC) break;
            if (ev.kind == JT_EV_CLICK && (ev.a < 0 || ev.b < 0 || ev.a >= (int)win.width || ev.b >= (int)win.height)) break;
            if (ev.kind != JT_EV_KEY && ev.kind != JT_EV_CLICK) { flags = JT_POLL_PRESENT; continue; }
            lx_to_picker(); lx_draw(); flags = JT_POLL_PRESENT;
            continue;
        }

        int slot = -1;
        if (ev.kind == JT_EV_CLICK) {
            if (ev.b >= LX_OPT_Y0 && ev.b < LX_OPT_Y0 + 4 * LX_OPT_H && ev.a >= 20 && ev.a < (int)win.width - 20)
                slot = (ev.b - LX_OPT_Y0) / LX_OPT_H;
            else
                break; /* the titlebar X or anywhere off the answers, same as every other app */
        } else if (ev.kind == JT_EV_KEY) {
            if (ev.a == JT_KEY_ESC) {
                if (!lx_live) break;
                lx_to_picker(); lx_draw(); flags = JT_POLL_PRESENT;
                continue;
            }
            if (ev.a >= '1' && ev.a <= '4') slot = ev.a - '1';
        } else {
            flags = JT_POLL_PRESENT;
            continue;
        }

        if (lx_pick >= 0) {
            lx_pick = -1; lx_round++;
            say("lexly: next", NONUM, 0); /* the marker the checks read to confirm the round advanced past a picked answer */
            if (lx_live && lx_round >= lx_nq) {
                lx_mode = LX_DONE;
                say("lexly: done ", lx_score, 0);
            } else lx_say_question();
            lx_draw(); flags = JT_POLL_PRESENT;
            continue;
        }
        if (slot < 0) { flags = JT_POLL_PRESENT; continue; }
        lx_pick = slot;
        int right = lx_right();
        if (slot == right) { lx_streak++; lx_score++; if (lx_streak > lx_best) lx_best = lx_streak; }
        else lx_streak = 0;
        say("lexly: pick ", slot + 1, slot == right ? "right" : "miss");
        lx_draw(); flags = JT_POLL_PRESENT;
    }
    jt_write(1, "lexly: closed\n", 15);
    jt_exit(0);
}
