/* The Browser window: the text browser of browser.c in the Console's rectangle, like the Clock and Brick. A toolbar (Back,
   Forward, Reload and the address bar), the page as text in Tree Sans with link lines in the accent colour, and one status
   line. It runs the same commands the Terminal does (browse, open N, back, forward, reload) with cmd_out pointed at a
   sink, and reads the page back through browser.c's accessors. Loading blocks, as it does at the Terminal; "loading..."
   is painted first. Every line it logs on the UART starts "browser: " (arm64-browser-app-check.py reads them). */
#include "mascot_art.h"
#include "home.h"
extern void (*cmd_out)(const char *s);                       /* cmd.c */
int browse_command(const char *q, unsigned n);               /* browser.c */
const char *browser_text(unsigned *len);
const char *browser_url(void);
const char *browser_title(void);
int sd_ready(void); int sd_write(const char *name, const void *data, unsigned len); int sd_enter_jt(int make);   /* emmc.c */
int fat_read_file(const char *name, void *buf, unsigned bufsize); int fat_chdir(const char *name); void fat_cwd_set(unsigned int cluster);   /* drivers/fat.c */
const char *browser_link(unsigned n);
unsigned browser_loads(void);
int browser_field_secret(unsigned n);
int browser_field_kind(unsigned n);
unsigned browser_tabs(unsigned *front);
static unsigned br_field;   /* 6.15.0: the form field (its link number) the address bar is editing, 0 when it edits the address */
const unsigned *browser_image(unsigned k, unsigned *w, unsigned *h);   /* 6.14.0: image k's 0RGB pixels and kept size, 0 when it did not load */
#define BR_ROW 16          /* a page line, logical pixels */
#define BR_PAGE_Y 64       /* the page well's top, under the toolbar */
#define BR_VL_MAX 1024     /* wrapped lines kept: 8 KiB of page never needs more than this */
#define BR_ADDR_MAX 200
#define BR_ACCENT 0x00b5502c
static unsigned short br_off[BR_VL_MAX], br_len[BR_VL_MAX];   /* wrapped line i: an offset into the page text and a length */
/* 6.13.0: each wrapped line's style. The low three bits: 0 body text, 1 an h1 or h2 (bold, 20 px, two rows tall), 2 an h3
   to h6 (bold, body size), 3 the second row under a big heading's line, 4 (6.14.0) one BR_ROW slice of an inline image,
   its number and slice in br_off (k * 32 + slice); BR_ST_BOLD when the line starts inside <b>. */
static unsigned char br_sty[BR_VL_MAX];
#define BR_ST_BOLD 8
#define BR_ST_CENTER 16   /* 6.35.0: the line is centred in the well */
static int br_face(unsigned kind, int bold) { return kind == 1 || kind == 2 || bold ? 1 : 2; }   /* text.c: 1 sans bold, 2 sans */
static int br_px(unsigned kind, int s) { return (kind == 1 ? 200 : 120) * s; }
static unsigned br_n, br_lines;                              /* wrapped lines, and the page's own lines (the `page N lines` log) */
static int br_top, br_edit, br_loading;
int br_frame = 1;   /* the window frame needs drawing: set on open, cleared once drawn, so a scroll or a keystroke repaints the well and not the frame */
static unsigned char *br_mascot;   /* the mascot's decoded RGBA, or 0 when it would not decode */
static int br_mascot_tried;
static unsigned br_sel;   /* the link Tab has picked, 1-based; 0 when none */
static char br_addr[BR_ADDR_MAX + 1], br_msg[96], br_err[64], br_ln[96];
static unsigned br_addr_n, br_ln_n;
/* Bookmarks: JT/BOOKMARK.TXT (8.3: BOOKMARKS would not fit), one line a page, the address then a space then the title. Read once, on the
   first open; the star in the address bar adds the open page or takes it off, and the card is written each time. With no page
   open the well lists them: Tab picks, Enter or a click opens. */
#define BM_MAX 12
#define BM_URL 200
#define BM_FILE "BOOKMARK.TXT"
#define BM_ROW 18          /* a list row, logical pixels, from BM_Y0 under the well's top */
#define BM_Y0 30
static char bm_url[BM_MAX][BM_URL + 1], bm_title[BM_MAX][64];
static unsigned bm_n, bm_sel;   /* bookmarks kept, and the one Tab picked (1-based; 0 when none) */
static int bm_loaded;
static unsigned br_slen(const char *s) { unsigned n = 0; while (s[n]) n++; return n; }
static int br_has(const char *s, const char *w) { unsigned k = br_slen(w); for (; *s; s++) { unsigned i = 0; while (i < k && s[i] == w[i]) i++; if (i == k) return 1; } return 0; }
static void br_set(char *d, const char *s, unsigned max) { unsigned n = 0; while (s[n] && n + 1 < max) { d[n] = s[n]; n++; } d[n] = 0; }
static int bm_find(const char *u) { for (unsigned i = 0; i < bm_n; i++) { unsigned k = 0; while (bm_url[i][k] && bm_url[i][k] == u[k]) k++; if (bm_url[i][k] == u[k]) return (int)i; } return -1; }
/* 6.7.0, Blackcomb: Recent, the last pages opened, newest first, in the same one-line-a-page form as the bookmarks. Both
   lists are each person's own, in their home folder, read on every open (a sign-in may have changed who that is). With no
   accounts, or before someone has a list of their own, they are the card's JT/ files. */
#define HS_MAX 8
#define HS_FILE "HISTORY.TXT"
static char hs_url[HS_MAX][BM_URL + 1], hs_title[HS_MAX][64];
static unsigned hs_n;
static char br_list_buf[BM_MAX * (BM_URL + 66)];
static int br_read(const char *name) {   /* the file into br_list_buf: ~/NAME first, else JT/NAME; its length, or <= 0 */
    char dir[HOME_DIR_MAX + 1]; int got = -1;
    if (!sd_ready() || !sd_enter_jt(0)) return -1;
    if (home_active() && home_mine(dir) && home_in(dir, 0)) {
        got = fat_read_file(name, br_list_buf, sizeof br_list_buf - 1);
        fat_cwd_set(0);
        if (got > 0 || !sd_enter_jt(0)) return got;
    }
    got = fat_read_file(name, br_list_buf, sizeof br_list_buf - 1);
    fat_chdir("..");
    return got;
}
static unsigned br_list_parse(int got, char (*url)[BM_URL + 1], char (*title)[64], unsigned max) {
    unsigned k = 0; const char *buf = br_list_buf;
    for (unsigned i = 0, n = got > 0 ? (unsigned)got : 0; i < n && k < max;) {
        unsigned e = i; while (e < n && buf[e] != '\n') e++;
        unsigned u = i; while (u < e && buf[u] != ' ') u++;
        if (u - i > 7 && u - i <= BM_URL && buf[i] == 'h') {
            for (unsigned j = i; j < u; j++) url[k][j - i] = buf[j];
            url[k][u - i] = 0;
            unsigned t = u < e ? u + 1 : e, m = 0; while (t < e && m < 63 && buf[t] != '\r') title[k][m++] = buf[t++];
            title[k][m] = 0; k++;
        }
        i = e + 1;
    }
    return k;
}
static int br_list_save(const char *name, char (*url)[BM_URL + 1], char (*title)[64], unsigned cnt) {   /* 1 when it is on the card */
    unsigned n = 0;
    for (unsigned i = 0; i < cnt; i++) {
        for (const char *p = url[i]; *p; p++) br_list_buf[n++] = *p;
        br_list_buf[n++] = ' ';
        for (const char *p = title[i]; *p; p++) br_list_buf[n++] = *p;
        br_list_buf[n++] = '\n';
    }
    if (!sd_ready()) return 0;
    return home_active() ? home_save(name, br_list_buf, n) == 1 : sd_write(name, br_list_buf, n) == 1;
}
static void bm_load(void) {
    bm_loaded = 1;
    bm_n = br_list_parse(br_read(BM_FILE), bm_url, bm_title, BM_MAX);
    hs_n = br_list_parse(br_read(HS_FILE), hs_url, hs_title, HS_MAX);
    con_quiet = 1; uart_puts("browser: bookmarks loaded "); uart_dec(bm_n); uart_puts(", recent "); uart_dec(hs_n); uart_putc('\n');
}
static int bm_save(void) { return br_list_save(BM_FILE, bm_url, bm_title, bm_n); }
static void hs_add(const char *u, const char *t) {   /* a page that loaded goes to the top of Recent, once */
    unsigned at = 0; while (at < hs_n && !(br_slen(hs_url[at]) == br_slen(u) && br_has(hs_url[at], u))) at++;
    if (at == hs_n && hs_n < HS_MAX) hs_n++;
    if (at >= hs_n) at = hs_n - 1;
    for (unsigned i = at; i > 0; i--) { br_set(hs_url[i], hs_url[i - 1], sizeof hs_url[0]); br_set(hs_title[i], hs_title[i - 1], sizeof hs_title[0]); }
    br_set(hs_url[0], u, sizeof hs_url[0]); br_set(hs_title[0], t, sizeof hs_title[0]);
    br_list_save(HS_FILE, hs_url, hs_title, hs_n);
}
/* The start page (6.7.0, Blackcomb's): Favorites, the bookmarks, then Recent under them, as many rows as the well holds.
   Item k (1-based) runs through both; st_y is its row's top under the well, st_url its address. */
static int br_h(void);
static int st_y(unsigned k) { return k <= bm_n ? BM_Y0 + (int)(k - 1) * BM_ROW : (bm_n ? BM_Y0 + (int)bm_n * BM_ROW + 6 : 6) + 24 + (int)(k - 1 - bm_n) * BM_ROW; }
static unsigned st_count(void) { unsigned k = 0; while (k < bm_n + hs_n && st_y(k + 1) + BM_ROW <= br_h()) k++; return k; }
static const char *st_url(unsigned k) { return k <= bm_n ? bm_url[k - 1] : hs_url[k - 1 - bm_n]; }
static const char *st_title(unsigned k) { return k <= bm_n ? bm_title[k - 1] : hs_title[k - 1 - bm_n]; }
static int br_x(void) { return win_lx + 8; }
static int br_w(void) { return win_lw - 16; }
static int br_h(void) { return win_lh - BR_PAGE_Y - 26; }
static int br_rows(void) { return (br_h() - 8) / BR_ROW; }
static const char *br_wk_t; static unsigned br_wk_len, br_wk_loads; static int br_wk_w = -1;   /* what the wrapped lines were made from: the page text, its length, the load count and the width */
static void br_wrap(void) {   /* the page into lines that fit the well: break at the last space that fits, or inside a word that is too long. Unchanged text and width keep the lines already made: wrapping measures every character, the slow part on a Pi */
    unsigned len; const char *t = browser_text(&len);
    int s = (int)window_scale(), maxw = (br_w() - 12) * s;
    if (br_wk_w == maxw && br_wk_t == t && br_wk_len == len && br_wk_loads == browser_loads()) return;
    br_wk_w = maxw; br_wk_t = t; br_wk_len = len; br_wk_loads = browser_loads();
#ifdef BR_TRACE
    con_quiet = 1; uart_puts("browser: wrap\n");
#endif
    br_n = 0; br_lines = len ? 1 : 0;
    for (unsigned i = 0; i < len; i++) if (t[i] == '\n') br_lines++;
    int center = 0;   /* 6.35.0: runs on across lines, BR_CENTER to BR_LEFT */
    for (unsigned i = 0; len && br_n < BR_VL_MAX;) {
        unsigned e = i; while (e < len && t[e] != '\n') e++;
        unsigned a = i, kind = 0; int bold = 0;
        if (a < e && (t[a] == '\x01' || t[a] == '\x02')) kind = (unsigned)t[a] == 1 ? 1 : 2;
        if (a == e) { br_off[br_n] = (unsigned short)a; br_sty[br_n] = 0; br_len[br_n++] = 0; }
        if (e - a == 2 && t[a] == '\x05') {   /* an image line: as many rows as the picture is tall */
            unsigned k = (unsigned)t[a + 1] - 0x10, iw, ih;
            if (browser_image(k, &iw, &ih)) for (unsigned sl = 0; sl * BR_ROW < ih && sl < 32 && br_n < BR_VL_MAX; sl++) { br_off[br_n] = (unsigned short)(k * 32 + sl); br_sty[br_n] = 4; br_len[br_n++] = 0; }
            a = e;
        }
        while (a < e && br_n < BR_VL_MAX) {
            unsigned b = a, sp = 0; int w = 0, start_bold = bold, b_at_sp = bold;
            for (unsigned k = a; k < e && (unsigned char)t[k] < 0x20; k++) { if (t[k] == '\x06') center = 1; else if (t[k] == '\x07') center = 0; }
            int line_center = center;
            while (b < e && b - a < 127) {
                char c[2] = { t[b], 0 }; int cw = 0;
                if (t[b] == '\x06') center = 1; else if (t[b] == '\x07') center = 0;
                if (t[b] == '\x03') bold = 1; else if (t[b] == '\x04') bold = 0;
                else if ((unsigned char)t[b] >= 0x20 && text_ok) cw = text_width(br_face(kind, bold), c, br_px(kind, s));
                if (w + cw > maxw && b > a) break;
                w += cw; if (t[b] == ' ') { sp = b; b_at_sp = bold; } b++;
            }
            unsigned brk = b < e && sp > a ? sp : b;
            if (brk == sp && b < e) bold = b_at_sp;   /* the next line starts where this one broke, in the bold state of that point */
            br_off[br_n] = (unsigned short)a; br_sty[br_n] = (unsigned char)(kind | (start_bold ? BR_ST_BOLD : 0) | (line_center ? BR_ST_CENTER : 0)); br_len[br_n++] = (unsigned short)(brk - a);
            if (kind == 1 && br_n < BR_VL_MAX) { br_off[br_n] = (unsigned short)brk; br_sty[br_n] = 3; br_len[br_n++] = 0; }
            a = brk; while (a < e && t[a] == ' ') a++;
        }
        if (e >= len) break;
        i = e + 1;
    }
}
static unsigned br_mark(const char *l, unsigned n, unsigned from, unsigned *end) {   /* the next "[N]" at or after from: N, and where it ends; 0 when none */
    for (unsigned i = from; i < n; i++) if (l[i] == '[') {
        unsigned k = i + 1, v = 0;
        while (k < n && l[k] >= '0' && l[k] <= '9' && v < 1000) v = v * 10 + (unsigned)(l[k++] - '0');
        if (k > i + 1 && k < n && l[k] == ']' && v) { *end = k + 1; return v; }
    }
    return 0;
}
static void br_scroll(int top) {   /* clamped: the last page of lines is the furthest down */
    int max = (int)br_n - br_rows(); if (max < 0) max = 0;
    br_top = top < 0 ? 0 : top > max ? max : top;
}
static void br_mascot_draw(int cx, int top) {   /* the mascot, 96 logical px square, centred on cx, top edge at top; 2x2 samples a pixel onto what is there */
    if (!br_mascot_tried) {
        br_mascot_tried = 1; unsigned w, h, ch;
        if (png_decode(mascot_png, MASCOT_PNG_LEN, &br_mascot, &w, &h, &ch) || w != MASCOT_PX || h != MASCOT_PX || ch != 4) br_mascot = 0;
    }
    if (!br_mascot) return;
    int s = (int)window_scale(), D = 96 * s, x0 = (cx - 48) * s, y0 = top * s;
    for (int y = 0; y < D; y++) for (int x = 0; x < D; x++) {
        unsigned r = 0, g = 0, b = 0, a = 0;
        for (int j = 0; j < 2; j++) for (int i = 0; i < 2; i++) {
            const unsigned char *q = br_mascot + (((y * 2 + j) * MASCOT_PX / (D * 2)) * MASCOT_PX + (x * 2 + i) * MASCOT_PX / (D * 2)) * 4;
            r += q[0] * q[3]; g += q[1] * q[3]; b += q[2] * q[3]; a += q[3];
        }
        if (!a) continue;
        unsigned ink = fb_color(((r / a) << 16) | ((g / a) << 8) | (b / a)), al = a / 4;
        unsigned *px = &fb[(unsigned)(y0 + y) * fb_pitch + (unsigned)(x0 + x)], d = *px, out = 0;
        for (int sh = 0; sh <= 16; sh += 8) out |= ((((ink >> sh) & 0xFF) * al + ((d >> sh) & 0xFF) * (255 - al)) / 255) << sh;
        *px = out;
    }
}
void browser_paint(int report) {
    int s = (int)window_scale(), x = br_x(), w = br_w(), y = win_ly + BR_PAGE_Y, h = br_h(), ty = win_ly + 34;
    cur_hide();
#ifdef BR_TRACE
    unsigned long h0 = off_screen_hash(win_lx * s, win_ly * s, win_lw * s, win_lh * s);
#endif
    int off = off_begin(win_lx * s, win_ly * s, win_lw * s, win_lh * s);   /* the whole window is drawn off screen, then copied over in one pass */
    if (br_frame) { gui_draw_window_frame(win_lx, win_ly, win_lw, win_lh, "Blackcomb"); br_frame = 0; }
    else window_rect(win_lx + 8, win_ly + 30, win_lw - 16, win_lh - 34, 0x00F5F0EB);   /* the frame's own body colour, down to 4 px off the edge so the status line's descenders clear too */
    static const char *const lbl[3] = { "<", ">", "R" };
    for (int i = 0; i < 3; i++) {
        int bx = win_lx + 10 + i * 26;
        window_rect(bx, ty, 22, 22, 0x00DDD8CE); window_rect(bx + 1, ty + 1, 20, 20, 0x00FFFFFF);
        gui_text(lbl[i], bx + (22 - gui_text_width(lbl[i])) / 2, ty + 3, 0x001C1C1E);
    }
    int fx = win_lx + 92, fw = win_lw - 102;
    window_rect(fx, ty, fw, 22, br_edit ? BR_ACCENT : 0x00DDD8CE); window_rect(fx + 1, ty + 1, fw - 2, 20, 0x00FFFFFF);
    {   /* the star at the bar's right end: the accent when the open page is a bookmark, a quiet grey when not */
        static const unsigned short star[11] = { 0x020, 0x020, 0x070, 0x070, 0x7FF, 0x3FE, 0x1FC, 0x0F8, 0x1FC, 0x1DC, 0x38E };
        int sx = fx + fw - 20, sy = ty + 5; unsigned saved = browser_url()[0] && bm_find(browser_url()) >= 0;
        for (int r = 0; r < 11; r++) for (int c = 0; c < 11; c++) if (star[r] >> (10 - c) & 1) window_rect(sx + c, sy + r, 1, 1, saved ? BR_ACCENT : 0x00B8B2A8);
    }
    char stars[BR_ADDR_MAX + 1]; unsigned sn = 0;
    if (br_field && browser_field_secret(br_field)) { while (sn < br_addr_n) stars[sn++] = '*'; }
    stars[sn] = 0;
    const char *shown = br_field && browser_field_secret(br_field) ? stars : br_edit ? br_addr : browser_url();
    while (*shown && gui_text_width(shown) > fw - 36) shown++;   /* a long address shows its end */
    gui_text(*shown || (br_edit && !br_field) ? shown : br_field ? "Type the field's text" : "Type an address and press Enter", fx + 6, ty + 3, *shown || br_edit ? 0x001C1C1E : 0x0075726E);
    if (br_edit) window_rect(fx + 7 + gui_text_width(shown), ty + 4, 1, 14, BR_ACCENT);
    window_rect(x - 1, y - 1, w + 2, h + 2, 0x00DDD8CE); window_rect(x, y, w, h, 0x00faf8f4);
    unsigned len; const char *t = browser_text(&len);
    int rows = br_rows();
    for (int r = 0; r < rows && br_top + r < (int)br_n; r++) {
        unsigned vi = (unsigned)(br_top + r), n = br_len[vi], e;
        char buf[128]; for (unsigned k = 0; k < n; k++) buf[k] = t[br_off[vi] + k]; buf[n] = 0;
        unsigned num = br_mark(buf, n, 0, &e);
        int ly = y + 4 + r * BR_ROW;
        for (unsigned f = 0, fe, k; br_sel && (k = br_mark(buf, n, f, &fe)); f = fe) if (k == br_sel) { window_rect(x + 2, ly - 2, w - 4, BR_ROW, 0x00F3E2D8); break; }
        unsigned kind = br_sty[vi] & 7; int bold = (br_sty[vi] & BR_ST_BOLD) != 0;
        if (kind == 3) continue;   /* the lower half of a big heading */
        if (kind == 4) {   /* one slice of an image, each logical pixel an s x s square */
            unsigned k = br_off[vi] >> 5, sl = br_off[vi] & 31, iw, ih;
            const unsigned *px = browser_image(k, &iw, &ih);
            for (unsigned yy = sl * BR_ROW; px && yy < ih && yy < (sl + 1) * BR_ROW; yy++)
                for (unsigned xx = 0; xx < iw && (int)xx < w - 12; xx++) {
                    unsigned c = fb_color(px[yy * iw + xx]);
                    for (int dy = 0; dy < s; dy++) for (int dx = 0; dx < s; dx++)
                        fb[(unsigned)((ly + (int)(yy - sl * BR_ROW)) * s + dy) * fb_pitch + (unsigned)((x + 6 + (int)xx) * s + dx)] = c;
                }
            if (report && px && !sl) { con_quiet = 1; uart_puts("browser: shows image "); uart_dec(k + 1); uart_puts(" at "); uart_dec((unsigned)(x + 6) * (unsigned)s); uart_putc(','); uart_dec((unsigned)ly * (unsigned)s); uart_putc('\n'); }
            continue;
        }
        {   /* runs of one face: marks switch bold, the heading's own face and size cover the whole line */
            unsigned fg = fb_color(num ? BR_ACCENT : 0x001C1C1E);
            int px = (x + 6) * s, base = kind == 1 ? (ly + 24) * s : ly * s + 25 * s / 2;   /* gui_text's baseline; a big heading's sits 24 down its two rows */
            char run[128]; unsigned rn = 0;
            if ((br_sty[vi] & BR_ST_CENTER) && text_ok) {   /* 6.35.0: measure the runs first, then start half the room in */
                int lw = 0, b2 = bold;
                for (unsigned k = 0; k <= n; k++) {
                    unsigned char c = k < n ? (unsigned char)buf[k] : 0;
                    if (c >= 0x20) { run[rn++] = (char)c; continue; }
                    if (rn) { run[rn] = 0; lw += text_width(br_face(kind, b2), run, br_px(kind, s)); rn = 0; }
                    if (c == 3) b2 = 1; else if (c == 4) b2 = 0;
                }
                int room = (w - 12) * s - lw;
                if (room > 0) px += room / 2;
                if (report) { con_quiet = 1; uart_puts("browser: shows centred at "); uart_dec((unsigned)px); uart_putc(' '); uart_puts(buf[0] >= 0x20 ? buf : buf + 1); uart_putc('\n'); }
            }
            for (unsigned k = 0; k <= n; k++) {
                unsigned char c = k < n ? (unsigned char)buf[k] : 0;
                if (c >= 0x20) { run[rn++] = (char)c; continue; }
                if (rn) {
                    run[rn] = 0;
                    if (text_ok) px = text_draw(br_face(kind, bold), run, px, base, br_px(kind, s), fg, fb, fb_pitch, (int)fb_w, (int)fb_h);
                    if (report && (kind || bold)) { con_quiet = 1; uart_puts(kind == 1 ? "browser: shows heading big " : kind == 2 ? "browser: shows heading small " : "browser: shows bold "); uart_puts(run); uart_putc('\n'); }
                    rn = 0;
                }
                if (c == 3) bold = 1; else if (c == 4) bold = 0;
            }
        }
        if (report && num) { con_quiet = 1; uart_puts("browser: shows link "); uart_dec(num); uart_puts(" at "); uart_dec((unsigned)(x + 16) * (unsigned)s); uart_putc(','); uart_dec((unsigned)(ly + 8) * (unsigned)s); uart_putc('\n'); }
    }
    if (!len && browser_url()[0]) {   /* 6.8.0: a page came back with no words: its text is built by script, which Blackcomb does not run */
        const char *l1 = "This page is built by JavaScript,", *l2 = "which Blackcomb does not run yet.";
        int top = y + (h - 40) / 2;
        gui_text(l1, x + (w - gui_text_width(l1)) / 2, top, 0x0075726E);
        gui_text(l2, x + (w - gui_text_width(l2)) / 2, top + 20, 0x0075726E);
    } else if (!len) {   /* an empty well: the start page or the mascot */
        const char *none = "Nothing to show yet";
        int top = y + (h - 96 - 28) / 2;
        unsigned items = st_count();
        if (items) {   /* the start page: saved and recent pages stand in for the mascot */
            if (bm_n) gui_text("Favorites", x + 8, y + 6, 0x0075726E);
            if (items > bm_n) gui_text("Recent", x + 8, y + st_y(bm_n + 1) - 24, 0x0075726E);
            for (unsigned k = 1; k <= items; k++) {
                int ly = y + st_y(k);
                if (bm_sel == k) window_rect(x + 2, ly - 2, w - 4, BM_ROW, 0x00F3E2D8);
                char nm[64]; br_set(nm, st_title(k)[0] ? st_title(k) : st_url(k), sizeof nm);
                for (unsigned c = br_slen(nm); c && gui_text_width(nm) > w - 24; ) nm[--c] = 0;
                gui_text(nm, x + 8, ly, 0x001C1C1E);
                if (report) {
                    con_quiet = 1; uart_puts(k <= bm_n ? "browser: shows bookmark " : "browser: shows recent ");
                    uart_dec(k <= bm_n ? k : k - bm_n); uart_putc(' '); uart_puts(st_url(k)); uart_putc('\n');
                }
            }
        } else {
            br_mascot_draw(x + w / 2, top);
            gui_text(none, x + (w - gui_text_width(none)) / 2, top + 96 + 10, 0x0075726E);
        }
    }
    char st[BR_ADDR_MAX + 48]; unsigned o = 0;
    const char *left = br_loading ? "loading..." : br_msg[0] ? br_msg : browser_url();
    for (; left[o] && o < BR_ADDR_MAX; o++) st[o] = left[o];
    st[o] = 0;
    char right[24]; unsigned rn = 0;
    {   /* 6.16.0: "tab 2 of 3" ahead of the line count once there is more than one tab */
        unsigned front, nt = browser_tabs(&front);
        if (nt > 1) { const char *a = "tab "; for (; *a; a++) right[rn++] = *a; right[rn++] = (char)('0' + front); for (a = " of "; *a; a++) right[rn++] = *a; right[rn++] = (char)('0' + nt); if (len) { right[rn++] = ','; right[rn++] = ' '; } }
    }
    if (!br_loading && !br_msg[0] && len) {
        const char *a = "line "; for (; *a; a++) right[rn++] = *a;
        unsigned v = (unsigned)br_top + 1; char d[10]; unsigned z = 0; do d[z++] = (char)('0' + v % 10); while (v /= 10); while (z) right[rn++] = d[--z];
        for (a = " of "; *a; a++) right[rn++] = *a;
        v = br_n; do d[z++] = (char)('0' + v % 10); while (v /= 10); while (z) right[rn++] = d[--z];
    }
    right[rn] = 0;
    int rw = gui_text_width(right);
    while (o && gui_text_width(st) > w - rw - 24) st[--o] = 0;
    gui_text(st, x + 2, win_ly + win_lh - 22, 0x0075726E);
    gui_text(right, x + w - rw - 2, win_ly + win_lh - 22, 0x0075726E);
#ifdef BR_TRACE   /* arm64-browser-flicker-check.py: until the copy, the real screen must hold exactly what it held before the paint */
    unsigned long h1 = off_screen_hash(win_lx * s, win_ly * s, win_lw * s, win_lh * s);
    con_quiet = 1; uart_puts(off ? "browser: paint offscreen, screen changed before the copy: " : "browser: paint direct, screen changed before the copy: ");
    uart_dec(h0 != h1); uart_putc('\n');
#endif
    if (off) off_end(); else fb_flush(win_lx * s, win_ly * s, win_lw * s, win_lh * s);
    cur_show();
}
static void br_sink(const char *s) {   /* browse_command's output: lines for the UART, and the last "browser: " one is the reason a load failed */
    for (; *s; s++) {
        if (*s != '\n') { if (br_ln_n + 1 < sizeof br_ln) br_ln[br_ln_n++] = *s; continue; }
        br_ln[br_ln_n] = 0; br_ln_n = 0;
        if (br_ln[0] == 'b' && br_has(br_ln, "browser: ")) br_set(br_err, br_ln + 9, sizeof br_err);
        con_quiet = 1; uart_puts(br_ln); uart_putc('\n');
    }
}
static void br_run(const char *cmd) {   /* one browse_command line, with "loading..." up while it blocks */
    unsigned before = browser_loads();
    br_msg[0] = 0; br_err[0] = 0; br_edit = 0; br_field = 0; br_loading = 1;
    browser_paint(0);
    void (*old)(const char *) = cmd_out;
    cmd_out = br_sink; br_ln_n = 0;
    browse_command(cmd, br_slen(cmd));
    cmd_out = old; br_loading = 0;
    if (browser_loads() != before) {
        br_top = 0; br_sel = 0; br_wrap();
        if (browser_url()[0] && br_lines) hs_add(browser_url(), browser_title());   /* 6.7.0: Recent; 6.8.0: not a page with no words */
        if (!br_lines && browser_url()[0]) { con_quiet = 1; uart_puts("browser: needs javascript\n"); }
        con_quiet = 1; uart_puts("browser: page "); uart_dec(br_lines); uart_puts(br_lines == 1 ? " line\n" : " lines\n");
        browser_paint(1);
        return;
    }
    br_set(br_msg, br_has(br_err, "saved ") || br_has(br_err, "not saved") || br_has(br_err, "could not save") ? br_err : br_has(br_err, "no network") ? "no network" : br_has(br_err, "bad url") ? "that is not a web address" :
                   br_has(br_err, "nothing behind") ? "nothing behind" : br_has(br_err, "nothing ahead") ? "nothing ahead" :
                   "the page did not load", sizeof br_msg);
    if (br_err[0] && br_has(br_msg, "did not load")) {   /* say why: the stage that stopped, from the last "browser: " line */
        unsigned m = br_slen(br_msg); br_msg[m++] = ':'; br_msg[m++] = ' '; br_set(br_msg + m, br_err, sizeof br_msg - m);
    }
    con_quiet = 1; uart_puts("browser: status "); uart_puts(br_msg); uart_putc('\n');
    browser_paint(0);
}
/* 6.7.0, as Blackcomb does: what is typed is an address when it has a scheme, a host when it has a dot and no space (https:// goes
   in front), and otherwise a search, on DuckDuckGo's page that needs no JavaScript. */
#define BR_SEARCH "https://html.duckduckgo.com/html/?q="
static void br_load(void) {   /* Enter in the address bar */
    if (br_field) {   /* 6.15.0: the bar was a form field: fill N TEXT, the page stays */
        char cmd[BR_ADDR_MAX + 16] = "fill "; unsigned n = 5; char d[5]; unsigned z = 0, v = br_field;
        do d[z++] = (char)('0' + v % 10); while (v /= 10); while (z) cmd[n++] = d[--z];
        cmd[n++] = ' ';
        for (unsigned i = 0; i < br_addr_n && n < sizeof cmd - 1; i++) cmd[n++] = br_addr[i];
        cmd[n] = 0;
        void (*old)(const char *) = cmd_out; cmd_out = br_sink; browse_command(cmd, n); cmd_out = old;
        con_quiet = 1; uart_puts("browser: filled field "); uart_dec(br_field); uart_putc('\n');
        br_set(br_msg, "field set: Tab to the button and press Enter", sizeof br_msg);
        br_field = 0; br_edit = 0; br_addr_n = 0; br_addr[0] = 0; br_wk_w = -1; br_wrap(); browser_paint(0);   /* 6.31.0: the page shows it */
        return;
    }
    if (!br_addr_n) return;
    char cmd[260]; unsigned n = 0;
    int scheme = br_has(br_addr, "://") || (br_addr_n >= 5 && br_addr[0] == 'm' && br_addr[1] == 'a' && br_addr[2] == 'i' && br_addr[3] == 'l' && br_addr[4] == ':'), space = br_has(br_addr, " "), dot = br_has(br_addr, ".");   /* 6.29.0: mail: is an address */
    for (const char *p = "browse "; *p; p++) cmd[n++] = *p;
    if (!scheme && dot && !space) for (const char *p = "https://"; *p; p++) cmd[n++] = *p;
    if (scheme || (dot && !space)) for (unsigned i = 0; i < br_addr_n && n < sizeof cmd - 1; i++) cmd[n++] = br_addr[i];
    else {
        static const char hex[] = "0123456789ABCDEF";
        for (const char *p = BR_SEARCH; *p; p++) cmd[n++] = *p;
        for (unsigned i = 0; i < br_addr_n && n < sizeof cmd - 4; i++) {
            unsigned char c = (unsigned char)br_addr[i];
            if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '.' || c == '_' || c == '~') cmd[n++] = (char)c;
            else if (c == ' ') cmd[n++] = '+';
            else { cmd[n++] = '%'; cmd[n++] = hex[c >> 4]; cmd[n++] = hex[c & 15]; }
        }
        con_quiet = 1; uart_puts("browser: search "); uart_puts(br_addr); uart_putc('\n');
    }
    cmd[n] = 0;
    con_quiet = 1; uart_puts("browser: load "); uart_puts(cmd + 7); uart_putc('\n');
    br_addr_n = 0; br_addr[0] = 0;
    br_run(cmd);
}
static void br_nav(int what) {   /* 0 back, 1 forward, 2 reload */
    static const char *const word[3] = { "back", "forward", "reload" };
    con_quiet = 1; uart_puts("browser: "); uart_puts(word[what]); uart_putc('\n');
    br_run(word[what]);
}
static void br_follow(unsigned n) {   /* link n, as a click on it would */
    const char *l = browser_link(n);
    if (browser_field_kind(n) >= 3) {   /* 6.28.0: a checkbox, radio or select changes in place; the status line says how */
        char cmd[16] = "open "; unsigned o = 5; char d[5]; unsigned z = 0, v = n;
        do d[z++] = (char)('0' + v % 10); while (v /= 10); while (z) cmd[o++] = d[--z];
        cmd[o] = 0;
        void (*old)(const char *) = cmd_out; cmd_out = br_sink; br_err[0] = 0; browse_command(cmd, o); cmd_out = old;
        br_set(br_msg, br_err, sizeof br_msg);
        br_wk_w = -1; br_wrap();   /* 6.31.0: the page text changed in place: [x], (o) or the picked option */
        browser_paint(0);
        return;
    }
    if (l && l[0] == 'f' && l[1] == 'i' && l[5] == ':') {   /* a form field: the address bar becomes its editor */
        br_field = n; br_edit = 1; br_addr_n = 0; br_addr[0] = 0;
        br_set(br_msg, "type, then press Enter", sizeof br_msg);
        con_quiet = 1; uart_puts("browser: editing field "); uart_dec(n); uart_putc('\n');
        browser_paint(0);
        return;
    }
    char cmd[16] = "open "; unsigned o = 5; char d[5]; unsigned z = 0, v = n;
    do d[z++] = (char)('0' + v % 10); while (v /= 10); while (z) cmd[o++] = d[--z];
    cmd[o] = 0;
    con_quiet = 1; uart_puts("browser: link "); uart_dec(n); uart_putc('\n');
    br_run(cmd);
}
static void br_star(void) {   /* the star: the open page joins the list, or leaves it when it is on it */
    const char *u = browser_url(); int at = u[0] ? bm_find(u) : -1; unsigned un = br_slen(u);
    con_quiet = 1;
    if (!u[0]) br_set(br_msg, "open a page to bookmark it", sizeof br_msg);
    else if (at >= 0) {
        for (unsigned i = (unsigned)at + 1; i < bm_n; i++) { br_set(bm_url[i - 1], bm_url[i], sizeof bm_url[0]); br_set(bm_title[i - 1], bm_title[i], sizeof bm_title[0]); }
        bm_n--; bm_sel = 0;
        br_set(br_msg, bm_save() ? "bookmark removed" : "bookmark removed (no card to save it)", sizeof br_msg);
        uart_puts("browser: star removed "); uart_puts(u); uart_putc('\n');
    } else if (bm_n >= BM_MAX) br_set(br_msg, "bookmarks full, remove one first", sizeof br_msg);
    else if (un > BM_URL) br_set(br_msg, "that address is too long to bookmark", sizeof br_msg);
    else {
        unsigned o = 0; for (; u[o]; o++) bm_url[bm_n][o] = u[o] == ' ' ? '+' : u[o];
        bm_url[bm_n][o] = 0; br_set(bm_title[bm_n], browser_title(), sizeof bm_title[0]); bm_n++;
        br_set(br_msg, bm_save() ? "bookmarked" : "bookmarked (no card to save it)", sizeof br_msg);
        uart_puts("browser: star saved "); uart_puts(u); uart_putc('\n');
    }
    browser_paint(0);
}
static int br_empty(void) { unsigned len; browser_text(&len); return !len && !browser_url()[0]; }   /* no page open: the start page */
static void br_bookmark(unsigned n) {   /* bookmark n (1-based) opens as a typed address would */
    if (!n || n > st_count()) return;
    char cmd[BM_URL + 8]; unsigned o = 0;
    for (const char *p = "browse "; *p; p++) cmd[o++] = *p;
    for (const char *p = st_url(n); *p; p++) cmd[o++] = *p;
    cmd[o] = 0; bm_sel = 0;
    con_quiet = 1; uart_puts("browser: bookmark "); uart_dec(n); uart_putc('\n');
    br_run(cmd);
}
static void br_tab(void) {
    if (br_empty()) {   /* no page: Tab walks the saved ones */
        if (!st_count()) return;
        bm_sel = bm_sel % st_count() + 1; br_edit = 0; br_addr_n = 0; br_addr[0] = 0; br_msg[0] = 0;
        con_quiet = 1; uart_puts("browser: selected bookmark "); uart_dec(bm_sel); uart_putc('\n');
        browser_paint(0); return;
    }   /* Tab: the next link, wrapping, scrolled into view */
    unsigned want = br_sel + 1;
    if (!browser_link(want)) want = 1;
    if (!browser_link(want)) return;
    br_sel = want; br_edit = 0; br_field = 0; br_addr_n = 0; br_addr[0] = 0; br_msg[0] = 0;
    unsigned len; const char *t = browser_text(&len);
    for (unsigned vi = 0; vi < br_n; vi++) {
        char buf[128]; unsigned n = br_len[vi], e;
        for (unsigned k = 0; k < n; k++) buf[k] = t[br_off[vi] + k];
        buf[n] = 0;
        for (unsigned f = 0, k; (k = br_mark(buf, n, f, &e)); f = e) if (k == want) {
            if ((int)vi < br_top || (int)vi >= br_top + br_rows()) br_scroll((int)vi - br_rows() / 2);
            con_quiet = 1; uart_puts("browser: selected link "); uart_dec(want); uart_putc('\n');
            browser_paint(0); return;
        }
    }
    browser_paint(0);
}
void browser_open(void) {
    if (browser_live || !fb) return;
    settings_close(); clock_close(); brick_close(); calc_close(); app_kill("closed for the Browser"); console_close();
    browser_live = 1; br_frame = 1; br_sel = 0; br_edit = 1; br_addr_n = 0; br_addr[0] = 0; br_msg[0] = 0; br_top = 0;
    br_wrap(); bm_sel = 0; bm_load();   /* 6.7.0: every open, so the lists are the signed-in person's */
    browser_paint(1); uart_puts("browser open\n");
}
void browser_go(const char *url) {   /* 6.32.0: open Blackcomb on a page, as typing it in the address bar would */
    browser_open();
    if (!browser_live) return;
    char cmd[BR_ADDR_MAX + 8] = "browse "; unsigned n = 7;
    for (const char *p = url; *p && n < sizeof cmd - 1; p++) cmd[n++] = *p;
    cmd[n] = 0;
    con_quiet = 1; uart_puts("browser: load "); uart_puts(url); uart_putc('\n');
    br_run(cmd);
}
void browser_close(void) {
    if (!browser_live) return;
    browser_live = br_edit = 0;
    con_live = 1; console_close(); pane_open(cp);
    uart_puts("browser closed\n");
}
int browser_key(unsigned code) {   /* a key down with the Browser in front and Spotlight closed; always the Browser's */
    if (alt_held && (code == 105 || code == 106)) { br_nav(code == 106); return 1; }          /* Alt+Left, Alt+Right */
    if (code == 63 || (ctrl_held && code == 19)) { br_nav(2); return 1; }                      /* F5, Ctrl+R */
    if (ctrl_held && code == 32) { br_star(); return 1; }                                      /* Ctrl+D, the star */
    if (ctrl_held && code == 38) { br_field = 0; br_edit = 1; br_addr_n = 0; br_addr[0] = 0; browser_paint(0); return 1; }   /* Ctrl+L */
    int page = br_rows() > 1 ? br_rows() - 1 : 1;
    if (code == 1) { if (br_edit) { br_field = 0; br_edit = 0; br_addr_n = 0; br_addr[0] = 0; browser_paint(0); } else browser_close(); return 1; }
    if (code == 104 || code == 109 || code == 102 || code == 107 || code == 103 || code == 108) {
        br_scroll(code == 104 ? br_top - page : code == 109 ? br_top + page : code == 102 ? 0 : code == 107 ? (int)br_n : code == 103 ? br_top - 1 : br_top + 1);
        br_msg[0] = 0; browser_paint(0); return 1;
    }
    if ((ctrl_held || gui_held) && (code == 46 || code == 45 || code == 47 || code == 30)) {   /* Cmd or Ctrl with C X V A on the address bar; the whole address is what copy takes, so A has nothing to select */
        if (code == 46 || code == 45) { const char *a = br_edit ? br_addr : browser_url(); unsigned n = 0; while (a[n]) n++; clip_put(a, n); if (code == 45 && br_edit) { br_addr_n = 0; br_addr[0] = 0; browser_paint(0); } }
        else if (code == 47) {
            char b[BR_ADDR_MAX + 1]; unsigned n = clip_take(b, BR_ADDR_MAX);
            if (!br_edit) { br_edit = 1; br_addr_n = 0; br_addr[0] = 0; }
            for (unsigned i = 0; i < n && br_addr_n < BR_ADDR_MAX; i++) if (b[i] > ' ' && b[i] < 127) br_addr[br_addr_n++] = b[i];
            br_addr[br_addr_n] = 0; browser_paint(0);
        }
        return 1;
    }
    {   /* 6.16.0: tabs, as a Mac browser: Cmd+T a new one, Cmd+W (desk.c hands it over) closes one, Ctrl+Tab the next, Cmd+1..4 */
        unsigned front, n = browser_tabs(&front);
        if (gui_held && code == 20) { br_run("tab new"); br_edit = 1; browser_paint(0); return 1; }
        if (gui_held && code == 17) { br_run("tab close"); return 1; }
        if (ctrl_held && code == 15) { char c[8] = "tab 1"; c[4] = (char)('0' + front % n + 1); br_run(c); return 1; }
        if (gui_held && code >= 2 && code <= 1 + n) { char c[8] = "tab 1"; c[4] = (char)('0' + code - 1); br_run(c); return 1; }
    }
    if (ctrl_held || gui_held || alt_held) return 1;
    if (code == 15 && !(br_edit && br_addr_n)) { br_tab(); return 1; }
    if (!br_edit && bm_sel && br_empty() && (code == 28 || code == 96)) { br_bookmark(bm_sel); return 1; }
    if (!br_edit && br_sel && (code == 28 || code == 96)) { br_follow(br_sel); return 1; }
    if (br_edit && (code == 28 || code == 96)) { br_load(); return 1; }
    if (br_edit && code == 14) { if (br_addr_n) br_addr[--br_addr_n] = 0; browser_paint(0); return 1; }
    int ch = ask_char(code);
    if (ch > ' ' || (br_edit && ch == ' ')) {   /* a letter with the page showing starts a new address, as in a Mac browser's Cmd+L */
        if (!br_edit) { br_edit = 1; br_addr_n = 0; bm_sel = 0; }
        if (ch < 127 && br_addr_n < BR_ADDR_MAX) { br_addr[br_addr_n++] = (char)ch; br_addr[br_addr_n] = 0; }
        browser_paint(0);
    }
    return 1;
}
void browser_wheel(int v) {   /* REL_WHEEL: up is positive */
    if (!browser_live || spot_live || !v) return;
    int was = br_top;
    br_scroll(br_top - v * 3);
    if (br_top == was) return;   /* already at that end of the page: nothing moved, so nothing to draw */
    br_msg[0] = 0; browser_paint(0);
}
void browser_click(int lx, int ly) {
    int ty = win_ly + 34;
    if (ly >= ty && ly < ty + 22) {
        for (int i = 0; i < 3; i++) if (lx >= win_lx + 10 + i * 26 && lx < win_lx + 32 + i * 26) { br_nav(i); return; }
        if (lx >= win_lx + win_lw - 36 && lx < win_lx + win_lw - 10) { br_star(); return; }   /* the star */
        if (lx >= win_lx + 92 && lx < win_lx + win_lw - 10) { br_field = 0; br_edit = 1; br_addr_n = 0; br_addr[0] = 0; browser_paint(0); }
        return;
    }
    if (br_empty() && st_count() && lx >= br_x() && lx < br_x() + br_w()) {   /* a start-page row opens its page */
        for (unsigned k = 1; k <= st_count(); k++) {
            int top = win_ly + BR_PAGE_Y + st_y(k) - 2;
            if (ly >= top && ly < top + BM_ROW) { br_bookmark(k); return; }
        }
    }
    int y = win_ly + BR_PAGE_Y + 4;
    if (lx < br_x() || lx >= br_x() + br_w() || ly < y) return;
    unsigned vi = (unsigned)br_top + (unsigned)((ly - y) / BR_ROW);
    if (vi >= br_n || (ly - y) / BR_ROW >= br_rows()) return;
    unsigned len; const char *t = browser_text(&len);
    char buf[128]; unsigned n = br_len[vi];
    for (unsigned k = 0; k < n; k++) buf[k] = t[br_off[vi] + k];
    buf[n] = 0;
    int s = (int)window_scale(), pick = 0;
    unsigned from = 0, e;
    for (unsigned num; (num = br_mark(buf, n, from, &e)); from = e) {   /* a link's text ends at its "[N]": the click picks the first one whose end is right of it */
        pick = (int)num;
        char sv = buf[e]; buf[e] = 0;
        int right = text_ok ? text_width(2, buf, 120 * s) / s : 0;
        buf[e] = sv;
        if (lx < br_x() + 6 + right) break;
    }
    if (!pick) return;
    br_follow((unsigned)pick);
}
