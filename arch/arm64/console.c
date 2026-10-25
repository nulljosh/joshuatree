/* console.c: cut out of main.c (the god-file split). The boot log on the screen: the Console and the Terminal panes, their scrollback, and the text grid. Built as part of main.o (the Makefile's MAINSRC) with
   main.c's own flags; what it shares with main.c is declared in kmain.h. */
#include "kmain.h"
/* ---- The boot log on the screen. Everything the kernel prints over the UART is also kept in a small buffer and drawn
   into the window, so a first boot with the monitor plugged in shows what happened even if the serial cable is wrong.
   Lines printed before the screen exists are replayed once it does. When the window fills, it is wiped and the newest
   half page is redrawn at the top, so the last lines printed are always the ones on screen. A USB or virtio keyboard
   scrolls it: Page Up and Page Down by half a page, Home to the first line, End to the newest. The title bar says which
   lines are shown, and a pinned row under the text keeps the latest wifi and usb status in view wherever you are. ---- */
#include "../../drivers/vgafont.h"
const unsigned char *const vgafont = vgafont_glyphs;   /* crash.c draws the panel with it: one copy of the table */
#define CON_FG 0x00202020
#define CON_BG 0x00ffffff
unsigned wall_get(void);   /* wall.c: the wallpaper colour, 0x00RRGGBB (Settings changes it) */
int wall_paint(unsigned *fb, unsigned pitch, int w, int h, int swap);   /* arch/arm64/wall.c: the flat Navy wallpaper */
#define MENUBAR_H 26             /* docs/ARCHITECTURE.md: GUI_MENUBAR_H, on the 960x540 grid */
#define MENUBAR_RULE 0x00BDB8B0  /* docs/ARCHITECTURE.md: MENUBAR_RULE */
int text_init(void);   /* arch/arm64/text.c: the DejaVu faces through drivers/ttf.c */
int text_draw(int which, const char *s, int x, int baseline, int px10, unsigned fg, unsigned *fb, unsigned pitch, int w, int h);
int text_width(int which, const char *s, int px10);
int text_selftest(int px10, int *w, int *h, int *adv);
int text_ok;     /* smooth text is up */
unsigned fb_color(unsigned c) { return fb_swap ? (c & 0xFF00FF00u) | (c >> 16 & 0xFF) | (c & 0xFF) << 16 : c; }
struct pane con_p, terminals[2], *cp = &con_p;
unsigned term_index;
#define term_p terminals[term_index]
int con_wx, con_wy, con_ww;
int con_live;    /* the framebuffer is up: draw as we go */
int calc_live, calc_front;   /* slice 5: the Calculator is open; calc_front, it is on top and has the keys, so the pane behind it only keeps its text */
int app_front;               /* M3c part 2: the same for an EL0 app's window (never open with the Calculator) */
int con_x, con_y, con_cw, con_ch, con_base, con_cols, con_rows, con_px10;
static int con_vga;     /* 0: DejaVu Sans Mono; n: the 8x16 VGA font drawn n times its size */
int mono_w, mono_h, mono_adv, mono_ok;   /* the 'M' self-test, kept for the diagnostic line */
static void con_glyph(unsigned col, unsigned row, char c) {
    int x = con_x + (int)col * con_cw, y = con_y + (int)row * con_ch;
    if (!con_vga) {
        char b[2] = { c, 0 };
        text_draw(0, b, x, y + con_base, con_px10, fb_color(CON_FG), fb, fb_pitch, (int)fb_w, (int)fb_h);
    } else if (c >= VGAFONT_FIRST && c <= VGAFONT_LAST) {   /* integer only: no rasterizer, no floating point */
        const unsigned char *g = vgafont_glyphs + (c - VGAFONT_FIRST) * 16;
        unsigned px = fb_color(CON_FG);
        for (int gy = 0; gy < 16; gy++)
            for (int gx = 0; gx < 8; gx++)
                if (g[gy] & (0x80 >> gx))
                    for (int a = 0; a < con_vga; a++)
                        for (int b = 0; b < con_vga; b++) fb[(unsigned)(y + gy * con_vga + a) * fb_pitch + (unsigned)(x + gx * con_vga + b)] = px;
    }
    fb_flush(x - 4, y, con_cw + 8, con_ch);   /* a few pixels spare: a smooth glyph can lean past its cell */
}
void con_wipe(void) {
    fb_rect(con_x, con_y, con_cols * con_cw, con_rows * con_ch, CON_BG);
    fb_flush(con_x - 4, con_y, con_cols * con_cw + 8, con_rows * con_ch);
    cp->col = cp->row = 0;
}
static int con_replaying;
void con_draw(unsigned i);
static void con_scroll(unsigned end) {   /* the window is full: wipe it and redraw the newest half page from the log */
    con_wipe();
    if (con_replaying) return;
    unsigned start = end; int nl = 0;
    while (start > 0) { if (cp->log[start - 1] == '\n' && ++nl > con_rows / 2) break; start--; }
    cp->anchor = start;
    con_replaying = 1;
    for (unsigned k = start; k < end; k++) con_draw(k);
    con_replaying = 0;
}
void con_draw(unsigned i) {   /* draws cp->log[i]; a newline only moves the cursor, the next character scrolls */
    char c = cp->log[i];
    if (c == '\r') return;
    if (c == '\n') { cp->col = 0; cp->row++; return; }
    if (cp->col >= (unsigned)con_cols) { cp->col = 0; cp->row++; }
    if (cp->row >= (unsigned)con_rows) {
        con_scroll(i);
        if (cp->col >= (unsigned)con_cols) { cp->col = 0; cp->row++; }
        if (cp->row >= (unsigned)con_rows) con_wipe();
    }
    con_glyph(cp->col++, cp->row, c);
}
unsigned con_nlines(void) {   /* lines in the log, a last line still being written counts */
    unsigned n = 0;
    for (unsigned i = 0; i < cp->len; i++) if (cp->log[i] == '\n') n++;
    return n + (cp->len && cp->log[cp->len - 1] != '\n');
}
unsigned con_line_off(unsigned line) {   /* where line n starts in the log */
    unsigned i = 0;
    while (line && i < cp->len) { if (cp->log[i++] == '\n') line--; }
    return i;
}
static unsigned con_off_line(unsigned off) { unsigned n = 0; for (unsigned i = 0; i < off && i < cp->len; i++) if (cp->log[i] == '\n') n++; return n; }
#define CON_TITLE_BG  0x00F5F0EB   /* the window frame's cream title band (gui_draw_window_frame) */
#define CON_HINT_INK  0x0075726E   /* secondary grey ink on it */
static void con_text(const char *s, int right, int y) {   /* a short grey string, right edge at x = right, vertically in the title band */
    int px = sc(110);
    if (text_ok) { text_draw(1, s, right - text_width(1, s, px), y + sc(20), px, fb_color(CON_HINT_INK), fb, fb_pitch, (int)fb_w, (int)fb_h); return; }
    int n = 0; while (s[n]) n++;
    int x = right - n * 8 * con_vga; y += (sc(28) - 16 * con_vga) / 2;
    for (int i = 0; i < n; i++) {
        char c = s[i];
        if (c < VGAFONT_FIRST || c > VGAFONT_LAST) continue;
        const unsigned char *g = vgafont_glyphs + (c - VGAFONT_FIRST) * 16;
        for (int gy = 0; gy < 16; gy++) for (int gx = 0; gx < 8; gx++) if (g[gy] & (0x80 >> gx))
            for (int a = 0; a < con_vga; a++) for (int b = 0; b < con_vga; b++)
                fb[(unsigned)(y + gy * con_vga + a) * fb_pitch + (unsigned)(x + (i * 8 + gx) * con_vga + b)] = fb_color(CON_HINT_INK);
    }
}
void con_hint(void) {   /* the title bar's "lines 12-27 of 61": a photo says which part of the log it shows */
    unsigned total = con_nlines(), first = cp->scrolled ? cp->vtop : con_off_line(cp->anchor), last = cp->scrolled ? cp->vlast : total;
    char b[40]; int n = 0;
    for (const char *t = "lines "; *t; ) b[n++] = *t++;
    unsigned v[3] = { first + 1, last, total };
    for (int k = 0; k < 3; k++) {
        char d[12]; int m = 0; unsigned x = v[k];
        do { d[m++] = (char)('0' + x % 10); x /= 10; } while (x);
        while (m) b[n++] = d[--m];
        if (k == 0) b[n++] = '-'; else if (k == 1) { b[n++] = ' '; b[n++] = 'o'; b[n++] = 'f'; b[n++] = ' '; }
    }
    b[n] = 0;
    /* right of the centred title and left of the rounded corner, above the hairline: only the band's flat cream */
    int x0 = con_wx + con_ww / 2 + sc(40), y0 = con_wy + sc(3), w = con_ww / 2 - sc(60), h = sc(24);
    fb_rect(x0, y0, w, h, CON_TITLE_BG);
    con_text(b, x0 + w - sc(4), con_wy);
    fb_flush(x0, y0, w, h);
}
static int con_is_status(const char *l, unsigned n) {   /* wifi and usb lines that report a failure; no news is good news */
    int bad = 0; for (unsigned i = 0; i + 4 <= n; i++) if (l[i] == 'F' && l[i + 1] == 'A' && l[i + 2] == 'I' && l[i + 3] == 'L') bad = 1;
    if (!bad) return 0;
    if (n >= 4 && l[0] == 'w' && l[1] == 'i' && l[2] == 'f' && l[3] == 'i') return 1;
    if (n >= 4 && l[0] == 'u' && l[1] == 's' && l[2] == 'b' && l[3] == ' ') return !(n >= 8 && l[4] == 'k' && l[5] == 'e' && l[6] == 'y' && l[7] == ' ');
    return 0;
}
void con_summary(void) {   /* the row under the text: the newest wifi and usb line, cut to fit, whatever is scrolled into view */
    if (cp != &con_p) return;   /* the Console's own row: the Terminal has its prompt there */
    unsigned wo = 0, wn = 0, uo = 0, un = 0;
    for (unsigned i = 0; i < con_p.len; ) {
        unsigned e = i; while (e < con_p.len && con_p.log[e] != '\n') e++;
        if (con_is_status(con_p.log + i, e - i)) { if (con_p.log[i] == 'w') { wo = i; wn = e - i; } else { uo = i; un = e - i; } }
        i = e + 1;
    }
    unsigned cols = (unsigned)con_cols > 90 ? 90 : (unsigned)con_cols, half = cols > 4 ? (cols - 3) / 2 : 1;
    char b[96]; unsigned n = 0;
    for (int part = 0; part < 2; part++) {
        const char *l = part ? con_p.log + uo : con_p.log + wo, *none = part ? "usb -" : "wifi -"; unsigned ln = part ? un : wn;
        unsigned start = n;
        if (!ln) { l = none; (void)l; ln = 0; }
        for (unsigned i = 0; i < ln && n - start < half; i++) b[n++] = (l[i] >= 32 && l[i] < 127) ? l[i] : '?';
        if (!part) { while (n - start < half) b[n++] = ' '; b[n++] = '|'; b[n++] = ' '; }
    }
    if (!wn && !un) n = 0;   /* nothing broke: an empty row */
    int y = con_y + con_rows * con_ch;
    fb_rect(con_x, y, con_cols * con_cw, con_ch, CON_BG);
    fb_flush(con_x - 4, y, con_cols * con_cw + 8, con_ch);
    for (unsigned i = 0; i < n; i++) if (b[i] != ' ') con_glyph(i, (unsigned)con_rows, b[i]);
}
static int con_render(unsigned top) {   /* draws the window from line `top` down; 1 if the end of the log fits */
    unsigned nl = con_nlines(), line = top, last = top;
    con_wipe();
    for (unsigned k = con_line_off(top); k < cp->len; k++) {
        char c = cp->log[k];
        if (c == '\r') continue;
        if (c == '\n') {
            cp->col = 0; cp->row++; line++;
            if (cp->row >= (unsigned)con_rows) { cp->vlast = last + 1; return 0; }
            last = line; continue;
        }
        if (cp->col >= (unsigned)con_cols) { cp->col = 0; cp->row++; }
        if (cp->row >= (unsigned)con_rows) { cp->vlast = last + 1; return 0; }
        con_glyph(cp->col++, cp->row, c);
        last = line;
    }
    cp->vlast = last < nl ? last + 1 : nl;
    return 1;
}
static void con_follow(void) {   /* End: back to the newest, the same picture the streaming draw would have made */
    if (!cp->scrolled) return;
    cp->scrolled = 0;
    con_wipe();
    for (unsigned k = cp->anchor; k < cp->len; k++) con_draw(k);
}
int ask_tui(void);   /* ask.c: the chat view (`sam` alone) has the Terminal window */
void ask_tui_scroll(unsigned code);
int con_key(unsigned code, unsigned value) {   /* Page Up 104, Page Down 109, Home 102, End 107: 1 if the console took the key */
    if (code != 104 && code != 109 && code != 102 && code != 107) return 0;
    if (ask_tui()) { if (value) ask_tui_scroll(code); return 1; }   /* the chat view scrolls its own history, not the pane's log */
    if (!con_live || !value || calc_front || app_front) return 1;
    unsigned half = (unsigned)con_rows / 2 ? (unsigned)con_rows / 2 : 1, nl = con_nlines();
    if (code == 104 || code == 102) {
        unsigned cur = cp->scrolled ? cp->vtop : con_off_line(cp->anchor);
        cp->vtop = code == 102 ? 0 : cur > half ? cur - half : 0;
        cp->scrolled = 1;
        con_render(cp->vtop);
    } else if (code == 109 && cp->scrolled) {
        cp->vtop += half; if (cp->vtop >= nl) cp->vtop = nl ? nl - 1 : 0;
        if (con_render(cp->vtop)) con_follow(); /* the end of the log is in view: follow again */
    } else if (code == 107) con_follow();
    con_hint();
    return 1;
}
int cur_hold(void);           /* slice 4, below: take the pointer's arrow off the Console while it draws */
void cur_release(int held);   /* ...and put it back */
static void pane_putc(struct pane *p, char c) {
    if (p->len == LOG_MAX) {   /* full: forget the older half, up to a line end */
        unsigned drop = LOG_MAX / 2, gone = 0;
        while (drop < p->len && p->log[drop - 1] != '\n') drop++;
        for (unsigned i = 0; i < drop; i++) if (p->log[i] == '\n') gone++;
        for (unsigned i = drop; i < p->len; i++) p->log[i - drop] = p->log[i];
        p->len -= drop;
        p->anchor = p->anchor > drop ? p->anchor - drop : 0;
        p->vtop = p->vtop > gone ? p->vtop - gone : 0; p->vlast = p->vlast > gone ? p->vlast - gone : 0;
    }
    p->log[p->len++] = c;
    if (!con_live || p != cp || calc_front || app_front) return;   /* a window that is closed or behind only keeps its text */
    int held = cur_hold();
    unsigned was = cp->anchor;
    if (!cp->scrolled) con_draw(cp->len - 1);
    if (c == '\n' || cp->anchor != was) {
        unsigned e = cp->len - 1, s = e; while (s > 0 && cp->log[s - 1] != '\n') s--;
        if (c == '\n' && con_is_status(cp->log + s, e - s)) con_summary();
        con_hint();
    }
    cur_release(held);
}
void console_putc(char c) { pane_putc(con_to_term ? &term_p : &con_p, c); }
/* The console's type. DejaVu Sans Mono at the screen's scale, unless a quick test of the rasterizer says no: then the
   8x16 VGA font at a whole-number scale (2x on a 1080p screen), which needs nothing but integer stores. */
void con_place(int win_x, int win_y, int win_w, int win_h) {   /* where the grid sits and how many columns and rows fit the window, for the font already chosen */
    con_wx = win_x; con_wy = win_y; con_ww = win_w;
    con_x = win_x + sc(8); con_y = win_y + sc(32);
    con_cols = (win_w - sc(12)) / con_cw;
    con_rows = (win_h - sc(46)) / con_ch - 1;   /* then one row: the Console's wifi and usb status, or the Terminal's ask> prompt */
}
void con_layout(int win_x, int win_y, int win_w, int win_h) {
    con_px10 = sc(133);
    mono_ok = text_ok && text_selftest(con_px10, &mono_w, &mono_h, &mono_adv);
    if (mono_ok) { con_vga = 0; con_cw = mono_adv; con_ch = sc(16); con_base = sc(12); }
    else {
        con_vga = ((int)fb_h * 10 / 600 + 5) / 10;
        if (con_vga < 1) con_vga = 1;
        con_cw = 8 * con_vga; con_ch = 16 * con_vga; con_base = 0;
    }
    con_place(win_x, win_y, win_w, win_h);
}
const char *ask_prompt(void);   /* ask.c: "joshua@tree % " */
/* The Terminal's bottom row: the prompt and the line being typed (ask.c), its tail when it is longer than the row, then a cursor. */
void con_clear(void) {         /* /clear: the Terminal's window empties, its text stays in the scrollback */
    if (!con_live || calc_front || app_front || ask_tui()) return;
    int held = cur_hold();
    con_wipe(); cp->anchor = cp->len; cp->scrolled = 0; con_hint();
    cur_release(held);
}
static void con_input_row(const char *p, const char *s, unsigned n) {   /* the bottom row: a prompt, the line being typed (its tail when long), a cursor */
    if (!con_live || calc_front || app_front || cp != &term_p) return;
    int held = cur_hold();
    unsigned plen = 0; while (p[plen]) plen++;
    unsigned row = (unsigned)con_rows, cols = (unsigned)con_cols, room = cols > plen + 2 ? cols - plen - 1 : 1;
    int y = con_y + (int)row * con_ch;
    fb_rect(con_x, y, con_cols * con_cw, con_ch, CON_BG);
    fb_flush(con_x - 4, y, con_cols * con_cw + 8, con_ch);
    unsigned col = 0;
    for (; *p; p++) con_glyph(col++, row, *p);
    if (n > room) { s += n - room; n = room; }
    for (unsigned i = 0; i < n; i++, col++) if (s[i] != ' ') con_glyph(col, row, s[i]);
    con_glyph(col, row, '_');
    cur_release(held);
}
void con_prompt(const char *s, unsigned n) { con_input_row(ask_prompt(), s, n); }
/* The chat view (ask.c, `sam` alone) takes the Terminal window over. The pane's log is not touched while it is up and is
   drawn again on the way out. Rows are kept in a shadow so a new line repaints only the rows that changed. */
#define TUI_ROWS 96
#define TUI_COLS 128
static char tui_shadow[TUI_ROWS][TUI_COLS];
int tui_face_rows;   /* the chat view's face takes the top rows (con_tui_begin), 0 when the window is short or the view is closed */
int con_rows_total(void) { return con_live && fb && cp == &term_p && !calc_front && !app_front ? con_rows - tui_face_rows : 0; }   /* text rows above the input row; 0 with no window to draw in */
#include "face_art.h"
void tui_log(const char *s);   /* below: a line to the UART only */
int png_decode(const unsigned char *data, unsigned int len, unsigned char **out, unsigned int *w, unsigned int *h, unsigned int *channels);   /* drivers/png.c */
/* The chat view's face (5.0, "Samantha is the interface"): her portrait above the rows, from face_art.h (two frames of the
   web Samantha's face, mouth closed and mouth open), blended by how loud her voice is at that moment (ask.c voice_level,
   from the clip the sound path is playing), so her mouth opens with what she says. Seven rows when the window has room
   for them, fourteen in a tall one (21 rows or more), none in a short window. "face: on" on the UART when it is drawn; ask.c logs the mouth. */
static unsigned char *face_idle, *face_talk;
int face_tried, face_mouth = -1;
static void face_blit(int mouth) {
    if (!face_tried) {
        face_tried = 1; unsigned w, h, ch;
        if (png_decode(face_idle_png, FACE_IDLE_LEN, &face_idle, &w, &h, &ch) || w != FACE_PX || h != FACE_PX || ch != 4) face_idle = 0;
        if (png_decode(face_talk_png, FACE_TALK_LEN, &face_talk, &w, &h, &ch) || w != FACE_PX || h != FACE_PX || ch != 4) face_talk = 0;
    }
    int D = con_ch * (tui_face_rows - 1), x0 = con_x + (con_cols * con_cw - D) / 2, y0 = con_y + con_ch / 2;
    if (mouth < 0) mouth = 0;
    if (mouth > 100) mouth = 100;
    if (!face_idle) { fb_rect(x0, y0, D, D, CON_BG); fb_flush(x0, y0, D, D); face_mouth = mouth; return; }
    const unsigned char *talk = face_talk ? face_talk : face_idle;
    for (int y = 0; y < D; y++) for (int x = 0; x < D; x++) {   /* 2x2 samples a pixel, the two frames mixed by the mouth */
        unsigned r = 0, g = 0, b = 0;
        for (int j = 0; j < 2; j++) for (int i = 0; i < 2; i++) {
            unsigned o = (((y * 2 + j) * FACE_PX / (D * 2)) * FACE_PX + (x * 2 + i) * FACE_PX / (D * 2)) * 4;
            const unsigned char *p = face_idle + o, *q = talk + o;
            r += p[0] * (100 - mouth) + q[0] * mouth; g += p[1] * (100 - mouth) + q[1] * mouth; b += p[2] * (100 - mouth) + q[2] * mouth;
        }
        fb[(unsigned)(y0 + y) * fb_pitch + (unsigned)(x0 + x)] = fb_color(((r / 400) << 16) | ((g / 400) << 8) | (b / 400));
    }
    fb_flush(x0, y0, D, D);
    face_mouth = mouth;
}
void con_tui_mouth(int mouth) {   /* ask.c, while her voice plays: only the face is repainted */
    if (!tui_face_rows || !con_rows_total() || mouth == face_mouth) return;
    int held = cur_hold(); face_blit(mouth); cur_release(held);
}
void con_tui_begin(void) {
    if (!con_rows_total()) return;
    int held = cur_hold();
    tui_face_rows = con_rows < 14 ? 0 : con_rows >= 21 ? 14 : 7;   /* a tall window (the Samantha tile's, 4.20.0) gets a face twice as big */
    con_wipe();
    for (unsigned r = 0; r < TUI_ROWS; r++) tui_shadow[r][0] = 0;
    if (tui_face_rows) { face_mouth = -1; face_blit(0); tui_log(tui_face_rows > 7 ? "face: on, big\n" : "face: on\n"); }   /* the UART only, never the window */
    cur_release(held);
}
void con_tui_rows(const char *const *rows, unsigned n) {
    if (!con_rows_total()) return;
    int held = cur_hold();
    for (unsigned r = 0; r < n && r < TUI_ROWS && r < (unsigned)(con_rows - tui_face_rows); r++) {
        const char *t = rows[r] ? rows[r] : "";
        unsigned k = 0, lim = (unsigned)con_cols < TUI_COLS - 1 ? (unsigned)con_cols : TUI_COLS - 1;
        while (k < lim && t[k] && t[k] == tui_shadow[r][k]) k++;
        if (k == lim || (!t[k] && !tui_shadow[r][k])) continue;   /* the same text */
        int y = con_y + ((int)r + tui_face_rows) * con_ch;
        fb_rect(con_x, y, con_cols * con_cw, con_ch, CON_BG);
        fb_flush(con_x - 4, y, con_cols * con_cw + 8, con_ch);
        unsigned i = 0;
        for (; i < lim && t[i]; i++) { tui_shadow[r][i] = t[i]; if (t[i] != ' ') con_glyph(i, r + (unsigned)tui_face_rows, t[i]); }
        tui_shadow[r][i] = 0;
    }
    cur_release(held);
}
void con_tui_input(const char *p, const char *s, unsigned n) { con_input_row(p, s, n); }
int con_columns(void) { return con_live ? con_cols : 0; }
void con_start(void) {   /* the screen is ready: replay what was printed before it */
#ifdef QUIET_BOOT
    return;   /* the log stays in the buffer with its window closed; pane_open draws every line when it is asked for */
#endif
    con_live = 1;
    cp->col = cp->row = 0;
    for (unsigned i = 0; i < cp->len; i++) con_draw(i);
    con_summary(); con_hint();
}

