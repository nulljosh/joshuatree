/* The login screen (4.9.0), drawn by main.c over the whole screen when JT/USERS.TXT holds at least one account: the wallpaper
   colour, a cream card with the mark, the name "Joshua Tree", a name field and a masked password field. Return submits, Tab
   switches field, Backspace deletes. A wrong password shows one plain line, clears the password and costs a second
   (login.c). While it is up main.c's input_event hands every key to login_key and drops the mouse, and the bar and tile
   painters stand back (login_live). A right password repaints the desktop (wall_repaint) with the name in the menu bar.
   The UART says `login: screen up`, `login: wrong password`, `login: signed in NAME` and `login: desktop up`; it never
   says a typed character. `logout` (cmd.c) and the tree menu's Log Out close every window and bring this screen back. */
#include "login.h"
#define LG_W 300
#define LG_H 244
#define LG_INK 0x001C1C1E
#define LG_HINT 0x0075726E
#define LG_EDGE 0x00DDD8CE
#define LG_ACCENT 0x00B5502C
#ifndef SUDO_TTL_MS
#define SUDO_TTL_MS 300000   /* sudo.c: how long a right sudo password is remembered; the sudo check's build shortens it */
#endif
unsigned long sudo_now_ms(void) {   /* sudo.c: the generic counter in milliseconds, which keeps counting where `ticks` does not */
    unsigned long f, c; __asm__ volatile ("mrs %0, cntfrq_el0\n mrs %1, cntpct_el0" : "=r"(f), "=r"(c));
    return c / ((f ? f : 54000000) / 1000);
}
unsigned sudo_ttl_ms(void) { return SUDO_TTL_MS; }
void ask_forget(void);   /* ask.c */
static void terminals_forget(void) {   /* a sign-out empties both Terminals' scrollback, so the next person starts on a clean page */
    for (unsigned t = 0; t < 2; t++) {
        accounts_wipe(terminals[t].log, sizeof terminals[t].log);
        terminals[t].len = terminals[t].col = terminals[t].row = terminals[t].anchor = 0; terminals[t].scrolled = 0; terminals[t].vtop = terminals[t].vlast = 0;
    }
}
static int logout_flag;
void logout_request(void) { logout_flag = 1; }   /* cmd.c: the next pass of the main loop does it, outside the command */
void login_wait_ms(unsigned ms) {
    unsigned long f, c, end; __asm__ volatile ("mrs %0, cntfrq_el0\n mrs %1, cntpct_el0" : "=r"(f), "=r"(c));
    end = c + (f ? f : 54000000) / 1000 * ms;
    do __asm__ volatile ("mrs %0, cntpct_el0" : "=r"(c)); while (c < end);
}
static void lg_field(int x, int y, int w, int h, int focus) {   /* a flat white field, a hairline edge, the accent when it has the keys */
    int s = (int)window_scale();
    unsigned edge = focus ? LG_ACCENT : LG_EDGE;
    fb_rect(x * s, y * s, w * s, h * s, edge);
    fb_rect((x + 1) * s, (y + 1) * s, (w - 2) * s, (h - 2) * s, 0x00FFFFFF);
}
static void lg_centred(const char *t, int y, int bold, unsigned ink) {
    int s = (int)window_scale(), cx = ((int)fb_w / s - LG_W) / 2;
    if (bold) gui_icon_text(t, cx + (LG_W - gui_icon_text_w(t, 2, 1)) / 2, y, 2, 1, ink);
    else gui_text(t, cx + (LG_W - gui_text_width(t)) / 2, y, ink);
}
static void login_paint(void) {
    int s = (int)window_scale(), W = (int)fb_w, H = (int)fb_h;
    int cx = (W / s - LG_W) / 2, cy = (H / s - LG_H) / 2 - 8;
    cur_hide();
    fb_rect((cx - 6) * s, (cy - 6) * s, (LG_W + 12) * s, (LG_H + 12) * s, wall_get());   /* wipe the last card, the corners included */
    gui_rounded_rect_on_wallpaper(cx, cy, LG_W, LG_H, 0x00faf8f4, 14);
    menu_mark_paint(W / 2, (cy + 44) * s, sg(48), LG_INK);
    lg_centred("Joshua Tree", cy + 72, 1, LG_INK);
    int f = login_field(), fx = cx + 30, fw = LG_W - 60;
    lg_field(fx, cy + 114, fw, 30, f == 0);
    lg_field(fx, cy + 154, fw, 30, f == 1);
    const char *n = login_name();
    if (n[0]) gui_text(n, fx + 9, cy + 114 + 7, LG_INK); else gui_text("Name", fx + 9, cy + 114 + 7, LG_HINT);
    unsigned pl = login_pass_len();
    if (pl) {
        char stars[ACC_PASS_MAX + 1]; unsigned k = 0;
        for (; k < pl && k < ACC_PASS_MAX; k++) stars[k] = '*';
        stars[k] = 0;
        gui_text(stars, fx + 9, cy + 154 + 8, LG_INK);
    } else gui_text("Password", fx + 9, cy + 154 + 7, LG_HINT);
    {   /* the caret, after the text of the field that has the keys */
        const char *t = f == 0 ? n : 0; char stars[ACC_PASS_MAX + 1];
        if (f == 1) { unsigned k = 0; for (; k < pl && k < ACC_PASS_MAX; k++) stars[k] = '*'; stars[k] = 0; t = stars; }
        int tw = t[0] ? gui_text_width(t) : 0, cy0 = cy + (f ? 154 : 114);
        if (tw < fw - 24) fb_rect((fx + 9 + tw + 1) * s, (cy0 + 7) * s, s, 16 * s, LG_ACCENT);
    }
    if (login_message()) lg_centred("Wrong name or password.", cy + 200, 0, LG_ACCENT);
    else lg_centred("Return to sign in. Tab changes field.", cy + 200, 0, LG_HINT);
    fb_flush((cx - 6) * s, (cy - 6) * s, (LG_W + 12) * s, (LG_H + 12) * s);
}
static void login_show(void) {
    if (!fb || !text_ok || !accounts_count()) return;     /* no screen, or nobody to ask: the desktop as before */
    login_reset();
    sudo_drop(); ask_forget();                            /* a sign-out forgets sudo, any half-typed password and the chat */
    terminals_forget();                                   /* and clears the Terminals' scrollback */
    login_live = 1;
    cur_hide();
    fb_rect(0, 0, (int)fb_w, (int)fb_h, wall_get());
    fb_flush(0, 0, (int)fb_w, (int)fb_h);
    login_paint();
    con_quiet = 1; uart_puts("login: screen up\n");
}
static void login_done(void) {
    login_live = 0;
    con_quiet = 1; uart_puts("login: signed in "); uart_puts(current_user); uart_putc('\n');
    if (desktop_deferred) { desktop_deferred = 0; fb_desktop(); fb_diag(); }   /* the Pi's first desktop paint waited for this */
    else wall_repaint();                                  /* the desktop again, the name in its bar */
    if (con_live) { con_live = 0; pane_open(cp); }        /* the boot log's window, drawn whole again as a wallpaper change does */
    con_quiet = 1; uart_puts("login: desktop up\n");
}
static void login_event(struct input_event e) {          /* input_event, while the screen is up: keys only, nothing else gets in */
    if (e.type != 1 || e.code >= 272) return;             /* the mouse, the pointer and the sync marks do nothing here */
    if (e.code == 42 || e.code == 54 || e.code == 58) { ask_key(e.code, e.value); return; }   /* Shift and Caps keep ask.c's state, which ask_char reads */
    if (!e.value) return;
    int ch = e.code == 14 ? '\b' : e.code == 28 || e.code == 96 ? '\n' : e.code == 15 ? '\t' : ask_char(e.code);
    if (!ch) return;
    int r = login_key(ch);
    if (r == LOGIN_DONE) { login_done(); return; }
    if (r == LOGIN_REDRAW) {
        if (login_message()) { con_quiet = 1; uart_puts("login: wrong password\n"); }
        login_paint();
    }
}
static void login_tick(void) {                           /* the main loop: a logout asked for by the Terminal or the tree menu */
    if (!logout_flag) return;
    logout_flag = 0;
    if (!accounts_count()) return;
    if (spot_live) spot_close();
    tree_close_all();
    login_show();
}
