/* curbfind: Craigslist deal rankings near the visitor, as a real ring-3
 * program.
 *
 * The sixteenth app to leave the kernel (roadmap 2.0), done the way
 * user/hikko.c was. Same screen kernel/curbfind.h drew in ring 0: a
 * ranked list on the left sorted by deal score, the selected listing's
 * title, price, neighbourhood, a ten-segment score bar and the reason on
 * the right. Up and down (or a click) select, Esc closes.
 *
 * Live rows come from joshuatree.heyitsmejosh.com/api/deals, the first
 * time a ring-3 program reaches the network, through the one new call this
 * port needed: SYS_HTTP_GET (387, 1.9.11). The kernel fixes the host and
 * port; this program hands it only the path and a buffer. When that fetch
 * fails for any reason (no NIC, no answer, not a 200) the compiled-in
 * Vancouver samples show instead, exactly as before. The reply is parsed
 * in place: one "city" line, then "score|price|hood|title" rows.
 *
 * The p key is the probe tools/checks/ring3curbfind-check.py presses: it
 * hands SYS_HTTP_GET the paths and pointers the kernel must refuse (a
 * relative path, a CR LF that would inject a header, a space, an over-long
 * path, a buffer in kernel memory, a path pointer in kernel memory) and
 * writes each result to serial. Every one of those must be refused before
 * the kernel touches the network, so the numbers are the same with or
 * without a NIC.
 *
 * Glyphs: the kernel's 8x16 VGA fallback font. Every state change writes
 * one serial line, which the check reads.
 */
#include "jtsys.h"
#include "libjt/text.h"

#define BG    0x00FAF8F6 /* GUI_BG */
#define INK   0x001C1C1E
#define HINT  0x0075726E
#define ROWBG 0x00F1EDE7
#define SELBG 0x00E2D8CC
#define BAREMPTY 0x00E8E6E1
#define BARFULL  0x002F7B4F

typedef struct { const char *title, *price, *neighbourhood; int score; const char *reason; } cf_listing_t;

static const cf_listing_t CF_SAMPLES[] = {
    {"Barely used office desk", "$40", "Kitsilano", 9, "Solid construction, minimal wear, great price for IKEA quality."},
    {"Mountain bike, full suspension", "$120", "Commercial Drive", 9, "Recent tune-up, all gears work, pedals and grips included."},
    {"LED monitor 27-inch", "$60", "Downtown", 8, "IPS panel, 60Hz, perfect for work or gaming, tested fully."},
    {"Bookshelf wooden unit", "$35", "Burnaby", 8, "Solid oak, five shelves, heavy but sturdy, matches decor."},
    {"Gaming headset wireless", "$45", "Coquitlam", 7, "Noise cancellation works, battery lasts 20 hours, minor ear pad wear."},
    {"Office chair leather", "$80", "West Vancouver", 7, "Adjustable height and recline, needs minor wheel replacement soon."},
    {"Coffee table glass top", "$50", "Mount Pleasant", 7, "Modern design, one tiny edge chip not visible when placed against wall."},
    {"Acoustic guitar softcase bundle", "$85", "Victoria", 6, "Tuned and ready, strings good, case has one zipper issue."},
    {"Desk lamp LED", "$25", "Maple Ridge", 6, "Bright 5000K color temperature, three brightness levels, USB charging."},
    {"Shelving unit metal", "$30", "Surrey", 5, "Industrial style, sturdy metal frame, some surface rust but structurally sound."},
};
#define CF_SAMPLE_N ((int)(sizeof(CF_SAMPLES) / sizeof(CF_SAMPLES[0])))
#define CF_LIVE_MAX 14
#define CF_PATH     "/api/deals"
#define CF_LIST_X 20
#define CF_LIST_W 220
#define CF_INFO_X 260
#define CF_TOP    64
#define CF_ROW_H  30

static struct jt_window_info win JT_DATA = {0, 0, 0, 0};
static char cf_body[JT_HTTP_BODY_MAX] JT_DATA;      /* the reply, parsed in place */
static char cf_arena[CF_LIVE_MAX][112] JT_DATA;     /* one "score|price|hood|title" row each */
static cf_listing_t cf_live[CF_LIVE_MAX] JT_DATA;
static char cf_city[32] JT_DATA;
static const cf_listing_t *cf_rows JT_DATA = CF_SAMPLES;
static int cf_n JT_DATA = CF_SAMPLE_N;
static int cf_order[CF_LIVE_MAX] JT_DATA;           /* cf_order[0] is the top-scored listing */
static int cf_sel JT_DATA = 0;                      /* selected position in cf_order */

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
/* s cut to maxw px with a trailing "...", drawn; returns the x after it */
static int etext(const char *s, int x, int y, int maxw, unsigned fg) {
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
    return jt_text_draw(&win, JT_FACE_BODY, x, y, fg, t);
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
    while (*pfx && l < 48) line[l++] = *pfx++;
    l += itoa10(a, line + l);
    line[l++] = '\n';
    jt_write(1, line, (unsigned)l);
}

/* Word-wrap into the info column by pixel width, one text line per 20 px. */
static void wrap(const char *s, int x, int y, int max_w, int max_h) {
    char line[96]; int ll = 0, y0 = y;
    if (max_w < 60) return;
    while (*s && y + 18 <= y0 + max_h) {
        int wl = 0;
        while (s[wl] && s[wl] != ' ') wl++;
        char trial[96]; int tl = 0;
        for (int i = 0; i < ll; i++) trial[tl++] = line[i];
        if (ll) trial[tl++] = ' ';
        for (int i = 0; i < wl && tl < 94; i++) trial[tl++] = s[i];
        trial[tl] = 0;
        if (ll && jt_text_width(JT_FACE_BODY, trial) > max_w) {
            line[ll] = 0; text(line, x, y, INK); y += 20; ll = 0;
            continue;
        }
        for (int i = 0; i <= tl; i++) line[i] = trial[i];
        ll = tl; s += wl;
        while (*s == ' ') s++;
    }
    if (ll && y + 18 <= y0 + max_h) { line[ll] = 0; text(line, x, y, INK); }
}

/* Sort indices by score, descending, the same bubble kernel/curbfind.h ran. */
static void cf_init_order(void) {
    for (int i = 0; i < cf_n; i++) cf_order[i] = i;
    for (int i = 0; i < cf_n - 1; i++)
        for (int j = i + 1; j < cf_n; j++)
            if (cf_rows[cf_order[i]].score < cf_rows[cf_order[j]].score) {
                int t = cf_order[i]; cf_order[i] = cf_order[j]; cf_order[j] = t;
            }
}

/* One GET through the kernel; on anything but a 200 with a body that fits,
   the samples stay. The city is the first line, then one row per line. */
static void cf_fetch(void) {
    cf_rows = CF_SAMPLES; cf_n = CF_SAMPLE_N; cf_city[0] = 0;
    int n = jt_http_get(CF_PATH, cf_body, sizeof(cf_body) - 1);
    say("curbfind: fetch ", n);
    if (n <= 0 || n >= (int)sizeof(cf_body) - 1) return;
    cf_body[n] = 0;
    char *p = cf_body; int i = 0, live = 0;
    while (*p && *p != '\n' && i < 31) cf_city[i++] = *p++;
    cf_city[i] = 0;
    if (*p) p++;
    while (*p && live < CF_LIVE_MAX) {
        char *row = cf_arena[live]; int j = 0;
        while (*p && *p != '\n' && j < 110) row[j++] = *p++;
        row[j] = 0;
        while (*p && *p != '\n') p++;
        if (*p) p++;
        char *f[4]; int k = 1; f[0] = row;
        for (char *q = row; *q && k < 4; q++) if (*q == '|') { *q = 0; f[k++] = q + 1; }
        if (k < 4 || !f[3][0]) continue;
        int sc = 0; for (char *q = f[0]; *q >= '0' && *q <= '9'; q++) sc = sc * 10 + (*q - '0');
        cf_live[live].title = f[3]; cf_live[live].price = f[1]; cf_live[live].neighbourhood = f[2];
        cf_live[live].score = sc > 10 ? 10 : sc;
        cf_live[live].reason = "Priced well under the median for its search. Live from Craigslist, ranked by Curbfind's own deal score.";
        live++;
    }
    if (live) { cf_rows = cf_live; cf_n = live; }
    else cf_city[0] = 0;
}

static void cf_draw(void) {
    int w = (int)win.width, h = (int)win.height;
    rect(0, 0, w, h, BG);
    if (cf_city[0]) {
        char hdr[64]; int l = 0; const char *t = "Deals near ";
        while (*t) hdr[l++] = *t++;
        for (const char *c = cf_city; *c && l < 62; c++) hdr[l++] = *c;
        hdr[l] = 0;
        etext(hdr, CF_LIST_X, 40, w - 2 * CF_LIST_X, HINT);
    } else {
        text("Offline, sample Vancouver listings", CF_LIST_X, 40, HINT);
    }

    int room_rows = (h - CF_TOP - 20) / CF_ROW_H;
    int shown = cf_n < room_rows ? cf_n : room_rows;
    for (int i = 0; i < shown; i++) {
        int y = CF_TOP + i * CF_ROW_H;
        const cf_listing_t *l = &cf_rows[cf_order[i]];
        unsigned bg = (i == cf_sel) ? SELBG : ROWBG;
        unsigned fg = (i == cf_sel) ? INK : HINT;
        rect(CF_LIST_X, y, CF_LIST_W, CF_ROW_H - 2, bg);
        char rank[4]; int r = 0, n = i + 1;
        if (n >= 10) rank[r++] = (char)('0' + n / 10);
        rank[r++] = (char)('0' + n % 10); rank[r++] = '.'; rank[r] = 0;
        text(rank, CF_LIST_X + 6, y + 5, fg);
        /* title, ellipsized to the room left of the price */
        int pw = jt_text_width(JT_FACE_BODY, l->price);
        etext(l->title, CF_LIST_X + 30, y + 5, CF_LIST_W - 30 - pw - 20, fg);
        text(l->price, CF_LIST_X + CF_LIST_W - pw - 8, y + 5, fg);
    }

    if (cf_sel < cf_n) {
        const cf_listing_t *l = &cf_rows[cf_order[cf_sel]];
        int x;
        { char tt[72]; int tn = 0; while (l->title[tn] && tn < 64) { tt[tn] = l->title[tn]; tn++; }
          tt[tn] = 0;
          if (jt_text_width(JT_FACE_BOLD, tt) > w - CF_INFO_X - 20) {
              while (tn > 0) {
                  tt[tn] = '.'; tt[tn + 1] = '.'; tt[tn + 2] = '.'; tt[tn + 3] = 0;
                  if (jt_text_width(JT_FACE_BOLD, tt) <= w - CF_INFO_X - 20) break;
                  tt[--tn] = 0;
              }
          }
          jt_text_draw(&win, JT_FACE_BOLD, CF_INFO_X, CF_TOP, INK, tt); }
        x = text(l->price, CF_INFO_X, CF_TOP + 24, HINT);
        x = text(" | ", x, CF_TOP + 24, HINT);
        text(l->neighbourhood, x, CF_TOP + 24, HINT);
        text("Deal score:", CF_INFO_X, CF_TOP + 50, INK);
        int bar_y = CF_TOP + 76, seg = 16, score = l->score;
        rect(CF_INFO_X, bar_y, seg * 10, 8, BAREMPTY);
        if (score > 0 && score <= 10) rect(CF_INFO_X, bar_y, seg * score, 8, BARFULL);
        char ss[8]; int sl = 0;
        if (score >= 10) ss[sl++] = '1';
        ss[sl++] = (char)('0' + score % 10); ss[sl++] = '/'; ss[sl++] = '1'; ss[sl++] = '0'; ss[sl] = 0;
        text(ss, CF_INFO_X + seg * 10 + 8, bar_y - 5, INK);
        wrap(l->reason, CF_INFO_X, CF_TOP + 100, w - CF_INFO_X - 24, h - CF_TOP - 84 - 20);
    }
    etext(cf_city[0] ? "Live Craigslist deals for your area   up/down or click to select   esc closes" : "up/down or click to select   esc closes", CF_LIST_X, h - 24, w - 2 * CF_LIST_X, HINT);
}

/* The probe: every call here must be refused by the kernel's own checks,
   never reach the network, and never write a byte into buf. The check
   asserts each number; -22 is EINVAL, -14 is EFAULT. */
static void cf_probe(void) {
    static char buf[64] JT_DATA;
    static char longpath[JT_HTTP_PATH_MAX + 8] JT_DATA;
    buf[0] = 'x'; buf[1] = 0;
    say("curbfind: probe relative ", jt_http_get("api/deals", buf, sizeof(buf)));
    say("curbfind: probe crlf ", jt_http_get("/api/deals\r\nX-Injected: 1", buf, sizeof(buf)));
    say("curbfind: probe space ", jt_http_get("/api/deals HTTP/1.1", buf, sizeof(buf)));
    say("curbfind: probe empty ", jt_http_get("", buf, sizeof(buf)));
    for (int i = 0; i < JT_HTTP_PATH_MAX + 4; i++) longpath[i] = '/';
    longpath[JT_HTTP_PATH_MAX + 4] = 0;
    say("curbfind: probe long ", jt_http_get(longpath, buf, sizeof(buf)));
    say("curbfind: probe kbuf ", jt_http_get(CF_PATH, (void *)0xC0100000u, 64));  /* kernel text */
    say("curbfind: probe nullbuf ", jt_http_get(CF_PATH, (void *)0, 64));
    say("curbfind: probe kpath ", jt_http_get((const char *)0xC0100000u, buf, sizeof(buf)));
    say("curbfind: probe untouched ", buf[0] == 'x' && buf[1] == 0);
}

/* Returns 0 when the program should close. */
static int cf_handle(const struct jt_event *ev) {
    int old = cf_sel;
    if (ev->kind == JT_EV_CLICK) {
        if (ev->b >= CF_TOP && ev->a >= CF_LIST_X && ev->a < CF_LIST_X + CF_LIST_W) {
            int sel = (ev->b - CF_TOP) / CF_ROW_H;
            if (sel >= cf_n) return 0;
            cf_sel = sel;
        } else return 0; /* the titlebar X, or anywhere off the list */
    } else if (ev->kind == JT_EV_KEY) {
        if (ev->a == JT_KEY_ESC) return 0;
        if (ev->a == '`') { jt_write(1, "curbfind: crashing on purpose\n", 30); *(volatile int *)0 = 1; } /* deliberate crash, as in every ring-3 app */
        if (ev->a == JT_KEY_UP && cf_sel > 0) cf_sel--;
        else if (ev->a == JT_KEY_DOWN && cf_sel < cf_n - 1) cf_sel++;
        else if (ev->a == 'p' || ev->a == 'P') cf_probe();
    }
    if (cf_sel != old) say("curbfind: sel ", cf_sel);
    return 1;
}

__attribute__((section(".text.start"), used))
void _start(int argc, char **argv) {
    (void)argc; (void)argv;
    if (jt_window_open(&win) != 0 || !win.pixels) {
        jt_write(2, "curbfind: no window\n", 20);
        jt_exit(1);
    }
    {   /* one line, one write: what tools/checks/ring3curbfind-check.py asserts on */
        char line[60]; int l = 0;
        const char *pfx = "curbfind: ring-3 window ";
        while (*pfx) line[l++] = *pfx++;
        l += itoa10((int)win.width, line + l); line[l++] = 'x';
        l += itoa10((int)win.height, line + l); line[l++] = '\n';
        jt_write(1, line, (unsigned)l);
    }

    cf_rows = CF_SAMPLES; cf_n = CF_SAMPLE_N; cf_city[0] = 0; cf_sel = 0;
    cf_init_order();
    cf_draw(); /* first frame before the network round trip, so the window never opens blank */
    struct jt_event ev;
    int pending = jt_window_poll(&ev, JT_POLL_PRESENT) == 1; /* presents; a key typed this early is kept, not lost */
    cf_fetch();
    cf_init_order();
    say(cf_city[0] ? "curbfind: live " : "curbfind: samples ", cf_n);
    cf_draw();
    if (pending && !cf_handle(&ev)) goto done;
    say("curbfind: sel ", cf_sel);

    unsigned flags = JT_POLL_PRESENT;
    for (;;) {
        int r = jt_window_poll(&ev, flags);
        if (r == 1 && jt_window_resized(&ev, &win)) { cf_draw(); flags = JT_POLL_PRESENT; continue; } /* JT_EV_RESIZE: remapped, repaint at the new size */
        flags = 0;
        if (r == -11 /* -EAGAIN */) { jt_sched_yield(); continue; }
        if (r != 1) break;
        if (ev.kind != JT_EV_KEY && ev.kind != JT_EV_CLICK) { flags = JT_POLL_PRESENT; continue; }
        if (!cf_handle(&ev)) break;
        cf_draw(); flags = JT_POLL_PRESENT;
    }
done:
    jt_write(1, "curbfind: closed\n", 17);
    jt_exit(0);
}
