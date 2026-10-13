/* The Browser window: the text browser of browser.c in the Console's rectangle, like the Clock and Brick. A toolbar (Back,
   Forward, Reload and the address bar), the page as text in Tree Sans with link lines in the accent colour, and one status
   line. It runs the same commands the Terminal does (browse, open N, back, forward, reload) with cmd_out pointed at a
   sink, and reads the page back through browser.c's accessors. Loading blocks, as it does at the Terminal; "loading..."
   is painted first. Every line it logs on the UART starts "browser: " (arm64-browser-app-check.py reads them). */
#include "mascot_art.h"
extern void (*cmd_out)(const char *s);                       /* cmd.c */
int browse_command(const char *q, unsigned n);               /* browser.c */
const char *browser_text(unsigned *len);
const char *browser_url(void);
const char *browser_link(unsigned n);
unsigned browser_loads(void);
#define BR_ROW 16          /* a page line, logical pixels */
#define BR_PAGE_Y 64       /* the page well's top, under the toolbar */
#define BR_VL_MAX 1024     /* wrapped lines kept: 8 KiB of page never needs more than this */
#define BR_ADDR_MAX 200
#define BR_ACCENT 0x00b5502c
static unsigned short br_off[BR_VL_MAX], br_len[BR_VL_MAX];   /* wrapped line i: an offset into the page text and a length */
static unsigned br_n, br_lines;                              /* wrapped lines, and the page's own lines (the `page N lines` log) */
static int br_top, br_edit, br_loading;
static int br_frame = 1;   /* the window frame needs drawing: set on open, cleared once drawn, so a scroll or a keystroke repaints the well and not the frame */
static unsigned char *br_mascot;   /* the mascot's decoded RGBA, or 0 when it would not decode */
static int br_mascot_tried;
static unsigned br_sel;   /* the link Tab has picked, 1-based; 0 when none */
static char br_addr[BR_ADDR_MAX + 1], br_msg[96], br_err[64], br_ln[96];
static unsigned br_addr_n, br_ln_n;
static unsigned br_slen(const char *s) { unsigned n = 0; while (s[n]) n++; return n; }
static int br_has(const char *s, const char *w) { unsigned k = br_slen(w); for (; *s; s++) { unsigned i = 0; while (i < k && s[i] == w[i]) i++; if (i == k) return 1; } return 0; }
static void br_set(char *d, const char *s, unsigned max) { unsigned n = 0; while (s[n] && n + 1 < max) { d[n] = s[n]; n++; } d[n] = 0; }
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
    for (unsigned i = 0; len && br_n < BR_VL_MAX;) {
        unsigned e = i; while (e < len && t[e] != '\n') e++;
        unsigned a = i;
        if (a == e) { br_off[br_n] = (unsigned short)a; br_len[br_n++] = 0; }
        while (a < e && br_n < BR_VL_MAX) {
            unsigned b = a, sp = 0; int w = 0;
            while (b < e && b - a < 127) {
                char c[2] = { t[b], 0 }; int cw = text_ok ? text_width(2, c, 120 * s) : 0;
                if (w + cw > maxw && b > a) break;
                w += cw; if (t[b] == ' ') sp = b; b++;
            }
            unsigned brk = b < e && sp > a ? sp : b;
            br_off[br_n] = (unsigned short)a; br_len[br_n++] = (unsigned short)(brk - a);
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
static void browser_paint(int report) {
    int s = (int)window_scale(), x = br_x(), w = br_w(), y = win_ly + BR_PAGE_Y, h = br_h(), ty = win_ly + 34;
    cur_hide();
#ifdef BR_TRACE
    unsigned long h0 = off_screen_hash(win_lx * s, win_ly * s, win_lw * s, win_lh * s);
#endif
    int off = off_begin(win_lx * s, win_ly * s, win_lw * s, win_lh * s);   /* the whole window is drawn off screen, then copied over in one pass */
    if (br_frame) { gui_draw_window_frame(win_lx, win_ly, win_lw, win_lh, "Browser"); br_frame = 0; }
    else window_rect(win_lx + 8, win_ly + 30, win_lw - 16, win_lh - 34, 0x00F5F0EB);   /* the frame's own body colour, down to 4 px off the edge so the status line's descenders clear too */
    static const char *const lbl[3] = { "<", ">", "R" };
    for (int i = 0; i < 3; i++) {
        int bx = win_lx + 10 + i * 26;
        window_rect(bx, ty, 22, 22, 0x00DDD8CE); window_rect(bx + 1, ty + 1, 20, 20, 0x00FFFFFF);
        gui_text(lbl[i], bx + (22 - gui_text_width(lbl[i])) / 2, ty + 3, 0x001C1C1E);
    }
    int fx = win_lx + 92, fw = win_lw - 102;
    window_rect(fx, ty, fw, 22, br_edit ? BR_ACCENT : 0x00DDD8CE); window_rect(fx + 1, ty + 1, fw - 2, 20, 0x00FFFFFF);
    const char *shown = br_edit ? br_addr : browser_url();
    while (*shown && gui_text_width(shown) > fw - 16) shown++;   /* a long address shows its end */
    gui_text(*shown || br_edit ? shown : "Type an address and press Enter", fx + 6, ty + 3, *shown || br_edit ? 0x001C1C1E : 0x0075726E);
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
        gui_text(buf, x + 6, ly, num ? BR_ACCENT : 0x001C1C1E);
        if (report && num) { con_quiet = 1; uart_puts("browser: shows link "); uart_dec(num); uart_puts(" at "); uart_dec((unsigned)(x + 16) * (unsigned)s); uart_putc(','); uart_dec((unsigned)(ly + 8) * (unsigned)s); uart_putc('\n'); }
    }
    if (!len) {   /* an empty well: the mascot, and the words under it */
        const char *none = "Nothing to show yet";
        int top = y + (h - 96 - 28) / 2;
        br_mascot_draw(x + w / 2, top);
        gui_text(none, x + (w - gui_text_width(none)) / 2, top + 96 + 10, 0x0075726E);
    }
    char st[BR_ADDR_MAX + 48]; unsigned o = 0;
    const char *left = br_loading ? "loading..." : br_msg[0] ? br_msg : browser_url();
    for (; left[o] && o < BR_ADDR_MAX; o++) st[o] = left[o];
    st[o] = 0;
    char right[24]; unsigned rn = 0;
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
    br_msg[0] = 0; br_err[0] = 0; br_edit = 0; br_loading = 1;
    browser_paint(0);
    void (*old)(const char *) = cmd_out;
    cmd_out = br_sink; br_ln_n = 0;
    browse_command(cmd, br_slen(cmd));
    cmd_out = old; br_loading = 0;
    if (browser_loads() != before) {
        br_top = 0; br_sel = 0; br_wrap();
        con_quiet = 1; uart_puts("browser: page "); uart_dec(br_lines); uart_puts(br_lines == 1 ? " line\n" : " lines\n");
        browser_paint(1);
        return;
    }
    br_set(br_msg, br_has(br_err, "no network") ? "no network" : br_has(br_err, "bad url") ? "that is not a web address" :
                   br_has(br_err, "nothing behind") ? "nothing behind" : br_has(br_err, "nothing ahead") ? "nothing ahead" :
                   "the page did not load", sizeof br_msg);
    if (br_err[0] && br_has(br_msg, "did not load")) {   /* say why: the stage that stopped, from the last "browser: " line */
        unsigned m = br_slen(br_msg); br_msg[m++] = ':'; br_msg[m++] = ' '; br_set(br_msg + m, br_err, sizeof br_msg - m);
    }
    con_quiet = 1; uart_puts("browser: status "); uart_puts(br_msg); uart_putc('\n');
    browser_paint(0);
}
static void br_load(void) {   /* Enter in the address bar */
    if (!br_addr_n) return;
    char cmd[BR_ADDR_MAX + 8]; unsigned n = 0;
    for (const char *p = "browse "; *p; p++) cmd[n++] = *p;
    for (unsigned i = 0; i < br_addr_n; i++) cmd[n++] = br_addr[i];
    cmd[n] = 0;
    con_quiet = 1; uart_puts("browser: load "); uart_puts(br_addr); uart_putc('\n');
    br_addr_n = 0; br_addr[0] = 0;
    br_run(cmd);
}
static void br_nav(int what) {   /* 0 back, 1 forward, 2 reload */
    static const char *const word[3] = { "back", "forward", "reload" };
    con_quiet = 1; uart_puts("browser: "); uart_puts(word[what]); uart_putc('\n');
    br_run(word[what]);
}
static void br_follow(unsigned n) {   /* link n, as a click on it would */
    char cmd[16] = "open "; unsigned o = 5; char d[5]; unsigned z = 0, v = n;
    do d[z++] = (char)('0' + v % 10); while (v /= 10); while (z) cmd[o++] = d[--z];
    cmd[o] = 0;
    con_quiet = 1; uart_puts("browser: link "); uart_dec(n); uart_putc('\n');
    br_run(cmd);
}
static void br_tab(void) {   /* Tab: the next link, wrapping, scrolled into view */
    unsigned want = br_sel + 1;
    if (!browser_link(want)) want = 1;
    if (!browser_link(want)) return;
    br_sel = want; br_edit = 0; br_addr_n = 0; br_addr[0] = 0; br_msg[0] = 0;
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
static void browser_open(void) {
    if (browser_live || !fb) return;
    settings_close(); clock_close(); brick_close(); calc_close(); app_kill("closed for the Browser"); console_close();
    browser_live = 1; br_frame = 1; br_sel = 0; br_edit = 1; br_addr_n = 0; br_addr[0] = 0; br_msg[0] = 0; br_top = 0;
    br_wrap();
    browser_paint(0); uart_puts("browser open\n");
}
static void browser_close(void) {
    if (!browser_live) return;
    browser_live = br_edit = 0;
    con_live = 1; console_close(); pane_open(cp);
    uart_puts("browser closed\n");
}
static int browser_key(unsigned code) {   /* a key down with the Browser in front and Spotlight closed; always the Browser's */
    if (alt_held && (code == 105 || code == 106)) { br_nav(code == 106); return 1; }          /* Alt+Left, Alt+Right */
    if (code == 63 || (ctrl_held && code == 19)) { br_nav(2); return 1; }                      /* F5, Ctrl+R */
    if (ctrl_held && code == 38) { br_edit = 1; br_addr_n = 0; br_addr[0] = 0; browser_paint(0); return 1; }   /* Ctrl+L */
    int page = br_rows() > 1 ? br_rows() - 1 : 1;
    if (code == 1) { if (br_edit) { br_edit = 0; br_addr_n = 0; br_addr[0] = 0; browser_paint(0); } else browser_close(); return 1; }
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
    if (ctrl_held || gui_held || alt_held) return 1;
    if (code == 15 && !(br_edit && br_addr_n)) { br_tab(); return 1; }
    if (!br_edit && br_sel && (code == 28 || code == 96)) { br_follow(br_sel); return 1; }
    if (br_edit && (code == 28 || code == 96)) { br_load(); return 1; }
    if (br_edit && code == 14) { if (br_addr_n) br_addr[--br_addr_n] = 0; browser_paint(0); return 1; }
    int ch = ask_char(code);
    if (ch > ' ' || (br_edit && ch == ' ')) {   /* a letter with the page showing starts a new address, as in a Mac browser's Cmd+L */
        if (!br_edit) { br_edit = 1; br_addr_n = 0; }
        if (ch < 127 && br_addr_n < BR_ADDR_MAX) { br_addr[br_addr_n++] = (char)ch; br_addr[br_addr_n] = 0; }
        browser_paint(0);
    }
    return 1;
}
static void browser_wheel(int v) {   /* REL_WHEEL: up is positive */
    if (!browser_live || spot_live || !v) return;
    int was = br_top;
    br_scroll(br_top - v * 3);
    if (br_top == was) return;   /* already at that end of the page: nothing moved, so nothing to draw */
    br_msg[0] = 0; browser_paint(0);
}
static void browser_click(int lx, int ly) {
    int ty = win_ly + 34;
    if (ly >= ty && ly < ty + 22) {
        for (int i = 0; i < 3; i++) if (lx >= win_lx + 10 + i * 26 && lx < win_lx + 32 + i * 26) { br_nav(i); return; }
        if (lx >= win_lx + 92 && lx < win_lx + win_lw - 10) { br_edit = 1; br_addr_n = 0; br_addr[0] = 0; browser_paint(0); }
        return;
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
