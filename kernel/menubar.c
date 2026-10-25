/* The i386 menu bar, out of kernel.c since 4.23.8: the mark, the bold app name, the clock (CMOS weekday, date
   and 12-hour time), the menu-bar weather and its hit box, redrawn once a real minute unless forced. i386 only;
   the Pi draws its own (arch/arm64/desk.c). */
#include "libc.h"
#include "window.h"
#include "font.h"
#include "serial.h"
#include "gui_prims.h"
#include "gui_paint.h"
#include "desktop.h"
#include "wallpaint.h"
#include "uitext.h"
#include "weather.h"
#include "menubar.h"

/* Real, user-reported flicker: this whole bar (a solid white rect, the
   logo, "Joshua Tree", the clock) got redrawn identically on every single
   hover-state change, since gui_draw_desktop calls this unconditionally
   on every mouse move. Nothing here actually depends on hover at all, and
   without a back buffer to swap in atomically, redrawing pixels that
   didn't need to change is pure flicker, not just pure waste. Skips the
   redraw entirely once per real minute unless forced, matching the one
   thing in this bar that actually changes on its own. */
int gui_menubar_last_min = -1;
void gui_menubar_force_redraw(void){ gui_menubar_last_min = -1; } int gui_bleed_open(void); /* a full-bleed window covers the menu bar */

/* Real macOS menu bar clock format: weekday, month, day, 12-hour time with
   AM/PM, not the bare 24h HH:MM this used to show. Reads the CMOS weekday
   (reg 6) and date (reg 7)/month (reg 8) registers the same BCD way
   show_time() already reads hour/minute, no new decoding scheme. RTC
   weekday numbering is 1=Sunday on every real PC and QEMU's own RTC
   emulation, confirmed against this exact machine's real wall clock the
   same way the earlier UTC-vs-local timezone fix was (a real screendump
   matching what `date` printed at that same moment). */
/* v43 (0.43.0): live weather in the menu bar. Open-Meteo answers over
   plain HTTP (checked: a real 200 on http://, no redirect), which is what
   makes this possible at all in a kernel with no TLS. Vancouver by default,
   the one place this machine actually sits. Honest limits, stated rather
   than hidden: the fetch is synchronous inside the GUI loop, once after
   the first frame and then every ten minutes, so on a NIC with no route
   out it can stall the desktop for the WAN timeout; moving it into a
   background task is the follow-up, held back only because net.c's
   receive path was written single-caller and hasn't been audited for a
   second one yet. No NIC (v86, the browser demo) means no attempt and
   nothing drawn, not an error. */

/* Hit box for the menu-bar weather text, recomputed by gui_draw_menubar
   every time it actually redraws that text (same cadence the clock hit
   test already tolerates: coarse, minute-granularity, matching how often
   the underlying layout can shift). -1/-1 means "nothing drawn, not
   clickable" rather than a stale box from a previous boot. */
int weather_hit_x0 = -1, weather_hit_x1 = -1;
void gui_draw_mark_sized(int cx, int cy, int size, unsigned int ink);   /* kernel/bootscreen.c */
void gui_draw_menubar(void){
    if (gui_bleed_open()) { gui_menubar_last_min = -1; return; } /* repaints on the first call after it closes */
    unsigned char h, m, wd, dom, mon;
    cmos_read_time_stable(&h, &m, &wd, &dom, &mon);
    unsigned char hv = (h & 0x0F) + ((h >> 4) * 10), mv = (m & 0x0F) + ((m >> 4) * 10);
    unsigned char wdv = (wd & 0x0F) + ((wd >> 4) * 10);
    unsigned char domv = (dom & 0x0F) + ((dom >> 4) * 10);
    unsigned char monv = (mon & 0x0F) + ((mon >> 4) * 10);
    if (mv == gui_menubar_last_min) return;
    gui_menubar_last_min = mv;
    serial_puts("menubarredraw\n"); /* discriminating marker for tools/checks/menuclock-check.sh: a real minute-change redraw */

    /* v48: Liquid Glass, direct request. No real alpha compositing in this
       framebuffer (see gui_blend's own note), so "translucent" here means
       the same trick the dock shadow already uses: a real, solid,
       precomputed blend of white toward whatever wallpaper color sits
       behind this row, sampled per-row (gui_wallpaper_color already does
       exactly this, reused, not a second sampler).
       v0.76.17: direct request ("menu bar transparency like 50%"), moved
       from 7:10 (70% white / 30% wallpaper) to a genuine 5:10 (50/50)
       blend -- still just a precomputed solid color per row, no real
       blur/alpha, but honestly a 50% mix now instead of mostly-white. */
    for (int row = 0; row < GUI_MENUBAR_H; row++)
        window_rect(0, row, (int)window_width(), 1, gui_lerp(gui_wallpaper_color(row), 0x00FFFFFF, 5, 10));
    gui_hairline_h(0, GUI_MENUBAR_H - 1, (int)window_width(), 0x00BDB8B0); /* 2.0: one physical pixel, not a doubled logical row */
    gui_draw_mark_sized(24, GUI_MENUBAR_H / 2, 20, 0x001C1C1E); /* 2.0: the real brand mark (boot_mark) sits in the apple-menu spot */
    font_draw_string(portfolio_dock ? "Joshua Trommel" : "Joshua Tree", 40, 7, 0x001C1C1E, -1); /* portfolio mode is his site, so the corner carries his name */

    static const char *WD[7] = {"Sun","Mon","Tue","Wed","Thu","Fri","Sat"};
    static const char *MO[12] = {"Jan","Feb","Mar","Apr","May","Jun","Jul","Aug","Sep","Oct","Nov","Dec"};
    int wdi = (wdv >= 1 && wdv <= 7) ? wdv - 1 : 0;
    int moi = (monv >= 1 && monv <= 12) ? monv - 1 : 0;
    int h12 = hv % 12; if (h12 == 0) h12 = 12;
    const char *ampm = hv < 12 ? "AM" : "PM";

    char clock[24];
    int p = 0;
    for (const char *s = WD[wdi]; *s; s++) clock[p++] = *s;
    clock[p++] = ' ';
    for (const char *s = MO[moi]; *s; s++) clock[p++] = *s;
    clock[p++] = ' ';
    if (domv >= 10) clock[p++] = '0' + domv / 10;
    clock[p++] = '0' + domv % 10;
    clock[p++] = ' '; clock[p++] = ' ';
    if (h12 >= 10) clock[p++] = '0' + h12 / 10;
    clock[p++] = '0' + h12 % 10;
    clock[p++] = ':';
    clock[p++] = '0' + mv / 10; clock[p++] = '0' + mv % 10;
    clock[p++] = ' '; clock[p++] = ampm[0]; clock[p++] = ampm[1];
    clock[p] = 0;

    int cw = font_string_width(clock); /* v77: real proportional width, not p*8 */
    font_draw_string(clock, (int)window_width() - cw - 16, 7, 0x001C1C1E, -1);
    if (weather_text[0]) {
        int wl = font_string_width(weather_text);
        int wx = (int)window_width() - cw - 16 - wl - 28;
        font_draw_string(weather_text, wx, 7, 0x001C1C1E, -1); /* match the clock's own ink and weight, not a warm brown -- must read on every wallpaper theme, not just the default */
        weather_hit_x0 = wx - 4; weather_hit_x1 = wx + wl + 4; /* v53: real click target, same padding feel as the clock's own */
    } else {
        weather_hit_x0 = weather_hit_x1 = -1;
    }
}
