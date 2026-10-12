/* The Nimble window: Joshua's answer engine (nimble.jaybulb.com) in the Console's rectangle, like the Browser and the Clock.
   A search field at the top (always ready for typing, Enter asks), the answer wrapped in Tree Sans in the well, and under it
   the numbered sources, pinned to the bottom so a long answer can never push them off the window. A click on a source, or
   Tab to pick one and Enter, opens it in the Browser. Asking blocks the way a Browser load does ("asking..." is painted
   first); nimble.c does the request and keeps the answer, this file reads it back through nimble_answer and nimble_source_*.
   Clear states, each in plain words in the well and on the status line: nothing asked yet, asking, no network, and the
   service's own reason when it could not answer. Every line it logs on the UART starts "nimble: "
   (arm64-nimble-check.py reads them). */
int nimble_ask(const char *q, unsigned n);                   /* nimble.c */
int nimble_state(void);                                      /* 0 idle, 1 answered, 2 no network, 3 error */
const char *nimble_error(void);
const char *nimble_answer(unsigned *len);
unsigned nimble_sources(void);
unsigned nimble_asks(void);                                  /* how many questions nimble.c has been asked, from anyone */
const char *nimble_source_title(unsigned i);
const char *nimble_source_url(unsigned i);
int ask_busy(void);                                          /* ask.c: Samantha holds the network and the heap */
#define NM_ROW 16          /* an answer line, logical pixels */
#define NM_WELL_Y 64       /* the well's top, under the field */
#define NM_VL_MAX 96       /* wrapped lines kept: 1000 characters never need more */
#define NM_Q_MAX 120
#define NM_SRC_HEAD 22     /* the "Sources" label above the rows */
#define NM_SRC_ROW 20
#define NM_ACCENT 0x00b5502c
static unsigned short nm_off[NM_VL_MAX], nm_len[NM_VL_MAX];   /* wrapped answer line i: an offset into the answer and a length */
static unsigned nm_n, nm_sel;                                 /* wrapped lines, and the source Tab picked (1-based, 0 none) */
static int nm_top, nm_loading, nm_all;                       /* first visible line; a request is running; the field's text is selected (typing replaces it) */
static int nm_have;                                           /* the window's own question has an answer on screen: Samantha's and the Terminal's asks share nimble.c but not this window */
static unsigned nm_seen;                                      /* nimble_asks() when it did: another ask since means that answer is gone */
static int nm_frame = 1;                                      /* the window frame needs drawing: set on open, cleared once drawn */
static char nm_q[NM_Q_MAX + 1], nm_msg[96];
static unsigned nm_qn;
static int nm_st(void) { return nm_have && nimble_asks() == nm_seen ? nimble_state() : 0; }   /* the window sees the state only of its own last ask */
static int nm_w(void) { return win_lw - 16; }
static int nm_h(void) { return win_lh - NM_WELL_Y - 26; }
static int nm_src_h(void) { unsigned k = nimble_sources(); return nm_st() == 1 && k ? NM_SRC_HEAD + (int)k * NM_SRC_ROW + 4 : 0; }
static int nm_rows(void) { int r = (nm_h() - 8 - nm_src_h()) / NM_ROW; return r > 1 ? r : 1; }
static void nm_wrap(void) {   /* the answer into lines that fit the well: break at the last space that fits, or inside a word that is too long */
    unsigned len; const char *t = nimble_answer(&len);
    int s = (int)window_scale(), maxw = (nm_w() - 20) * s;
    nm_n = 0;
    for (unsigned i = 0; len && nm_n < NM_VL_MAX;) {
        unsigned e = i; while (e < len && t[e] != '\n') e++;
        unsigned a = i;
        if (a == e) { nm_off[nm_n] = (unsigned short)a; nm_len[nm_n++] = 0; }
        while (a < e && nm_n < NM_VL_MAX) {
            unsigned b = a, sp = 0; int w = 0;
            while (b < e && b - a < 127) {
                char c[2] = { t[b], 0 }; int cw = text_ok ? text_width(2, c, 120 * s) : 0;
                if (w + cw > maxw && b > a) break;
                w += cw; if (t[b] == ' ') sp = b; b++;
            }
            unsigned brk = b < e && sp > a ? sp : b;
            nm_off[nm_n] = (unsigned short)a; nm_len[nm_n++] = (unsigned short)(brk - a);
            a = brk; while (a < e && t[a] == ' ') a++;
        }
        if (e >= len) break;
        i = e + 1;
    }
}
static void nm_scroll(int top) {   /* clamped: the last page of lines is the furthest down */
    int max = (int)nm_n - nm_rows(); if (max < 0) max = 0;
    nm_top = top < 0 ? 0 : top > max ? max : top;
}
static int nm_src_y(void) { return win_ly + NM_WELL_Y + nm_h() - nm_src_h(); }   /* the top of the sources block */
static void nm_glyph(int cx, int cy) {   /* the icon's magnifier, flat in the accent: a ring and a handle, no text */
    gui_draw_capsule(cx + 14, cy + 14, cx + 30, cy + 30, 5, NM_ACCENT, 0x00faf8f4);
    gui_fill_circle(cx, cy, 21, NM_ACCENT, 0x00faf8f4);
    gui_fill_circle(cx, cy, 12, 0x00faf8f4, NM_ACCENT);
}
static void nm_center(const char *s, int x, int w, int y, unsigned color) { gui_text(s, x + (w - gui_text_width(s)) / 2, y, color); }
static void nimble_paint(int report) {
    int s = (int)window_scale(), x = win_lx + 8, w = nm_w(), y = win_ly + NM_WELL_Y, h = nm_h(), ty = win_ly + 34;
    cur_hide();
    int off = off_begin(win_lx * s, win_ly * s, win_lw * s, win_lh * s);   /* the whole window is drawn off screen, then copied over in one pass */
    if (nm_frame) { gui_draw_window_frame(win_lx, win_ly, win_lw, win_lh, "Nimble"); nm_frame = 0; }
    else window_rect(win_lx + 8, win_ly + 30, win_lw - 16, win_lh - 34, 0x00F5F0EB);
    int fx = win_lx + 10, fw = win_lw - 20;
    window_rect(fx, ty, fw, 22, NM_ACCENT); window_rect(fx + 1, ty + 1, fw - 2, 20, 0x00FFFFFF);
    const char *shown = nm_q;
    while (*shown && gui_text_width(shown) > fw - 20) shown++;   /* a long question shows its end */
    if (nm_all && *shown) window_rect(fx + 5, ty + 3, gui_text_width(shown) + 2, 16, 0x00F3E2D8);
    gui_text(*shown ? shown : "Ask Nimble anything", fx + 6, ty + 3, *shown ? 0x001C1C1E : 0x0075726E);
    if (!nm_all) window_rect(fx + 7 + gui_text_width(shown), ty + 4, 1, 14, NM_ACCENT);
    window_rect(x - 1, y - 1, w + 2, h + 2, 0x00DDD8CE); window_rect(x, y, w, h, 0x00faf8f4);
    int st = nm_st();
    unsigned len; const char *t = nimble_answer(&len);
    if (nm_loading) {
        nm_center("Asking Nimble...", x, w, y + h / 2 - 8, 0x0075726E);
    } else if (st == 1) {
        int rows = nm_rows();
        for (int r = 0; r < rows && nm_top + r < (int)nm_n; r++) {
            unsigned vi = (unsigned)(nm_top + r), n = nm_len[vi];
            char buf[128]; for (unsigned k = 0; k < n; k++) buf[k] = t[nm_off[vi] + k]; buf[n] = 0;
            gui_text(buf, x + 10, y + 6 + r * NM_ROW, 0x001C1C1E);
            if (report && n) { con_quiet = 1; uart_puts("nimble: line "); uart_puts(buf); uart_putc('\n'); }
        }
        unsigned ns = nimble_sources();
        if (ns) {
            int sy = nm_src_y();
            gui_hairline_h(x + 8, sy, w - 16, 0x00DDD8CE);
            gui_text("Sources", x + 10, sy + 4, 0x0075726E);
            for (unsigned k = 0; k < ns; k++) {
                int ry = sy + NM_SRC_HEAD + (int)k * NM_SRC_ROW;
                if (nm_sel == k + 1) window_rect(x + 2, ry - 1, w - 4, NM_SRC_ROW - 2, 0x00F3E2D8);
                char line[96], num[6] = { (char)('1' + k), '.', ' ', 0, 0, 0 }; unsigned o = 0;
                for (const char *c = num; *c; c++) line[o++] = *c;
                for (const char *c = nimble_source_title(k); *c && o < 80; c++) line[o++] = *c;
                line[o] = 0;
                while (o > 4 && gui_text_width(line) > w - 28) line[--o] = 0;
                gui_text(line, x + 10, ry + 1, NM_ACCENT);
                if (report) { con_quiet = 1; uart_puts("nimble: shows source "); uart_dec(k + 1); uart_puts(" at "); uart_dec((unsigned)(x + 24) * (unsigned)s); uart_putc(','); uart_dec((unsigned)(ry + 9) * (unsigned)s); uart_putc('\n'); }
            }
        }
    } else if (st == 0) {
        int top = y + (h - 64 - 30) / 2;
        nm_glyph(x + w / 2 - 8, top + 24);
        nm_center("Ask a question, press Enter", x, w, top + 64 + 10, 0x0075726E);
    } else {   /* 2 no network, 3 the service could not answer: the reason in plain words */
        int top = y + h / 2 - 22;
        nm_center(st == 2 ? "No network" : "Nimble could not answer", x, w, top, 0x001C1C1E);
        nm_center(st == 2 ? "Nimble needs the internet. Check Wi-Fi." : nimble_error(), x, w, top + 24, 0x0075726E);
    }
    char left[96]; unsigned o = 0;
    const char *msg = nm_loading ? "asking..." : nm_msg[0] ? nm_msg : st == 2 ? "no network" : st == 3 ? nimble_error() : st == 1 ? "from Nimble" : "";
    if (!nm_loading && !nm_msg[0] && nm_sel && nm_sel <= nimble_sources()) msg = nimble_source_url(nm_sel - 1);
    for (; msg[o] && o < sizeof left - 1; o++) left[o] = msg[o];
    left[o] = 0;
    while (o && gui_text_width(left) > w - 24) left[--o] = 0;
    gui_text(left, x + 2, win_ly + win_lh - 22, 0x0075726E);
    if (off) off_end(); else fb_flush(win_lx * s, win_ly * s, win_lw * s, win_lh * s);
    cur_show();
}
static void nm_ask(void) {   /* Enter in the field */
    if (!nm_qn) return;
    nm_msg[0] = 0;
    if (ask_busy()) {   /* Samantha holds the one connection: say so, do not collide */
        br_set(nm_msg, "Samantha is using the network, try again in a moment", sizeof nm_msg);
        nimble_paint(0); return;
    }
    con_quiet = 1; uart_puts("nimble: ask "); uart_puts(nm_q); uart_putc('\n');
    nm_loading = 1; nm_sel = 0; nm_top = 0;
    nimble_paint(0);
    int ok = nimble_ask(nm_q, nm_qn);
    nm_loading = 0; nm_all = 1; nm_have = 1; nm_seen = nimble_asks();
    if (ok) {
        unsigned len; nimble_answer(&len);
        nm_wrap();
        con_quiet = 1; uart_puts("nimble: answer "); uart_dec(len); uart_puts(" chars, "); uart_dec(nimble_sources()); uart_puts(nimble_sources() == 1 ? " source\n" : " sources\n");
    } else {
        nm_n = 0;
        con_quiet = 1; uart_puts("nimble: status "); uart_puts(nimble_state() == 2 ? "no network" : nimble_error()); uart_putc('\n');
    }
    nimble_paint(1);
}
static void nimble_open(void) {
    if (nimble_live || !fb) return;
    clock_close(); brick_close(); calc_close(); browser_close(); app_kill("closed for Nimble"); console_close();
    nimble_live = 1; nm_frame = 1; nm_sel = 0; nm_top = 0; nm_all = nm_qn != 0; nm_msg[0] = 0; nm_loading = 0;
    if (nm_st() == 1) nm_wrap(); else nm_n = 0;
    nimble_paint(nm_st() == 1); uart_puts("nimble open\n");
}
static void nimble_close(void) {
    if (!nimble_live) return;
    nimble_live = 0;
    con_live = 1; console_close(); pane_open(cp);
    uart_puts("nimble closed\n");
}
static void nm_open_source(unsigned n) {   /* source n in the Browser, as typing its address there would */
    const char *u = nimble_source_url(n - 1);
    if (!*u || n > nimble_sources()) return;
    con_quiet = 1; uart_puts("nimble: open source "); uart_dec(n); uart_putc(' '); uart_puts(u); uart_putc('\n');
    nimble_live = 0;   /* the Browser takes the window; no Terminal repaint in between */
    browser_open();    /* opens with an empty address bar, so the address goes in after */
    unsigned k = 0; for (; u[k] && k < BR_ADDR_MAX; k++) br_addr[k] = u[k];
    br_addr[k] = 0; br_addr_n = k;
    br_load();
}
static int nimble_key(unsigned code) {   /* a key down with Nimble in front and Spotlight closed; always Nimble's */
    int page = nm_rows() > 1 ? nm_rows() - 1 : 1;
    if (code == 1) { if (nm_qn) { nm_qn = 0; nm_q[0] = 0; nm_all = 0; nimble_paint(0); } else nimble_close(); return 1; }
    if (code == 104 || code == 109 || code == 102 || code == 107 || code == 103 || code == 108) {
        nm_scroll(code == 104 ? nm_top - page : code == 109 ? nm_top + page : code == 102 ? 0 : code == 107 ? (int)nm_n : code == 103 ? nm_top - 1 : nm_top + 1);
        nm_msg[0] = 0; nimble_paint(0); return 1;
    }
    if (ctrl_held || gui_held || alt_held) return 1;
    if (code == 15) {   /* Tab: the next source, wrapping */
        unsigned k = nm_st() == 1 ? nimble_sources() : 0;
        if (k) { nm_sel = nm_sel % k + 1; nm_msg[0] = 0; con_quiet = 1; uart_puts("nimble: selected source "); uart_dec(nm_sel); uart_putc('\n'); nimble_paint(0); }
        return 1;
    }
    if (code == 28 || code == 96) {
        if (nm_sel) nm_open_source(nm_sel);
        else if (nm_qn) nm_ask();
        return 1;
    }
    if (code == 14) { if (nm_all) { nm_qn = 0; nm_q[0] = 0; nm_all = 0; } else if (nm_qn) nm_q[--nm_qn] = 0; nm_msg[0] = 0; nimble_paint(0); return 1; }
    int ch = ask_char(code);
    if (ch >= ' ' && ch < 127) {
        if (nm_all) { nm_qn = 0; nm_q[0] = 0; nm_all = 0; }
        nm_sel = 0; nm_msg[0] = 0;
        if (nm_qn < NM_Q_MAX) { nm_q[nm_qn++] = (char)ch; nm_q[nm_qn] = 0; }
        nimble_paint(0);
    }
    return 1;
}
static void nimble_wheel(int v) {   /* REL_WHEEL: up is positive */
    if (!nimble_live || spot_live || !v) return;
    nm_scroll(nm_top - v * 3); nimble_paint(0);
}
static void nimble_click(int lx, int ly) {
    if (nm_st() != 1) return;
    unsigned ns = nimble_sources();
    int sy = nm_src_y() + NM_SRC_HEAD, x = win_lx + 8;
    if (!ns || lx < x || lx >= x + nm_w() || ly < sy) return;
    unsigned k = (unsigned)((ly - sy) / NM_SRC_ROW);
    if (k < ns) nm_open_source(k + 1);
}
