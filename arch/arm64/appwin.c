/* appwin.c: cut out of main.c (the god-file split). An EL0 app's window: its frame and title, its buffer blitted in, and its events queued. Built as part of main.o (the Makefile's MAINSRC) with
   main.c's own flags; what it shares with main.c is declared in kmain.h. */
#include "kmain.h"
/* ---- M3c part 2: an EL0 app's window. The Calculator's frame and title (the app's name), placed like it over the right of
   the Console, but as tall as fits between the menu bar and the dock: calendar.c's month grid needs about 320 rows. The app
   draws into its own buffer, app_cw by app_ch pixels at UP_WIN in its address space, and JT_SYS_WINDOW_POLL with
   JT_POLL_PRESENT copies it into the window's content (8, 32 inside the frame, as on i386), each pixel s by s. The window in
   front takes the keys and clicks, which the desktop queues as JT_EV_KEY and JT_EV_CLICK (content coordinates); the red dot
   is i386's close, a click above the content that the app answers by exiting, and an app that ignores it is ended a quarter
   second later. Closing puts back the wallpaper saved at boot and redraws the Console or
   the Terminal whole. Opening one closes the Calculator, and opening the Calculator ends the app. ---- */
void spot_close(void);
static unsigned mix31(unsigned a, unsigned b) {   /* three parts a, one part b, in each colour lane */
    return ((((a & 0xFF00FFu) * 3 + (b & 0xFF00FFu)) >> 2) & 0xFF00FFu) | ((((a & 0x00FF00u) * 3 + (b & 0x00FF00u)) >> 2) & 0x00FF00u);
}
static void app_blit(void) {
    int s = (int)window_scale(), x0 = (app_lx + 8) * s, y0 = (app_ly + 32) * s;
    const unsigned *src = (const unsigned *)(UB + UP_WIN);
    unsigned bw = app_bw, cw_ = app_bw < app_cw ? app_bw : app_cw, ch_ = app_bh < app_ch ? app_bh : app_ch;   /* while a resize is in flight the buffer is the old size: its pixels go where they fit */
    if (app_hi) {   /* 3.9.0: the app drew at device size (hires.h and JT_SYS_TEXT2): one buffer pixel per screen pixel, nothing to smooth */
        unsigned hw = bw * 2;
        for (unsigned y = 0; y < ch_ * 2; y++) {
            const unsigned *r = src + y * hw; unsigned *d = fb + (unsigned)(y0 + (int)y) * fb_pitch + (unsigned)x0;
            for (unsigned x = 0; x < cw_ * 2; x++) d[x] = fb_color(r[x] & 0x00FFFFFFu);
        }
        return;
    }
    if (s == 2) {   /* an app that did not ask: the apps draw on a 1x grid: spread each pixel over its 2x2 by bilinear weights (3/4 itself, 1/4 the near neighbour), so a stem is a soft ramp, not a 2x2 block */
        int cw = (int)cw_, ch = (int)ch_;
        for (int y = 0; y < ch; y++) for (int j = 0; j < 2; j++) {
            const unsigned *r0 = src + (unsigned)y * bw, *r1 = src + (unsigned)(j ? (y + 1 < ch ? y + 1 : y) : (y ? y - 1 : y)) * bw;
            unsigned *d = fb + (unsigned)(y0 + y * 2 + j) * fb_pitch + (unsigned)x0;
            for (int x = 0; x < cw; x++) for (int i = 0; i < 2; i++) {
                int xn = i ? (x + 1 < cw ? x + 1 : x) : (x ? x - 1 : x);
                unsigned a = mix31(r0[x] & 0x00FFFFFFu, r0[xn] & 0x00FFFFFFu), b = mix31(r1[x] & 0x00FFFFFFu, r1[xn] & 0x00FFFFFFu);
                d[x * 2 + i] = fb_color(mix31(a, b));
            }
        }
        return;
    }
    for (unsigned y = 0; y < ch_; y++) for (unsigned x = 0; x < cw_; x++) {
        unsigned c = fb_color(src[y * bw + x] & 0x00FFFFFFu), *d = fb + (unsigned)(y0 + (int)y * s) * fb_pitch + (unsigned)(x0 + (int)x * s);
        for (int j = 0; j < s; j++) for (int i = 0; i < s; i++) d[(unsigned)j * fb_pitch + (unsigned)i] = c;
    }
}
void app_paint(void) {   /* the frame, then the last frame the app presented */
    char t[16]; unsigned n = 0;
    for (const char *p = app_cur ? app_cur->name : "app"; *p && n < sizeof t - 1; p++, n++) t[n] = n ? *p : (char)(*p & ~32);   /* "calendar" is titled Calendar */
    t[n] = 0;
    if (n == 8 && t[0] == 'H' && t[7] == 'i') t[6] = 'p';   /* hamurabi is shown as Hamurapi (the store name Hamurabi was taken) */
    int s = (int)window_scale();
    fb_hold();   /* the frame, the content and the arrow reach the screen as one copy */
    cur_hide();
    gui_draw_window_frame(app_lx, app_ly, app_lw, app_lh, t);
    if (app_shown) app_blit();
    fb_flush(app_lx * s, app_ly * s, app_lw * s, app_lh * s);
    cur_show();
    fb_release();
}
static void app_repaint_all(void) {   /* every app window, back to front: what a moved, raised or closed window uncovered comes back whole */
    struct app *keep = ap;
    for (int z = (int)app_zn - 1; z >= 0; z--) { ap = &apps[app_z[z]]; app_paint(); }
    ap = keep;
}
void app_raise(void) {   /* this app's window in front of the Console, the Terminal and the other apps, with the keys */
    if (!app_wbytes) return;
    int was = app_front && app_top == (int)ap->slot;
    app_front = 1;
    app_z_front(ap->slot);
    if (was) app_paint(); else app_repaint_all();
    if (!was) { con_quiet = 1; uart_puts("focus app\n"); }
}
void app_push(unsigned kind, int a, int b) {   /* one event for the app; a full ring drops the newest, as on i386 */
    if (!app_cur || app_qt - app_qh >= APP_EVQ) return;
    app_q[app_qt % APP_EVQ].kind = kind; app_q[app_qt % APP_EVQ].a = a; app_q[app_qt % APP_EVQ].b = b;
    app_qt++; app_kick = 1;   /* it runs on this pass, not 10 ms from now */
}
static void st32(unsigned long a, unsigned v) { for (int i = 0; i < 4; i++) ((unsigned char *)a)[i] = (unsigned char)(v >> (8 * i)); }   /* any alignment: the pointer is the program's */
/* JT_SYS_WINDOW_OPEN: only for an app the scheduler runs (the boot self-test gets -ENODEV, as i386 answers a program run
   from the text shell). The first call maps the buffer, zeroed, EL0 read-write and never executable, and puts the window up
   in front; any call fills the LP64 struct jt_window_info: width, height, pitch, then the 64-bit pixels pointer at +16. */
long app_window_open(unsigned long info, unsigned long flags) {
    if (!app_running || !fb || !app_cw) return -19;
    if (!user_writable(info, 24)) return -14;
    if (!app_wbytes || app_cw != app_bw || app_ch != app_bh) {   /* the first open, or the answer to JT_EV_RESIZE: a buffer of the content's size now, zeroed */
        const unsigned long PAGE = 0x3UL | (1UL << 2) | (3UL << 8) | (1UL << 10), AP_EL0_RW = 1UL << 6, PXN = 1UL << 53, UXN = 1UL << 54;
        int hi = ((flags & 1) || (app_wbytes && app_hi)) && window_scale() == 2;   /* hires.h's ask: only at scale 2, where there are device pixels to give */
        unsigned long bytes = ((unsigned long)app_cw * app_ch * (hi ? 16 : 4) + 4095) & ~4095UL;
        if (bytes > UWIN_MAX) return -12;
        for (unsigned long i = 0; i < bytes / 4096; i++) l3[ap->l3off + (UP_WIN >> 12) + i] = (UB + UP_WIN + i * 4096) | PAGE | AP_EL0_RW | PXN | UXN;
        __asm__ volatile ("dsb ishst\n tlbi vmalle1\n dsb ish\n isb" ::: "memory");
        for (unsigned long i = 0; i < bytes / 8; i++) ((unsigned long *)(UB + UP_WIN))[i] = 0;
        int first = !app_wbytes;
        app_wbytes = bytes; app_shown = 0; app_bw = app_cw; app_bh = app_ch; app_hi = hi;
        if (first) { spot_close(); calc_close(); browser_close(); settings_close(); app_front = 1; app_z_front(ap->slot); app_rs_log = 0; }
        else app_rs_log = 1;
        if (app_front && first) app_paint();   /* 4.3.0: a resize's answer keeps the frame win_set drew (old pixels where they fit) until the redrawn frame is presented, so no empty frame flashes */
        con_quiet = !first; uart_puts("M3c window "); uart_puts(app_cur->name); uart_putc(' '); uart_dec(app_cw); uart_putc('x'); uart_dec(app_ch);
        if (first) { uart_puts(" at "); uart_dec((unsigned)app_lx + 8); uart_putc(','); uart_dec((unsigned)app_ly + 32); }
        else uart_puts(" resized");
        uart_putc('\n');
    }
    st32(info, app_bw); st32(info + 4, app_bh); st32(info + 8, app_bw * (app_hi ? 8 : 4));   /* width and height stay logical; the pitch (bytes) says the buffer is twice as wide */
    st32(info + 12, 0); st32(info + 16, (unsigned)(UB + UP_WIN)); st32(info + 20, (unsigned)((UB + UP_WIN) >> 32));
    return 0;
}
/* JT_SYS_TEXT2 (3.9.0): one run of Tree Sans into the device-size window buffer, for an app that opened it hi-res. The
   request is user/jtsys.h's struct jt_text2 (LP64). Each byte is drawn alone at the pen, which then moves by that byte's
   own advance from the app, so the app's 1x layout carries over exactly while the glyphs are rasterised at twice the size
   by the same rasterizer the Console uses (text.c). Faces: 0 body, 1 bold, 2 display (digits), 3 mono; the sizes are the
   ones tools/gen/gen_user_text.c gave the 1x atlas, times the scale, times pct/100. Answers the pen's x, or -EINVAL when
   the window is not hi-res and -EFAULT for a request or string outside the app's memory. */
struct jt_text2 { int x, y; unsigned rgb; int face, pct, len; unsigned long s, adv; };
long app_text(unsigned long req) {
    if (!app_running || !app_wbytes || !app_hi || !text_ok) return -22;
    if (!user_in_range(req, sizeof(struct jt_text2))) return -14;
    const struct jt_text2 *t = (const struct jt_text2 *)req;
    int x = t->x, y = t->y, face = t->face, pct = t->pct, n = t->len;
    unsigned rgb = t->rgb; unsigned long sp = t->s, adv = t->adv;
    if (face < 0 || face > 3 || pct < 25 || pct > 400 || n < 1 || n > 255) return -22;
    if (!user_in_range(sp, (unsigned long)n) || !user_in_range(adv, (unsigned long)n)) return -14;
    static const unsigned char which[4] = {2, 1, 2, 0};   /* text.c's faces: 0 mono, 1 sans bold, 2 sans */
    static const int px10[4] = {140, 140, 560, 135};
    int k = (int)window_scale(), bw = (int)app_cw * k, bh = (int)app_ch * k, px = px10[face] * k * pct / 100;
    unsigned *buf = (unsigned *)(UB + UP_WIN);
    for (int i = 0; i < n; i++) {
        char one[2] = { ((const char *)sp)[i], 0 };
        text_draw(which[face], one, x, y, px, rgb, buf, (unsigned)bw, bw, bh);
        x += ((const unsigned char *)adv)[i];
    }
    return x;
}
/* JT_SYS_WINDOW_POLL, i386's semantics: with JT_POLL_PRESENT the buffer goes to the screen first (only while the window is
   in front and Spotlight is not over it; a raise draws it later), then one event (the LP64 struct jt_event: kind, a, b)
   and 1, or -EAGAIN when there is none. It never waits: the app yields and asks again. */
long app_window_poll(unsigned long ev, unsigned long flags) {
    if (!app_running || !app_wbytes) return -9;
    if (!user_writable(ev, 12)) return -14;
    if (flags & ~1UL) return -22;
    if (flags & 1) {
        ap->presents++;
        if (!app_shown) {
            con_quiet = 1; uart_puts("M3c window "); uart_puts(app_cur->name);
            if (app_rs_log) { uart_puts(" redrawn "); uart_dec(app_bw); uart_putc('x'); uart_dec(app_bh); uart_putc('\n'); app_rs_log = 0; }   /* the first frame in the buffer a resize gave it */
            else uart_puts(" first frame\n");
        }
        app_shown = 1;
        if (app_front && app_top == (int)ap->slot && !spot_live) {
            int s = (int)window_scale();
            fb_hold(); cur_hide(); app_blit(); fb_flush((app_lx + 8) * s, (app_ly + 32) * s, (int)app_cw * s, (int)app_ch * s); cur_show(); fb_release();
        }
    }
    if (app_qh == app_qt) return -11;
    unsigned i = app_qh++ % APP_EVQ;
    st32(ev, app_q[i].kind); st32(ev + 4, (unsigned)app_q[i].a); st32(ev + 8, (unsigned)app_q[i].b);
    con_quiet = 1; uart_puts("M3c event "); uart_puts(app_q[i].kind == 1 ? "key " : app_q[i].kind == 4 ? "resize " : "click "); uart_dec((unsigned)app_q[i].a);
    if (app_q[i].kind != 1) { int y = app_q[i].b; uart_putc(','); if (y < 0) { uart_putc('-'); y = -y; } uart_dec((unsigned)y); }   /* a close click is above the content */
    uart_puts(" to "); uart_puts(app_cur->name); uart_putc('\n');
    return 1;
}
void app_window_close(void) {
    if (!app_wbytes) return;
    cur_hide();
    wall_vacate(app_lx, app_ly, app_lw, app_lh, 0);   /* the frame and a ring round it: anything that overhung the frame goes too */
    app_shown = 0;
    unsigned gone = ap->slot;
    app_z_drop(gone);
    app_top = app_zn ? (int)app_z[0] : -1;
    ap = &apps[app_top >= 0 ? app_top : gone];
    app_front = app_top >= 0;
    if (app_front) app_repaint_all();   /* the other apps' windows the closed one covered or was covered by */
    else if (con_live) { wall_vacate(win_lx, win_ly, win_lw, win_lh, 0); con_live = 0; pane_open(cp); }   /* the wallpaper went over its part of the Console too: draw it whole */
    cur_show();
}
/* A dock tile whose name is an app in the table (Calendar, and any later one): start it, or bring its window up. */
int app_cycle(void) {   /* Cmd+Tab or Ctrl+Tab: the app window furthest back comes to the front, so each tap walks every open one */
    if (!app_zn) return 1;
    spot_close(); settings_close(); brick_close(); clock_close(); browser_close();
    ap = &apps[app_z[app_front ? app_zn - 1 : 0]];   /* the Terminal or the Console in front: the last app used comes back first */
    app_raise();   /* ponytail: no hold-and-pick panel as on i386; with three or more windows a tap goes to the oldest, not the last used */
    con_quiet = 1; uart_puts("switch "); uart_puts(ap->cur->name); uart_putc('\n');
    return 1;
}
int app_dock(const char *name) {
    for (const struct user_app *a = user_apps; a->name; a++) {
        unsigned i = 0;
        while (name[i] && (name[i] | 32) == a->name[i]) i++;
        if (name[i] || a->name[i]) continue;
        for (unsigned k = 0; k < APP_SLOTS; k++) if (apps[k].cur == a) { ap = &apps[k]; app_raise(); return 1; }   /* already open: its window comes to the front */
        if (app_start(a) == -3 && app_zn) {   /* every slot is taken: the window furthest back closes for it, files written back */
            ap = &apps[app_z[app_zn - 1]];
            con_quiet = 1; uart_puts("M3c app swap: "); uart_puts(app_cur->name); uart_puts(" closed for "); uart_puts(a->name); uart_putc('\n');
            app_kill_one("closed for another app");
            app_start(a);
        }
        return 1;
    }
    return 0;
}
