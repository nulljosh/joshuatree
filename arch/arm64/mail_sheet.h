/* 13.25.0, Mail's sign-in sheet: the Mail window's "Sign in" asks for it (JT_SYS_MAIL, user/mailpi.c), and the kernel draws
   it, so the app password never passes through a ring-3 app. A card over the desktop with three fields: the server
   (imap.mail.me.com to start), the address and the app password, shown as stars. Tab or Return moves to the next field,
   Return on the last signs in through the same mail_account and mail_password `mail login` uses, Esc cancels. While it is
   up it has every key and the mouse does nothing, like the lock card. The password is wiped the moment it has been tried,
   right or wrong, and on every close. Signed in, the sheet closes and Blackcomb opens the inbox (mail:), as the Mail tile
   does from then on. The pixels under the card are kept and put back on close, as Spotlight's are.
   Included by boot.c after login_ui.h (lg_field, lg_char). tools/checks/arm64-mail-sheet-check.py drives it. */
#define MS_W 340
#define MS_H 300
#define MS_FIELDS 3
static const char *const ms_label[MS_FIELDS] = { "Server", "Email", "App password" };
static char ms_text[MS_FIELDS][128];
static unsigned ms_len[MS_FIELDS];
static int ms_live, ms_want, ms_field, ms_x, ms_y;
static const char *ms_msg;
static unsigned *ms_under;
const char *mail_account(const char *host, unsigned hn, const char *user, unsigned un);   /* mail.c */
void mail_password(const char *pass);
int mail_signed_in(void);
void browser_go(const char *url);
extern void (*cmd_out)(const char *s);

static void ms_wipe(void) {
    for (int f = 0; f < MS_FIELDS; f++) { accounts_wipe(ms_text[f], sizeof ms_text[f]); ms_len[f] = 0; }
}
static void ms_paint(void) {
    int s = (int)window_scale(), w = MS_W * s, h = MS_H * s, x0 = ms_x * s, y0 = ms_y * s;
    cur_hide();
    for (int j = 0; j < h; j++) for (int i = 0; i < w; i++) fb[(unsigned)(y0 + j) * fb_pitch + (unsigned)(x0 + i)] = ms_under[j * w + i];
    gui_rounded_rect_on_wallpaper(ms_x, ms_y, MS_W, MS_H, 0x00d9d3cb, 14);          /* a hairline edge (WINDOW_RULE): it sits on windows of its own paper */
    gui_rounded_rect_on_wallpaper(ms_x + 1, ms_y + 1, MS_W - 2, MS_H - 2, 0x00faf8f4, 13);
    const char *t = "Sign in to Mail";
    gui_icon_text(t, ms_x + (MS_W - gui_icon_text_w(t, 2, 1)) / 2, ms_y + 22, 2, 1, LG_INK);
    t = "Use an app password from your mail provider.";
    gui_text(t, ms_x + (MS_W - gui_text_width(t)) / 2, ms_y + 56, LG_HINT);
    int fx = ms_x + 28, fw = MS_W - 56;
    for (int f = 0; f < MS_FIELDS; f++) {
        int ly = ms_y + 84 + f * 60, fy = ly + 18;
        gui_text(ms_label[f], fx, ly, LG_INK);
        lg_field(fx, fy, fw, 30, f == ms_field);
        char shown[130]; unsigned n = 0;
        if (f == 2) { for (; n < ms_len[f] && n < 64; n++) shown[n] = '*'; }
        else for (; n < ms_len[f]; n++) shown[n] = ms_text[f][n];
        shown[n] = 0;
        const char *v = shown;
        while (*v && gui_text_width(v) > fw - 24) v++;              /* a long address shows its end, where the typing is */
        if (*v) gui_text(v, fx + 9, fy + 7, LG_INK);
        if (f == ms_field) { int tw = *v ? gui_text_width(v) : 0; fb_rect((fx + 9 + tw + 1) * s, (fy + 7) * s, s, 16 * s, LG_ACCENT); }
    }
    t = ms_msg ? ms_msg : "Tab moves. Return signs in. Esc cancels.";
    gui_text(t, ms_x + (MS_W - gui_text_width(t)) / 2, ms_y + MS_H - 30, ms_msg ? LG_ACCENT : LG_HINT);
    fb_flush(x0, y0, w, h);
    cur_show();
}
static void ms_close(void) {
    if (!ms_live) return;
    int s = (int)window_scale(), w = MS_W * s, h = MS_H * s, x0 = ms_x * s, y0 = ms_y * s;
    ms_wipe();
    cur_hide();
    for (int j = 0; j < h; j++) for (int i = 0; i < w; i++) fb[(unsigned)(y0 + j) * fb_pitch + (unsigned)(x0 + i)] = ms_under[j * w + i];
    fb_flush(x0, y0, w, h);
    ms_live = 0; ms_msg = 0;
    cur_show();
    if (app_front && app_shown) app_paint();
    con_quiet = 1; uart_puts("mail sheet close\n");
}
static void ms_open(void) {
    int s = (int)window_scale(), w = MS_W * s, h = MS_H * s;
    if (ms_live || !fb || login_live || spot_live) return;
#ifdef PI_BUILD
    if (lock_on) return;
#endif
    if (!ms_under) ms_under = kmalloc((unsigned)(w * h) * 4);
    if (!ms_under) { uart_puts("oom mail sheet\n"); return; }
    ms_x = ((int)fb_w / s - MS_W) / 2; ms_y = ((int)fb_h / s - MS_H) / 2 - 20;
    cur_hide();
    for (int j = 0; j < h; j++) for (int i = 0; i < w; i++) ms_under[j * w + i] = fb[(unsigned)(ms_y * s + j) * fb_pitch + (unsigned)(ms_x * s + i)];
    cur_show();
    ms_wipe(); ms_msg = 0;
    static const char def[] = "imap.mail.me.com";
    for (ms_len[0] = 0; def[ms_len[0]]; ms_len[0]++) ms_text[0][ms_len[0]] = def[ms_len[0]];
    ms_field = 1; ms_live = 1;
    con_quiet = 1; uart_puts("mail sheet open\n");
    ms_paint();
}
static void ms_quiet(const char *t) { con_quiet = 1; uart_puts(t); }   /* mail.c's lines, to the UART only: none holds the password */
static void ms_submit(void) {
    const char *why = mail_account(ms_text[0], ms_len[0], ms_text[1], ms_len[1]);
    if (!why && !ms_len[2]) why = "the app password is empty";
    if (why) { accounts_wipe(ms_text[2], sizeof ms_text[2]); ms_len[2] = 0; ms_msg = why; ms_field = 2; ms_paint(); return; }
    ms_msg = "Signing in..."; ms_paint();
    void (*saved)(const char *) = cmd_out; cmd_out = ms_quiet;
    mail_password(ms_text[2]);
    cmd_out = saved;
    accounts_wipe(ms_text[2], sizeof ms_text[2]); ms_len[2] = 0;   /* tried: gone, right or wrong */
    if (mail_signed_in()) { ms_close(); con_quiet = 1; uart_puts("mail sheet: signed in\n"); browser_go("mail:"); return; }
    ms_msg = "That did not sign in. Check all three."; ms_field = 2;
    con_quiet = 1; uart_puts("mail sheet: refused\n");
    ms_paint();
}
static void ms_event(struct input_event e) {   /* input_event, while the sheet is up: keys only */
    if (e.type == 1 && e.code == 1 && e.value) { ms_close(); return; }
    int ch = lg_char(e);
    if (!ch) return;
    ms_msg = 0;
    if (ch == '\t') ms_field = (ms_field + 1) % MS_FIELDS;
    else if (ch == '\n') { if (ms_field < MS_FIELDS - 1) ms_field++; else { ms_submit(); return; } }
    else if (ch == '\b') { if (ms_len[ms_field]) ms_text[ms_field][--ms_len[ms_field]] = 0; }
    else if (ch > ' ' && ch < 127 && ms_len[ms_field] + 1 < sizeof ms_text[0]) { ms_text[ms_field][ms_len[ms_field]++] = (char)ch; ms_text[ms_field][ms_len[ms_field]] = 0; }
    else return;
    ms_paint();
}
long mail_sheet_ask(unsigned long op) {   /* JT_SYS_MAIL from the Mail window: 0 asks for the sheet, 1 asks if mail is signed in */
    if (op == 1) return mail_signed_in();
    if (op != 0) return -22;
    if (mail_signed_in()) return 0;
    ms_want = 1;                               /* opened by the main loop, not inside the app's system call */
    return 1;
}
static void mail_sheet_tick(void) { if (ms_want) { ms_want = 0; ms_open(); } }
