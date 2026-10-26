/* desk.c: cut out of main.c (the god-file split). The tree menu, the menu bar and its clock, the boot splash and the desktop's first paint. Built as part of main.o (the Makefile's MAINSRC) with
   main.c's own flags; what it shares with main.c is declared in kmain.h. */
#include "kmain.h"
int ask_busy(void);   /* ask.c: a `sam` request is queued or waiting on the relay */
#include "power.h"
#ifdef POWER_FILE_TEST   /* the power check's output catcher: what a Terminal command printed, searched for a word */
char seen[256];
void grab(const char *t) { unsigned k = 0; while (seen[k]) k++; for (; *t && k + 1 < sizeof seen; t++) seen[k++] = *t; seen[k] = 0; }
int has(const char *w) { for (unsigned i = 0; seen[i]; i++) { unsigned j = 0; while (w[j] && seen[i + j] == w[j]) j++; if (!w[j]) return 1; } return 0; }
#endif
/* The reboot and shutdown commands are refused only while a `sam` request is live. They must not use ask_pending(): that is
   also true while the typed line that holds this very command runs, so every typed reboot would be refused (found on the Pi). */
static int power_refused(void) {
    if (!ask_busy()) return 0;
    uart_puts("Wait for Samantha to finish, then try again.\n");
    return 1;
}
#ifdef PI_BUILD
void power_run(int off) {   /* the same action the confirmed chord runs */
    if (!power_refused()) power_apply(off);
}
#else
void power_run(int off) { if (!power_refused()) uart_puts(off ? "power: shutdown (this build has no power control)\n" : "power: restart (this build has no power control)\n"); }   /* the virt test build: only the Pi has a watchdog to pull */
#endif
int ask_edit(unsigned code);   /* ask.c: C X V A on the Terminal's input line */
int front_close(void) {   /* the window in front, topmost first, ended the way its red dot ends it; always 1 (the chord is the desktop's) */
    if (spot_live) spot_close();
    else if (brick_live) brick_close();
    else if (clock_live) clock_close();
    else if (browser_live) browser_close();
    else if (app_front) { app_push(2, 16, -16); if (!app_close_at) app_close_at = cnt_now() + cnt_hz() / 4; }
    else if (calc_front) calc_close();
    else if (con_live) console_close();
    return 1;
}
/* ---- The tree menu: click the mark or "Joshua Tree" at the left of the menu bar (or F10, Ctrl+Option+T) for About, Restart,
   Shut Down and Log Out, in Spotlight's cream card. Restart and Shut Down are the confirmed chords' own power_ask (the
   Terminal, the Wait-for-Samantha refusal, Enter confirms); About shows the version and closes on any key. ---- */
#ifndef JT_VERSION
#define JT_VERSION "dev"
#endif
static int tree_sel, tree_about;
static const char *const tree_rows[4] = { "About Joshua Tree", "Restart", "Shut Down", "Log Out" };
static int tree_ry(int r) { return spot_ly + (r ? 37 + (r - 1) * SPOT_ROW : 6); }
int tree_bar_hit(void) {   /* the pointer is on the mark or the title, physical px like the bar's own drawing */
    if (!text_ok || (int)mouse_y >= sg(MENUBAR_H) - 1) return 0;
    return (int)mouse_x < sg(40) + text_width(1, "Joshua Tree", sg(130)) + sg(6);
}
static void tree_paint(void) {
    int s = (int)window_scale(), w = TREE_W * s, h = TREE_H * s, x = spot_lx * s, y = spot_ly * s;
    cur_hide();
    for (int j = 0; j < h; j++) for (int i = 0; i < w; i++) fb[(unsigned)(y + j) * fb_pitch + (unsigned)(x + i)] = spot_under[j * w + i];
    if (tree_about) {
        gui_rounded_rect_on_wallpaper(spot_lx, spot_ly, TREE_W, 82, 0x00faf8f4, 10);
        gui_text("Joshua Tree", spot_lx + 16, spot_ly + 10, 0x001C1C1E);
        gui_text("Version " JT_VERSION, spot_lx + 16, spot_ly + 32, 0x0075726E);
        gui_text("Made by Joshua, from scratch.", spot_lx + 16, spot_ly + 54, 0x001C1C1E);
    } else {
        gui_rounded_rect_on_wallpaper(spot_lx, spot_ly, TREE_W, 109, 0x00faf8f4, 10);
        gui_hairline_h(spot_lx + 10, spot_ly + 31, TREE_W - 20, 0x00DDD8CE);
        for (int r = 0; r < 4; r++) {
            int ry = tree_ry(r), on = r == tree_sel;
            if (on) window_rect(spot_lx + 8, ry, TREE_W - 16, SPOT_ROW - 2, 0x00b5502c);
            gui_text(tree_rows[r], spot_lx + 22, ry + 2, on ? 0x00FFFFFF : 0x001C1C1E);
        }
    }
    fb_flush(x, y, w, h);
    cur_show();
    con_quiet = 1; uart_puts(tree_about ? "treemenu: about\n" : "treemenu: row "); if (!tree_about) { uart_dec((unsigned)tree_sel); uart_putc('\n'); }
}
void tree_open(void) {
    if (spot_live || !fb) return;
    int s = (int)window_scale(), w = SPOT_W * s, h = spot_hmax() * s;   /* the buffer Spotlight sizes; the tree's card is smaller */
    spot_lx = 8; spot_ly = sg(MENUBAR_H) / s + 2;
    if (!spot_under) spot_under = kmalloc((unsigned)(w * h) * 4);
    if (!spot_under) { uart_puts("oom treemenu\n"); return; }
    w = TREE_W * s; h = TREE_H * s;
    cur_hide();
    for (int j = 0; j < h; j++) for (int i = 0; i < w; i++) spot_under[j * w + i] = fb[(unsigned)(spot_ly * s + j) * fb_pitch + (unsigned)(spot_lx * s + i)];
    spot_live = spot_tree = 1; tree_sel = 0; tree_about = 0;
    con_quiet = 1; uart_puts("treemenu open\n");
    tree_paint();
}
/* Log Out closes every window; with no accounts it says so on the menu bar, with accounts the login screen (login_ui.h) comes back. */
void tree_close_all(void) {
    power_pending = 0; if (dock_sel >= 0) dock_select(-1);
    calc_close(); brick_close(); clock_close(); browser_close(); app_kill("logged out"); console_close();
}
void logout_request(void);   /* login_ui.h */
static void tree_logout(void) {
    con_quiet = 1; uart_puts("treemenu: logout\n");
    if (accounts_count()) { logout_request(); return; }   /* 4.9.0: with accounts the main loop closes everything and shows the login screen */
    tree_close_all();
    menubar_notify("Logged out");
}
static void tree_pick(int r) {
    if (r == 0) { tree_about = 1; tree_paint(); return; }
    spot_close();
    con_quiet = 1; uart_puts(r == 1 ? "treemenu: restart\n" : r == 2 ? "treemenu: shutdown\n" : "");
    if (r == 1) power_ask(0); else if (r == 2) power_ask(1); else tree_logout();
}
void tree_key(unsigned code) {
    if (tree_about || code == 1) { spot_close(); return; }   /* About closes on any key; Esc closes the menu */
    if (code == 103) { tree_sel = (tree_sel + 3) % 4; tree_paint(); }
    else if (code == 108) { tree_sel = (tree_sel + 1) % 4; tree_paint(); }
    else if (code == 28 || code == 96) tree_pick(tree_sel);
}
void tree_click(int lx, int ly) {
    if (tree_about || !in_box(lx, ly, spot_lx, spot_ly, TREE_W, 109)) { spot_close(); return; }   /* outside the card, or any click on About, dismisses */
    for (int r = 0; r < 4; r++) if (in_box(lx, ly, spot_lx + 8, tree_ry(r), TREE_W - 16, SPOT_ROW)) { tree_sel = r; tree_pick(r); return; }
}
void tree_hover(int lx, int ly) {
    if (tree_about) return;
    for (int r = 0; r < 4; r++) if (in_box(lx, ly, spot_lx + 8, tree_ry(r), TREE_W - 16, SPOT_ROW) && r != tree_sel) { tree_sel = r; tree_paint(); return; }
}
void tui_log(const char *s) { con_quiet = 1; uart_puts(s); }   /* ask.c: the chat view's "tui| ..." lines, UART only (one line, newline included) */
int volume_key(unsigned code);   /* cmd.c: F11 and F12, Volume Down, Up and Mute */
int ui_key(unsigned code) {   /* a key down, before any pane sees it: 1 if the desktop took it */
    int chord = ctrl_held || gui_held || alt_held;
    if (power_key(code)) return 1;
    if (volume_key(code)) return 1;   /* before Spotlight and every pane, like the Spotlight shortcut */
    if (gui_held && (code == 17 || code == 16)) return front_close();   /* Cmd+W closes the front window, Cmd+Q quits the front app: every window here is one app's, so both end it */
    if ((gui_held || ctrl_held) && code == 15) return app_cycle();   /* Cmd+Tab, or Ctrl+Tab on a keyboard with no Cmd */
    if (code == 59 || (gui_held && code == 57)) { if (spot_live) spot_close(); else spot_open(); return 1; }   /* F1 or Cmd+Space toggle, like a Mac; Ctrl/Alt+Space and a bare Space do nothing (Joshua, 2026-10-11, on the board) */
    if (code == 68 || (ctrl_held && alt_held && code == 20)) { int was = spot_live && spot_tree; spot_close(); if (!was) tree_open(); return 1; }   /* F10 or Ctrl+Option+T: the tree menu, for a board with no mouse; again closes it */
    if (code == 60 || (ctrl_held && code == 20)) { spot_close(); settings_close(); brick_close(); clock_close(); browser_close(); pane_open(&term_p); return 1; }   /* F2, Ctrl+T: the Terminal in front, an open Calculator behind it */
    if (spot_live) return spot_key(code);
    if (brick_live) return brick_key(code, 1);
    if (clock_live) { if (code == 1) clock_close(); return 1; }
    if (settings_live) return settings_key(code);
    if (browser_live) return browser_key(code);
    if (calc_front || app_front) return 0;
    if (code == 61 && term_front()) {   /* F3: the other Terminal session; never move a running request. */
        unsigned next = term_index ^ 1;
        if (ask_select(next)) {
            term_index = next;
            pane_open(&term_p);
            uart_puts(next ? "terminal session 2\n" : "terminal session 1\n");
        }
        return 1;
    }
    if (term_front() && (ctrl_held || gui_held) && ask_edit(code)) return 1;   /* Cmd or Ctrl with C X V A on the input line */
    if (term_front()) { if (code == 1 && !ask_pending()) { console_open(); return 1; } return 0; }   /* Esc: the keys back to the desktop */
    if (!fb) return 0;
    if (code == 105 || code == 106) { dock_select(dock_sel < 0 ? (code == 106 ? 0 : GUI_ICON_COUNT - 1) : (dock_sel + (code == 106 ? 1 : GUI_ICON_COUNT - 1)) % GUI_ICON_COUNT); return 1; }
    if ((code == 28 || code == 96) && dock_sel >= 0) { int slot = dock_sel; dock_select(-1); dock_activate(slot); return 1; }
    if (code == 1) { if (dock_sel >= 0) dock_select(-1); return 1; }   /* Esc with nothing open does nothing */
    /* The bare desktop: no pane has the keys (the Console is logs only). A letter or digit opens Spotlight with it
       typed, so a keyboard whose F-keys or Cmd never arrive still gets in. Space, Enter and Tab alone do nothing. */
    if (chord) return 0;
    int ch = ask_char(code);
    if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9')) { spot_open(); if (spot_live) spot_key(code); return 1; }
    return 0;
}

/* The dev card's key debug line (KEY_DEBUG, set by JT_KEY_DEBUG=1 builds only): for 3 seconds after a key, the menu bar
   shows the last raw HID report in hex ("m" in front for a media interface) and the decoded key, so a real-board test
   shows exactly what the keyboard sends. The UART gets the same line ("keydbg: ..."). Release builds compile it out. */
#ifdef KEY_DEBUG
#define KD_X 180                      /* on the 960x540 grid: the menu bar's middle, clear of Spotlight and the status half */
#define KD_W 296
static char kd_hex[48], kd_name[32]; static int kd_dirty, kd_shown; static unsigned long kd_until; static unsigned *kd_under;
static unsigned long kd_ms(void) {
    unsigned long f, c; __asm__ volatile ("mrs %0, cntfrq_el0\n mrs %1, cntpct_el0" : "=r"(f), "=r"(c));
    return c / ((f ? f : 54000000) / 1000);
}
void key_raw(const unsigned char *r, unsigned len, unsigned kind) {   /* xhci.c: every keyboard or media report */
    static unsigned prev; unsigned n = 0, set = 0;
    for (unsigned i = 0; i < len; i++) set += r[i] != 0;
    int release = set < prev || !set; prev = set;
    if (release) return;              /* fewer bytes set: a key let go; keep showing the press */
    if (kind == 3) { kd_hex[n++] = 'm'; kd_hex[n++] = ' '; }
    for (unsigned i = 0; i < len && n + 4 < sizeof kd_hex; i++) {
        kd_hex[n++] = "0123456789abcdef"[r[i] >> 4]; kd_hex[n++] = "0123456789abcdef"[r[i] & 15]; kd_hex[n++] = ' ';
    }
    kd_hex[n] = 0; kd_name[0] = 0; kd_dirty = 1;
}
static void kd_cat(const char *t) { unsigned n = 0; while (kd_name[n]) n++; while (*t && n + 1 < sizeof kd_name) kd_name[n++] = *t++; kd_name[n] = 0; }
void kd_key(unsigned code) {   /* a key down: its name, with the modifiers held */
    static const char *const fk[] = { "F1", "F2", "F3", "F4", "F5", "F6", "F7", "F8", "F9", "F10" };
    kd_name[0] = 0;
    if (ctrl_held && code != 29 && code != 97) kd_cat("Ctrl+");
    if (gui_held && code != 125 && code != 126) kd_cat("Cmd+");
    if (alt_held && code != 56 && code != 100) kd_cat("Alt+");
    int ch = ask_char(code); char one[2] = { (char)ch, 0 };
    kd_cat(code >= 59 && code <= 68 ? fk[code - 59] : code == 87 ? "F11" : code == 88 ? "F12" : code == 57 ? "Space" :
           code == 28 || code == 96 ? "Enter" : code == 1 ? "Esc" : code == 15 ? "Tab" : code == 14 ? "Backspace" :
           code == 29 || code == 97 ? "Ctrl" : code == 125 || code == 126 ? "Cmd" : code == 56 || code == 100 ? "Alt" :
           code == 42 || code == 54 ? "Shift" : code == 58 ? "Caps" : code == 103 ? "Up" : code == 108 ? "Down" : code == 105 ? "Left" :
           code == 106 ? "Right" : ch > ' ' && ch < 127 ? one : "?");
    if (kd_name[0] == '?') { kd_name[0] = 0; kd_cat("code "); char d[4] = { (char)('0' + code / 100 % 10), (char)('0' + code / 10 % 10), (char)('0' + code % 10), 0 }; kd_cat(d); }
    kd_dirty = 1;
}
void keydbg_tick(void) {   /* the poll loop: draw a new line, or take an old one down after 3 seconds */
    if (!fb) return;
    int x = sg(KD_X), w = sg(KD_W), h = sg(MENUBAR_H) - 1;
    if (kd_shown && !kd_dirty && kd_ms() > kd_until) {
        cur_hide();
        for (int j = 0; j < h; j++) for (int i = 0; i < w; i++) fb[(unsigned)j * fb_pitch + (unsigned)(x + i)] = kd_under[j * w + i];
        fb_flush(x, 0, w, h);
        kd_shown = 0;
    }
    if (!kd_dirty) return;
    kd_dirty = 0;
    if (!kd_under) kd_under = kmalloc((unsigned)(w * h) * 4);
    if (!kd_under) return;
    cur_hide();
    if (!kd_shown) for (int j = 0; j < h; j++) for (int i = 0; i < w; i++) kd_under[j * w + i] = fb[(unsigned)j * fb_pitch + (unsigned)(x + i)];
    fb_rect(x, 0, w, h, 0x00141311);
    char line[96]; unsigned n = 0;
    for (const char *t = "key "; *t; ) line[n++] = *t++;
    for (const char *t = kd_hex; *t && n < 80; ) line[n++] = *t++;
    for (const char *t = kd_name; *t && n < 95; ) line[n++] = *t++;
    line[n] = 0;
    int s = (int)window_scale();
    gui_text(line, x / s + 6, (h / s - 13) / 2, 0x00faf8f4);
    fb_flush(x, 0, w, h);
    con_quiet = 1; uart_puts("keydbg: "); uart_puts(line); uart_putc('\n');
    kd_shown = 1; kd_until = kd_ms() + 3000;
}
#else
void key_raw(const unsigned char *r, unsigned len, unsigned kind) { (void)r; (void)len; (void)kind; }
void kd_key(unsigned code) { (void)code; }
void keydbg_tick(void) {}
#endif
#ifdef KEY_SELFTEST
/* keydbg-kernel8.elf only (arm64-keydbg-check.py): made-up HID reports through xhci.c's parser, since QEMU's usb-kbd
   never sends a report ID. Each step prints a marker, then the UART shows what the keys did. */
void hid_kbd(const unsigned char *r, unsigned len, unsigned char *p);
void hid_media(const unsigned char *r, unsigned len);
void key_selftest(void) {
    static unsigned char last[8];
    static const unsigned char id_ctrl_t[9] = { 2, 0x01, 0, 0x17 }, id_none[9] = { 2 }, id_esc[9] = { 2, 0, 0, 0x29 };   /* ID 2 reads as Shift if not skipped */
    static const unsigned char cmd_space[8] = { 0x08, 0, 0x2C }, ctrl_space[8] = { 0x01, 0, 0x2C }, space[8] = { 0, 0, 0x2C }, none[8] = { 0 }, esc[8] = { 0, 0, 0x29 };
    uart_puts("keytest: id9 ctrl+t\n"); hid_kbd(id_ctrl_t, 9, last); hid_kbd(id_none, 9, last);
    uart_puts("keytest: id9 esc\n"); hid_kbd(id_esc, 9, last); hid_kbd(id_none, 9, last);
    uart_puts("keytest: boot8 space\n"); hid_kbd(space, 8, last); hid_kbd(none, 8, last);                 /* a bare Space and Ctrl+Space must not open Spotlight */
    uart_puts("keytest: boot8 ctrl+space\n"); hid_kbd(ctrl_space, 8, last); hid_kbd(none, 8, last);
    uart_puts("keytest: boot8 cmd+space\n"); hid_kbd(cmd_space, 8, last); hid_kbd(none, 8, last);
    uart_puts("keytest: boot8 esc\n"); hid_kbd(esc, 8, last); hid_kbd(none, 8, last);
    static const unsigned char br_down[3] = { 3, 0x70, 0 }, br_up[3] = { 3, 0x6F, 0 };   /* a Mac-mode F1 and F2 */
    uart_puts("keytest: media f1\n"); hid_media(br_down, 3);
    uart_puts("keytest: media esc\n"); hid_kbd(esc, 8, last); hid_kbd(none, 8, last);
    uart_puts("keytest: media f2\n"); hid_media(br_up, 3);
    uart_puts("keytest: media esc2\n"); hid_kbd(esc, 8, last); hid_kbd(none, 8, last);
    static const unsigned char vol_dn[3] = { 3, 0xEA, 0 }, vol_up[3] = { 3, 0xE9, 0 }, vol_mute[3] = { 3, 0xE2, 0 };
    uart_puts("keytest: media vol down\n"); hid_media(vol_dn, 3);
    uart_puts("keytest: media vol up\n"); hid_media(vol_up, 3);
    uart_puts("keytest: media mute\n"); hid_media(vol_mute, 3);
    uart_puts("keytest: media unmute\n"); hid_media(vol_mute, 3);
    uart_puts("keytest: done\n");
}
#endif

unsigned long net_clock_utc(void);   /* ip.c: UTC seconds once the network has told us, 0 before */
#include "notice.h"
unsigned notice_now(void) {
    unsigned long f, c;
    __asm__ volatile ("mrs %0, cntfrq_el0\n mrs %1, cntpct_el0" : "=r"(f), "=r"(c));
    return (unsigned)(c / ((f ? f : 54000000) / 100));
}
struct menu_notice notice;
int notice_up(void) { return notice.text[0] != 0; }   /* boot.c: a notice is showing, so the loop keeps polling */
static unsigned *mb_save; static int mb_h, mb_x0;
static int mb_hit[4][2];   /* the status icons' columns in physical pixels: power, sound, Wi-Fi, clock (menubar_wifi sets them) */
static int mb_wifi = -1;   /* the Wi-Fi state last shown (0 off, 1 working, 2 connected), so the clock can redraw alone each minute */
static int clock_minute = -1;
/* Vancouver stays UTC-7 from March 8, 2026. Earlier dates retain the seasonal rule. */
unsigned long civil_days(long y, unsigned m, unsigned d) {
    y -= m <= 2; long era = (y >= 0 ? y : y - 399) / 400; unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1, doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return (unsigned long)(era * 146097 + (long)doe - 719468);
}
static void civil_from_days(unsigned long z, long *y, unsigned *m, unsigned *d) {
    z += 719468; unsigned long era = z / 146097; unsigned doe = (unsigned)(z - era * 146097);
    unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365; long yy = (long)yoe + (long)era * 400;
    unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100), mp = (5 * doy + 2) / 153;
    *d = doy - (153 * mp + 2) / 5 + 1; *m = mp < 10 ? mp + 3 : mp - 9; *y = yy + (*m <= 2);
}
static unsigned long nth_sunday_utc(long y, unsigned mon, unsigned nth, unsigned hour) {   /* the nth Sunday of a month, at hour UTC */
    unsigned long d1 = civil_days(y, mon, 1); unsigned dow = (unsigned)((d1 + 4) % 7);   /* 1970-01-01 was a Thursday; 0 = Sunday */
    unsigned first = dow == 0 ? 1 : 1 + (7 - dow);
    return (civil_days(y, mon, first + (nth - 1) * 7)) * 86400UL + hour * 3600UL;
}
long clock_offset(unsigned long utc) {   /* Vancouver: 7 h behind UTC in daylight time, 8 h otherwise */
    long y; unsigned m, d; civil_from_days(utc / 86400, &y, &m, &d);
    int dst = utc >= civil_days(2026, 3, 8) * 86400UL + 10 * 3600UL
           || (utc >= nth_sunday_utc(y, 3, 2, 10) && utc < nth_sunday_utc(y, 11, 1, 9));
    return (dst ? 7 : 8) * 3600L;
}
void clock_local(unsigned long utc, unsigned *hh, unsigned *mm, unsigned *mon, unsigned *day) {
    long y;
    unsigned long loc = utc - (unsigned long)clock_offset(utc);
    civil_from_days(loc / 86400, &y, mon, day);
    clock_year = y;
    *hh = (unsigned)(loc % 86400) / 3600; *mm = (unsigned)(loc % 3600) / 60;
}
#include "clock_ui.h"
#include "brick_ui.h"
#include "browser_ui.h"
#include "glyphs.h"
#include "settings_ui.h"
/* The right end of the menu bar: the clock in 12-hour form, and left of it the Wi-Fi fan, no name. The fan, the speaker and the power
   symbol are all flat ink, the same 0x001C1C1E as the clock, in every state.
   Each arc is anti-aliased (4x4 samples a pixel, blended onto the bar), so it stays crisp at scale 1 and 2. Each call puts the
   saved bare bar back first, so old pixels never show through. */
#define WIFI_INK 0x001C1C1E
static int mb_mic;
void menubar_mic(int on) {   /* ask.c: voice mode started or ended; the bar is redrawn with or without the mark */
    if (mb_mic == (on != 0)) return;
    mb_mic = on != 0;
    uart_puts(mb_mic ? "mic mark on\n" : "mic mark off\n");
    if (!fb || !text_ok) return;
    cur_hide(); menubar_wifi(mb_wifi); cur_show();
}
void menubar_wifi(int state) {
    mb_wifi = state;   /* kept even before the desktop exists (the boot splash), so its first paint shows the right fan */
    if (!fb || !text_ok || login_live) return;   /* the login screen is over the bar: wall_repaint draws it again when it goes */
    int W = (int)fb_w, H = (int)fb_h, mb = sg(MENUBAR_H);
    if (mb_save) for (int y = 0; y < mb_h; y++) for (int x = mb_x0; x < W; x++) fb[(unsigned)y * fb_pitch + (unsigned)x] = mb_save[y * (W - mb_x0) + (x - mb_x0)];
    char clkbuf[12] = "--:--"; const char *clk = clkbuf;   /* the clock slot: dashes until the network has told us the time */
    unsigned long u = net_clock_utc();
    if (u) {
        unsigned h, m, mo, dd; clock_local(u, &h, &m, &mo, &dd); unsigned h12 = h % 12 ? h % 12 : 12; unsigned n = 0;
        if (h12 >= 10) clkbuf[n++] = '1';
        clkbuf[n++] = (char)('0' + h12 % 10); clkbuf[n++] = ':'; clkbuf[n++] = (char)('0' + m / 10); clkbuf[n++] = (char)('0' + m % 10);
        clkbuf[n++] = ' '; clkbuf[n++] = h >= 12 ? 'P' : 'A'; clkbuf[n++] = 'M'; clkbuf[n] = 0;
        clock_minute = (int)(h * 60 + m);
    }
    int cx = W - sg(16) - text_width(2, clk, sg(120));
    text_draw(2, clk, cx, (mb + sg(8)) / 2, sg(120), fb_color(0x001C1C1E), fb, fb_pitch, W, H);
    int R = sg(11), th = sg(3) / 2 < 2 ? 2 : sg(3) / 2, dr = sg(3) / 2, ox = cx - sg(16) - R, oy = mb / 2 + sg(5);   /* the classic fan: a dot, then three arcs above it */
    unsigned ink = fb_color(WIFI_INK);   /* the clock's ink in every state, joined or not (3.19.1: no grey icons) */
    for (int y = -R - 1; y <= dr + 1; y++) for (int x = -R - 1; x <= R + 1; x++) {
        int hit = 0;
        for (int j = 0; j < 4; j++) for (int i = 0; i < 4; i++) {   /* a sample's offset from the dot's centre, in eighths of a pixel */
            int X = 8 * x + 2 * i - 3, Y = 8 * y + 2 * j - 3, d2 = X * X + Y * Y, in = d2 <= 64 * dr * dr;
            if (!in && Y < 0 && X <= -Y && -X <= -Y) for (int k = 1; k <= 3; k++) { int ro = 8 * sg(3 * k + 2) - 4, ri = ro - 8 * th; if (d2 <= ro * ro && d2 >= ri * ri) in = 1; }   /* 45 degrees either side of straight up */
            hit += in;
        }
        if (!hit) continue;
        unsigned *d = fb + (unsigned)(oy + y) * fb_pitch + (unsigned)(ox + x), o = 0;
        for (int b = 0; b <= 16; b += 8) o |= (((ink >> b & 0xFF) * (unsigned)hit + (*d >> b & 0xFF) * (16u - (unsigned)hit)) / 16) << b;
        *d = o;
    }
#ifdef WIFI_ICON_TEST   /* the wifiicon check's builds: where the fan is, for arm64-quietboot-check.py to look at its pixels */
    uart_puts("wifi icon "); uart_dec((unsigned)ox); uart_putc(','); uart_dec((unsigned)oy); uart_putc(','); uart_dec((unsigned)R); uart_putc('\n');
#endif
    /* 3.13.0: two more status icons left of the fan, a speaker (dim and without waves at volume 0) and a power symbol. A click on
       any of them, the fan and the clock too, opens Settings at its section (menubar_click). */
    int isz = sg(16), sx = ox - R - sg(14) - isz, wx = sx - sg(12) - isz, iy = (mb - 1 - isz) / 2, vol = audio_volume();
    glyph_paint(GL_SOUND, vol == 0 ? 0 : vol <= 50 ? 1 : 2, sx, iy, isz, WIFI_INK);   /* ink at every volume: the waves say the level, not a grey */
    glyph_paint(GL_POWER, 0, wx, iy, isz, WIFI_INK);
    if (mb_mic) {   /* voice mode is listening (ask.c): a terracotta microphone left of the power symbol, a capsule on a stem */
        int mx = wx - sg(12) - isz, cw = isz * 2 / 5, ch = isz * 3 / 5, cx0 = mx + (isz - cw) / 2;
        unsigned t = fb_color(0x00B5502C);
        fb_rect(cx0, iy, cw, ch, t);
        fb_rect(mx + isz / 2 - sg(1), iy + ch, sg(2), isz - ch - sg(2), t);
        fb_rect(cx0 - sg(1), iy + isz - sg(2), cw + sg(2), sg(2), t);
    }
    mb_hit[0][0] = wx - sg(5); mb_hit[0][1] = wx + isz + sg(5);         /* power */
    mb_hit[1][0] = sx - sg(5); mb_hit[1][1] = sx + isz + sg(5);         /* sound */
    mb_hit[2][0] = ox - R - sg(5); mb_hit[2][1] = ox + R + sg(5);       /* Wi-Fi */
    mb_hit[3][0] = cx - sg(5); mb_hit[3][1] = W;                        /* the clock */
    {   static int said[4][2];   /* where the icons are, said when they move (the check clicks them; the clock's width changes with the minute) */
        int moved = 0;
        for (int i = 0; i < 4; i++) for (int j = 0; j < 2; j++) if (said[i][j] != mb_hit[i][j]) { said[i][j] = mb_hit[i][j]; moved = 1; }
        if (moved) {
            static const char *const nm[4] = { "power", "sound", "wifi", "clock" };
            con_quiet = 1; uart_puts("menubar icons");
            for (int i = 0; i < 4; i++) { uart_putc(' '); uart_puts(nm[i]); uart_putc(' '); uart_dec((unsigned)mb_hit[i][0]); uart_putc('-'); uart_dec((unsigned)mb_hit[i][1]); }
            uart_putc('\n');
        }
    }
    int nx = mb_x0 + sg(12), nw = wx - sg(16) - nx;
    if (notice.text[0] && nw > 0)
        text_draw(2, notice.text, nx, (mb + sg(8)) / 2, sg(120), fb_color(0x001C1C1E), fb, fb_pitch, nx + nw, H);
    fb_flush(mb_x0, 0, W - mb_x0, mb);
}
int menubar_click(int x, int y) {
    static const int section[4] = { SET_ABOUT, SET_SOUND, SET_ABOUT, SET_TIME };
    static const char *const name[4] = { "power", "sound", "wifi", "clock" };
    if (!fb || y >= mb_h) return 0;
    for (int i = 0; i < 4; i++) if (mb_hit[i][1] > mb_hit[i][0] && x >= mb_hit[i][0] && x < mb_hit[i][1]) {
        con_quiet = 1; uart_puts("menubar click "); uart_puts(name[i]); uart_putc('\n');
        settings_open(section[i]);
        return 1;
    }
    return 0;
}
void menubar_notify(const char *message) {
    notice_set(&notice, message, notice_now());
    if (login_live) { uart_puts("notice: "); uart_puts(notice.text); uart_putc('\n'); return; }
    cur_hide(); menubar_wifi(mb_wifi); cur_show();
    if (settings_live && !spot_live) set_paint();   /* the volume keys come through here: the Sound page shows the new level */
    uart_puts("notice: "); uart_puts(notice.text); uart_putc('\n');
}
static void cal_tick(void) {   /* the day has changed (or the clock just arrived): the Calendar tile again, the hover band's copy with it */
    if (!fb || login_live || dock_mag_on() || cal_today() == cal_shown) return;
    int s = (int)window_scale(), h = hover_slot, top = dock_band_y(), rows = (int)fb_h - top;
    if (dock_band) dock_hover(-1);   /* the plain band back first, so the copy below holds no label */
    cur_hide();
    dock_tile(3, 0);   /* slot 3, Calendar */
    fb_flush(gui_slot_x(3) * s, gui_dock_y0() * s, DOCK_ICON * s + s, (DOCK_PAD + DOCK_ICON) * s + s);
    if (dock_band) for (unsigned i = 0; i < (unsigned)rows * fb_pitch; i++) dock_band[i] = fb[(unsigned)top * fb_pitch + i];
    cur_show();
    if (h >= 0) dock_hover(h);
    uart_puts("calendar tile redrawn\n");
}
void menubar_tick(void) {   /* the poll loop calls this; it redraws only when the minute has changed */
    if (notice_expire(&notice, notice_now())) {
        cur_hide(); menubar_wifi(mb_wifi); cur_show();
        uart_puts("notice cleared\n");
    }
    cal_tick();
    clock_tick();
    if (settings_live && !spot_live && set_sec == SET_TIME && cal_utc() / 60 != (unsigned long)set_minute) set_paint();   /* the Date and Time page follows the minute */
    unsigned long u = net_clock_utc();
    if (!u || mb_wifi < 0) return;
    unsigned h, m, mo, dd; clock_local(u, &h, &m, &mo, &dd);
    if ((int)(h * 60 + m) != clock_minute) menubar_wifi(mb_wifi);
}
/* The Pi's boot splash (QUIET_BOOT): from the first frame a solid light grey screen with the crisp tree mark centred and a thin
   grey line under it, one sixth longer as each stage of boot finishes (fb, sd, wifi, clock, bt, desktop). Nothing else is drawn,
   not the desktop, not the Console, until splash_finish: Wi-Fi joined or given up, the clock synced or given up, Bluetooth
   up or given up. Then fb_desktop paints the desktop once. The UART says `splash step N name` and `boot done`. */
#ifdef QUIET_BOOT
static int splash_pm;   /* the bar's fill, in thousandths of the track */
static void splash_wait_ms(unsigned ms) {
    unsigned long f, c, end; __asm__ volatile ("mrs %0, cntfrq_el0\n mrs %1, cntpct_el0" : "=r"(f), "=r"(c));
    end = c + (f ? f : 54000000) / 1000 * ms;
    do __asm__ volatile ("mrs %0, cntpct_el0" : "=r"(c)); while (c < end);
}
static void splash_draw(int pm) {   /* 3.9.8: the fill is any width, not one of six */
    int W = (int)fb_w, H = (int)fb_h, bw = sg(140), bh = sg(4) > 2 ? sg(4) : 2, bx = (W - bw) / 2, by = H / 2 + sg(62);
    splash_pm = pm;
    fb_rect(bx, by, bw, bh, 0x00C8C8CC);              /* the track, a calm darker grey */
    fb_rect(bx, by, bw * pm / 1000, bh, 0x00636366);  /* the part of boot done, mid grey */
    fb_flush(bx, by, bw, bh);
}
static void splash_ease(int to) {   /* glide to a new fill in small frames, so the bar never jumps a third */
    int from = splash_pm;
    for (int i = 1; i <= 12; i++) {
        splash_draw(from + (to - from) * i / 12);   /* linear on purpose: progress glides evenly; lib/ease.h is for motion */
        uart_puts("splash bar "); uart_dec((unsigned)splash_pm); uart_putc('\n');
        splash_wait_ms(10);
    }
}
void splash_creep(void) {   /* inside a long wait (the radio loading): move toward the next stage as time passes, never past it */
    static unsigned long t0;
    unsigned long f, c; __asm__ volatile ("mrs %0, cntfrq_el0\n mrs %1, cntpct_el0" : "=r"(f), "=r"(c));
    if (!splash_on) return;
    if (!t0) t0 = c;
    unsigned long ms = (c - t0) / ((f ? f : 54000000) / 1000);
    int pm = 667 + (int)(166 * ms / (ms + 6000));
    if (pm >= splash_pm + 4) splash_draw(pm);
}
void splash_step(int n, const char *name) {
    if (!splash_on) return;
    splash_ease(1000 * n / 6);
    uart_puts("splash step "); uart_dec((unsigned)n); uart_putc(' '); uart_puts(name); uart_putc('\n');
#ifdef SPLASH_STEP_MS   /* the splash checks: hold each step so a screendump can catch it */
    splash_wait_ms(SPLASH_STEP_MS);
#endif
}
static void splash_begin(void) {
    int W = (int)fb_w, H = (int)fb_h;
    fb_rect(0, 0, W, H, 0x00E5E5E7);
    fb_flush(0, 0, W, H);
    splash_mark(W / 2, H / 2 - sg(30), sg(150), 0x001C1C1E, 0x00E5E5E7);
    splash_on = 1;
    uart_puts("splash up\n");
    splash_step(1, "fb");
}
#endif
static int desktop_up;   /* fb_desktop has painted the desktop once: a wallpaper change may repaint it */
static int text_early;         /* the deferred boot started the text engine for the login card */
int desktop_deferred;   /* 4.10.0, the Pi's quiet boot: an account is on the card, so the login card goes up first and login_done paints the desktop */
static void menubar_base(void) {   /* the bar over bare wallpaper: half wallpaper, half white, per pixel, closed by a one pixel rule */
    int W = (int)fb_w, mb = sg(MENUBAR_H);
    for (int y = 0; y < mb - 1; y++) for (int x = 0; x < W; x++) {
        unsigned c = fb[(unsigned)y * fb_pitch + (unsigned)x];
        fb[(unsigned)y * fb_pitch + (unsigned)x] = ((c >> 1) & 0x007F7F7Fu) + 0x00808080u;   /* each colour lane: half itself plus half of 255 */
    }
    fb_rect(0, mb - 1, W, 1, MENUBAR_RULE);
}
static void menubar_keep(void) {   /* the bare right half of the bar, kept for menubar_wifi's redraws */
    int W = (int)fb_w;
    if (mb_save) for (int y = 0; y < mb_h; y++) for (int x = mb_x0; x < W; x++) mb_save[y * (W - mb_x0) + (x - mb_x0)] = fb[(unsigned)y * fb_pitch + (unsigned)x];
}
static void desktop_chrome(int band_y) {   /* the mark, the title, the status icons and the quiet line above the dock */
    int W = (int)fb_w, H = (int)fb_h, mb = sg(MENUBAR_H);
    if (text_ok) {
        menu_mark_paint(sg(24), mb / 2, sg(20), 0x001C1C1E);   /* the mark in the corner, the i386 menu bar's */
        text_draw(1, "Joshua Tree", sg(40), (mb + sg(8)) / 2, sg(130), fb_color(0x001C1C1E), fb, fb_pitch, W, H);   /* menu bar title: bold sans, 13 on the grid, ink */
        if (current_user[0]) text_draw(1, current_user, sg(40) + text_width(1, "Joshua Tree", sg(130)) + sg(14), (mb + sg(8)) / 2, sg(130), fb_color(0x00B5502C), fb, fb_pitch, W, H);   /* 4.9.0: who is signed in, after the title */
        menubar_wifi(mb_wifi < 0 ? 1 : mb_wifi);   /* replaces the ARM64 badge: wifi.c moves it along as the chip comes up */
        /* Steve Jobs died on 5 October 2011. Fifteen years on, one quiet line above the dock, ending on the title
           of the Steve Jobs Archive's book of his own words, which Joshua was reading that week. */
        const char *thanks = "Seduce. Vilify. Ignore.";
        int ar = sc(6), gapx = sc(7), tw = text_width(2, thanks, sc(110)), tx = (W - tw - 7 * ar / 2 - gapx) / 2, ty = band_y - sc(4);
        text_draw(2, thanks, tx, ty, sc(110), fb_color(0x00f0f4f8), fb, fb_pitch, W, H);
        {   /* a small bitten apple after the line: apple_mask scaled to 3.5 ar square, 4x4 samples a pixel, onto the flat fill */
            int S = 7 * ar / 2, x0 = tx + tw + gapx, y0 = ty - sc(4) + ar - S; unsigned ink = fb_color(0x00f0f4f8);
            for (int y = 0; y < S; y++) for (int x = 0; x < S; x++) {
                int sum = 0;
                for (int j = 0; j < 4; j++) for (int i = 0; i < 4; i++) sum += apple_mask[((y * 4 + j) * APPLE_N / (S * 4)) * APPLE_N + (x * 4 + i) * APPLE_N / (S * 4)];
                unsigned al = (unsigned)sum / 16, d = fb[(unsigned)(y0 + y) * fb_pitch + (unsigned)(x0 + x)], out = 0;
                if (!al) continue;
                for (int b = 0; b <= 16; b += 8) out |= ((((ink >> b) & 0xFF) * al + ((d >> b) & 0xFF) * (255 - al)) / 255) << b;
                fb[(unsigned)(y0 + y) * fb_pitch + (unsigned)(x0 + x)] = out;
            }
        }
    } else uart_puts(heap_oom ? "oom text\n" : "M1d text FAIL\n");
}
void fb_desktop(void) {
    int W = (int)fb_w, H = (int)fb_h;
    dock_pct_boot = dock_scale_pct;
    int win_w = sc(500), win_h = sc(350), win_x = (W - win_w) / 2, win_y = sc(100);
    int mb = sg(MENUBAR_H);                               /* the menu bar's height on this screen: 26 on the 960x540 grid */
#ifndef QUIET_BOOT
    {   /* the boot screen while the photo decodes (the slow part): the tree on off-white and a thin bar a third full,
           one of three steps done (screen up; the photo and the desktop follow). The wallpaper paints straight over it. */
        fb_rect(0, 0, W, H, 0x00faf8f4);
        menu_mark_paint(W / 2, H / 2 - sg(30), sg(110), 0x001C1C1E);
        int bw = sg(140), bh = sg(4) > 2 ? sg(4) : 2, bx = (W - bw) / 2, by = H / 2 + sg(50);
        fb_rect(bx, by, bw, bh, 0x00DDD8CE);
        fb_rect(bx, by, bw / 3, bh, 0x001C1C1E);
        fb_flush(0, 0, W, H);
    }
#endif
    int wall_ok = wall_paint(fb, fb_pitch, W, H, fb_swap);
    if (!wall_ok) fb_rect(0, 0, W, H, 0x00203040);   /* Navy (clrs.cc), see wall.c; this fallback is now unreachable */
    menubar_base();                                       /* the bar, half wallpaper and half white, closed by a one pixel rule */
    mb_h = mb - 1; mb_x0 = W / 2;                          /* keep the bare right half of the bar, for menubar_status() */
    mb_save = kmalloc((unsigned)(mb_h * (W - mb_x0)) * 4);
    menubar_keep();
    if (!text_early) text_ok = text_init();               /* before the window (its title); the deferred boot did it already for the login card */
    text_early = 0;
    dock_paint();                                         /* the i386 dock, while only the wallpaper is under it */
    int band_y = gui_dock_band_top() * (int)window_scale();   /* the top of the dock's band, room for a hover label */
    /* Slice 3: the Console wears the i386 window frame (gui_paint.c): rounded cream body on the wallpaper, traffic
       lights, centred name, a hairline under the title band. Its content well is white for the log. */
    int s = (int)window_scale(), lx = win_x / s, ly = win_y / s, lw = win_w / s, lh = win_h / s;
    win_lx = lx; win_ly = ly; win_lw = lw; win_lh = lh;
    off_reserve(lw * s, lh * s);
#ifdef BR_TRACE
    uart_puts(off_buf ? "browser: offscreen buffer reserved at boot\n" : "browser: offscreen buffer NOT reserved\n");
#endif
    dock_mem = kmalloc((unsigned)(((int)fb_h - dock_band_y()) * (int)fb_pitch) * 4);
    /* Slice 5: the Calculator's own frame, over the right of the Console's so both show, clear of the line above the dock */
    calc_lw = 280; calc_lx = lx + lw - 150; if (calc_lx + calc_lw > W / s - 8) calc_lx = W / s - 8 - calc_lw;
    calc_ly = ly + 40; calc_lh = gui_dock_band_top() - 22 - calc_ly; if (calc_lh > 300) calc_lh = 300;
    /* M3c part 2: an EL0 app's window, placed like the Calculator's, then moved up and cut to fit above the dock's band */
    /* Scale 1 keeps 576x400. At scale 2 (1080p) an app is the Console's size plus a little: its 1x buffer is smoothed up to 2x,
       so a window as big as the 576x400 one would be soft and look huge beside the Terminal. */
    app_lw = s >= 2 ? 520 : 576; if (app_lw > W / s - 16) app_lw = W / s - 16;
    app_lx = lx + lw - 150; if (app_lx + app_lw > W / s - 8) app_lx = W / s - 8 - app_lw;
    {   int top = mb / s + 8, bottom = gui_dock_band_top() - 22;
        app_lh = s >= 2 ? 368 : 400; app_ly = ly + 40;
        if (app_ly + app_lh > bottom) app_ly = bottom - app_lh;
        if (app_ly < top) { app_ly = top; app_lh = bottom - top; }
    }
    app_cw = (unsigned)(app_lw - 16); app_ch = (unsigned)(app_lh - 40);
    for (unsigned i = 1; i < APP_SLOTS; i++) {   /* 4.0: the other slots open cascaded down and to the left, a title band apart, so a second window leaves the first's title showing */
        struct app *o = &apps[i];
        o->lw = app_lw; o->lh = app_lh; o->cw = app_cw; o->ch = app_ch;
        o->lx = app_lx - (int)i * 36; if (o->lx < 8) o->lx = 8;
        o->ly = app_ly + (int)i * 28; if (o->ly + o->lh > gui_dock_band_top() - 22) o->ly = gui_dock_band_top() - 22 - o->lh;
    }
    win_def[WT_MAIN][0] = lw; win_def[WT_MAIN][1] = lh; win_def[WT_CALC][0] = calc_lw; win_def[WT_CALC][1] = calc_lh; win_def[WT_APP][0] = app_lw; win_def[WT_APP][1] = app_lh;
#ifndef QUIET_BOOT
    console_frame();
#endif
    desktop_chrome(band_y);
    con_layout(win_x, win_y, win_w, win_h);
    {   /* keep the log inside the frame's white well: under the title band's hairline, clear of the cream margins */
        int wx = (lx + 8) * s, wy = (ly + 30) * s + 2 * s, wr = (lx + lw - 8) * s, wb = (ly + lh - 8) * s;
        if (con_x < wx) con_x = wx;
        if (con_y < wy) con_y = wy;
        if (con_x + con_cols * con_cw > wr) con_cols = (wr - con_x) / con_cw;
        if (con_y + (con_rows + 1) * con_ch > wb) con_rows = (wb - con_y) / con_ch - 1;
    }
    con_start();
#ifdef DOCK_HOVER_TEST   /* the dockhover test build: the label over one slot, then (arm64-m1c-check.py) its pixels */
    dock_hover(DOCK_HOVER_TEST);
#endif
#ifdef CURSOR_TEST   /* the cursortest build: the pointer over dock slot 3 at boot, the 1080p (scale 2) arrow for arm64-mouse-check.py */
    mouse_x = (unsigned)((gui_slot_x(3) + DOCK_ICON / 2) * s); mouse_y = (unsigned)((gui_dock_y0() + DOCK_PAD + DOCK_ICON / 2) * s);
    pointer_moved();
#endif
    fb_flush(0, 0, W, H);   /* the whole still picture to the screen; con_glyph flushes as it goes from here */
    /* Blank spots, clear of any text: the bottom of the window's white well, the menu bar's rule, and the wallpaper (the
       design system's dark fill at three corners, not the old 0x203040 fallback). Grey and white read the same either
       byte order; the rule and the fill are checked in fb_color order. */
#ifdef QUIET_BOOT
    unsigned well = fb_color(wall_get());   /* no window was drawn: the spot where its well would end is bare wallpaper */
#else
    unsigned well = 0x00ffffff;
#endif
    int ok = fb[(unsigned)((ly + lh - 8) * s - 2) * fb_pitch + fb_w / 2] == well && fb[(unsigned)(mb - 1) * fb_pitch + fb_w / 2] == fb_color(MENUBAR_RULE);
    unsigned w0 = fb[(unsigned)sc(300) * fb_pitch + 50], w1 = fb[(unsigned)(H - sc(8)) * fb_pitch + fb_w - 50], w2 = fb[(unsigned)(mb + sc(40)) * fb_pitch + fb_w - 50];
    ok = ok && wall_ok && w0 == fb_color(wall_get()) && w1 == w0 && w2 == w0;
    uart_puts(ok ? "M1c fb ok\n" : "M1c fb FAIL\n");
    desktop_up = 1;
#ifdef QUIET_BOOT
    uart_puts("console quiet\n");
#endif
#ifdef DRAG_TEST   /* the dragtest build: a drag of 100 right and 60 down at boot, clamped like a real one, for arm64-drag-check.py at 1080p (scale 2) */
    pane_open(&con_p);   /* the Console is not up at boot: bring it up to be dragged */
    drag_on = 1; win_move_to(win_lx + 100, win_ly + 60); drag_end();
#endif
#ifdef APP_BOOT   /* the apptest build: that app opened at boot, for arm64-appsize-check.py at 1080p (the Pi model has no USB to click with). 4.0: "a,b,c" opens each in turn, for arm64-windows-check.py */
    int boot_multi = 0;
    {   const char *list = APP_BOOT;
        for (const char *c = list; *c; c++) if (*c == ',') boot_multi = 1;
        if (boot_multi) {
            char one[24]; unsigned n = 0;
            for (const char *c = list;; c++) {
                if (*c == ',' || !*c) { one[n] = 0; app_dock(one); n = 0; if (!*c) break; }
                else if (n < sizeof one - 1) one[n++] = *c;
            }
        }
    }
    if (!boot_multi && !app_dock(APP_BOOT)) {   /* 3.9.0: the kernel-drawn windows too (arm64-sharp2x-check.py), then their frame on the UART: WINBOX name x y w h, logical */
        const char *nm = APP_BOOT; int bx, by, bw, bh;
        #define NM_IS(w) ({ const char *a_ = nm, *b_ = w; while (*a_ && *a_ == *b_) { a_++; b_++; } !*a_ && !*b_; })
        if (NM_IS("calculator")) { calc_open(); bx = calc_lx; by = calc_ly; bw = calc_lw; bh = calc_lh; }
        else {
            if (NM_IS("settings")) settings_open(0);
            else if (NM_IS("clock")) clock_open();
            else if (NM_IS("brick")) brick_open();
            else if (NM_IS("browser")) browser_open();
            else if (NM_IS("terminal")) pane_open(&term_p);
            else console_open();
            bx = win_lx; by = win_ly; bw = win_lw; bh = win_lh;
        }
        uart_puts("WINBOX "); uart_puts(nm); uart_putc(' '); uart_dec((unsigned)bx); uart_putc(' '); uart_dec((unsigned)by); uart_putc(' '); uart_dec((unsigned)bw); uart_putc(' '); uart_dec((unsigned)bh); uart_putc('\n');
    }
#endif
#ifdef AUDIO_TEST   /* the audiotest build: `beep` run at boot on virt, which has no PWM (arm64-audio-check.py) */
    void cmd_run(const char *q, unsigned n, void (*out)(const char *));
    cmd_run("beep", 4, 0);
#endif
#ifdef CALC_TEST   /* the calctest build: the Calculator open at boot, four sums typed into it (arm64-calc-check.py) */
    calc_open();
    static const char *const sums[] = {"sin(30deg)", "5!", "2^10", "1/0"};
    for (unsigned i = 0; i < 4; i++) { calc_press("C"); for (const char *p = sums[i]; *p; p++) calc_type(*p); calc_did(calc_type('\n')); }
    calc_press("Sci"); calc_press("M+"); calc_did(0);   /* end on the scientific pad, memory held, for the screendump */
#endif
}
/* A new wallpaper colour (Settings, or a saved choice read once the card is up): the whole desktop again in the same order
   fb_desktop painted it, over the buffers it already holds (the heap never frees, so nothing is allocated here). The wallpaper,
   the bar and the dock go first; the pixels saved under the Calculator's and an app's windows are plain wallpaper, so they are
   refilled with the new colour (a window closed later must not paint the old one back); the hover band is forgotten so its next
   copy is taken from the new dock. Whatever window was open is drawn again on top. */
void wall_repaint(void) {
    int W = (int)fb_w, H = (int)fb_h, s = (int)window_scale();
    cur_hide();
    wall_paint(fb, fb_pitch, W, H, fb_swap);
    menubar_base();
    menubar_keep();
    dock_band = 0; hover_slot = -1; dock_sel = -1; dock_mag_forget();
    dock_paint();
    desktop_chrome(gui_dock_band_top() * s);
    fb_flush(0, 0, W, H);
    cur_show();
}
void desktop_wall_changed(void) {   /* settings.c: the colour is already set */
    if (!fb || !desktop_up) return;
    wall_repaint();
    if (settings_live) set_paint();
    else if (con_live) { con_live = 0; pane_open(cp); }
}
static void fb_init(void) {
    if (!fb_setup()) return;
    if ((unsigned long)fb_pitch * fb_h * 4 <= FB_RAM_MAX) {   /* 4.3.0: draw into RAM from here on; fb_flush copies to the scanout */
        fb_scan = fb; fb = (unsigned *)FB_RAM_BASE;
        for (unsigned long i = 0; i < (unsigned long)fb_pitch * fb_h; i++) fb[i] = fb_scan[i];
    }
    mouse_x = fb_w / 2; mouse_y = fb_h / 2;               /* the pointer starts in the middle of the screen */
#ifdef QUIET_BOOT
    splash_begin();                                       /* the desktop waits for splash_finish */
#else
    fb_desktop();
#endif
}
#ifdef QUIET_BOOT
void fb_diag(void);
void splash_abort(void) {   /* a FAIL or oom line was printed under the splash: show the desktop now, boot carries on behind it */
    splash_on = 0;
    fb_desktop();
}
void splash_finish(void) {   /* everything boot waits for is done: the one desktop paint (already made if a failure ended the splash) */
    uart_puts("boot done\n");
    if (splash_on) {
        splash_step(6, "desktop");
        splash_on = 0;
        if (accounts_count()) { desktop_deferred = 1; text_ok = text_init(); text_early = 1; }   /* the login card goes over the splash's colour; the desktop is first drawn after the sign-in */
        else fb_desktop();
    }
    if (!desktop_deferred) fb_diag();             /* it measures the console font, which the desktop's layout does */
}
#endif
/* One line that turns a photo of the screen into a measurement: where the firmware really put the kernel, the monitor
   size it reported, the buffer we got and its pitch in bytes, and the console font's self-test ('M': width x height,
   advance, verdict). For example "@0x80000 fb 1920x1080>1920x1080 p7680 M13x18 a14 ok". */
static void uart_hexs(unsigned long v) {   /* hex without the leading zeros */
    int i = 60; while (i > 0 && !((v >> i) & 15)) i -= 4;
    uart_puts("0x"); for (; i >= 0; i -= 4) uart_putc("0123456789abcdef"[(v >> i) & 15]);
}
void fb_diag(void) {
    unsigned long at; __asm__ volatile ("adrp %0, _start\n add %0, %0, :lo12:_start" : "=r"(at));
    uart_puts("@"); uart_hexs(at);
    uart_puts(" fb "); uart_dec(disp_w); uart_putc('x'); uart_dec(disp_h);   /* monitor > buffer: 53 characters at most, */
    uart_putc('>'); uart_dec(fb_w); uart_putc('x'); uart_dec(fb_h);          /* so it fits one console row in either font */
    uart_puts(" p"); uart_dec(fb_pitch * 4);
    uart_puts(" M"); uart_dec((unsigned)mono_w); uart_putc('x'); uart_dec((unsigned)mono_h);
    uart_puts(" a"); uart_dec((unsigned)mono_adv);
    uart_puts(mono_ok ? " ok\n" : " FAIL\n");
}

void m1_selftest(void) {
    __asm__ volatile ("msr vbar_el1, %0\n isb" :: "r"(vectors));
    uart_puts("M1 vectors set\n");
    m1b_selftest();   /* with the exception table in place a bad map prints instead of hanging */
    __asm__ volatile ("svc #0");                   /* proves the sync path and the return */
    unsigned long freq; __asm__ volatile ("mrs %0, cntfrq_el0" : "=r"(freq));
    if (!freq) freq = 54000000;                    /* a Pi 4 whose firmware left CNTFRQ unset runs its timer at 54 MHz */
    timer_step = freq / 20;                        /* 50 ms */
    GICD(0x000) = 1;                               /* distributor on */
    GICD(0x100) = 1u << TIMER_INTID;               /* enable the timer interrupt */
    *(volatile unsigned char *)(GICD_BASE + 0x400 + TIMER_INTID) = 0x80;   /* priority: a byte register, a 32-bit write there is an alignment fault */
    GICC(0x004) = 0xFF;                            /* let every priority through */
    GICC(0x000) = 1;                               /* CPU interface on */
    __asm__ volatile ("msr cntp_tval_el0, %0" :: "r"(timer_step));
    __asm__ volatile ("msr cntp_ctl_el0, %0" :: "r"(1UL));
    __asm__ volatile ("msr daifclr, #2");          /* unmask interrupts */
#ifdef FP_TEST   /* fptest.c: hold known FP registers across the timer interrupts, whose handler wipes them */
    unsigned long bad = fp_spin(&irq_ticks, 3);
    uart_puts(bad ? "FPTEST FAIL, registers changed: " : "FPTEST ok\n");
    if (bad) { uart_hex(bad); uart_putc('\n'); }
#else
    while (irq_ticks < 3) __asm__ volatile ("wfi");
#endif
    uart_puts("M1a ok\n");
    fb_init();
#ifdef CRASH_TEST   /* arm64-crash-check.py: an unexpected EL1 fault right after the screen is up */
    uart_puts("CRASHTEST: storing to unmapped memory\n");
    *(volatile unsigned long *)0x300000000UL = 1;
#endif
}

/* ask.c's agent loop: [[open APP]] opens a dock app by name. Calculator, Console, Clock, Brick, Browser, Settings, Weather and Stocks are available; other names return 0. */
int app_open_name(const char *name) {
    if ((name[0] | 32) == 'r' && (name[1] | 32) == 'e' && (name[2] | 32) == 'm' && (name[3] | 32) == 'i' && (name[4] | 32) == 'n' && (name[5] | 32) == 'd' && (name[6] | 32) == 'e' && (name[7] | 32) == 'r' && (name[8] | 32) == 's' && !name[9]) return app_dock("Reminders");   /* 3.17.0: [[open Reminders]] */
    if ((name[0] | 32) == 'm' && (name[1] | 32) == 'a' && (name[2] | 32) == 'i' && (name[3] | 32) == 'l' && !name[4]) { dock_activate(2); return 1; }   /* 3.15.0: the Mail tile */
    static const char *const openable[] = {"calculator", "console", "clock", "brick", "browser", "settings", "weather", "stocks"};
    for (unsigned a = 0; a < 8; a++) {
        const char *w = openable[a]; unsigned i = 0;
        for (; w[i] && name[i] && (name[i] | 32) == w[i]; i++) ;
        if (!w[i] && !name[i]) { if (a == 0) calc_open(); else if (a == 2) clock_open(); else if (a == 3) brick_open(); else if (a == 4) browser_open(); else if (a == 5) settings_open(0); else if (a == 6) dock_activate(8); else if (a == 7) dock_activate(9); return 1; }
    }
    static const char *const finder[] = {"finder", "burrow", "files"};   /* 3.14.0: Samantha's [[open Finder]] starts user/finder.c, as the Burrow tile and Spotlight do */
    for (unsigned a = 0; a < 3; a++) {
        const char *w = finder[a]; unsigned i = 0;
        for (; w[i] && name[i] && (name[i] | 32) == w[i]; i++) ;
        if (!w[i] && !name[i]) { dock_activate(SPOT_FINDER); return 1; }
    }
    for (int s = 1; s <= SPOT_LAST; s++) {   /* 4.17.0: anything Spotlight lists and the Pi runs, by the name it shows (Notes, Music, Calendar, the games), the way Spotlight's Enter opens it */
        const char *w = spot_name(s); unsigned i = 0;
        for (; w[i] && name[i] && (name[i] | 32) == (w[i] | 32); i++) ;
        if (!w[i] && !name[i] && lp_on_arm(s)) { dock_activate(s); return 1; }
    }
    return 0;
}

