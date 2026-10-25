/* The Apple menu's rows, hit test and drawing, and the menu-bar weather dropdown, out of kernel.c since 4.20.10. The
   menu's actions and the lock screen stay in kernel.c with the Settings and sign-in code they call. */
#include "libc.h"
#include "serial.h"
#include "window.h"
#include "font.h"
#include "gui_prims.h"
#include "gui_paint.h"
#include "gui_shapes.h"
#include "desktop.h"
#include "weather.h"
#include "bootscreen.h"
#include "menu.h"

/* A real Apple-menu-style dropdown off the tree logo, macOS-shaped (dark
   panel, one highlighted row under the cursor) but with items that
   actually do something real on this kernel, not a decorative copy of
   a macOS menu that happens to not work: About shows real memory/uptime
   stats, Files and Notes launch the same real apps the dock does,
   Restart calls this kernel's own real reboot() (the 8042 reset pulse,
   already used by the "reboot" shell command), Shut Down really halts
   the CPU. "-" is a separator row, not a real item. */
#define GUI_MENU_ITEM_COUNT 8
static const char *GUI_MENU_LABELS[GUI_MENU_ITEM_COUNT] = {
    "About Joshua Tree", "Burrow", "Notes", "Settings", "Lock Screen", "-", "Restart", "Shut Down"
};
#define GUI_MENU_ROW_H  22
#define GUI_MENU_SEP_H  9
#define GUI_MENU_X0     4
#define GUI_MENU_W      180

/* v66, real restraint pass, not a new look: every other surface on this
   desktop (the dock tray, the weather app's own card, the Apps folder
   glass) is a soft rounded rect blended straight into whatever's behind
   it, gui_rounded_rect_on_wallpaper, zero separate stroke line. These
   three flyouts (this Apple menu, the notif panel, the weather dropdown)
   were the one place still built as a flat, hard-cornered rectangle with
   a manually drawn 1px border on all four sides, confirmed with a real
   headless framebuffer dump: square corners sitting directly against the
   dock's rounded ones read as two different chrome languages on one
   desktop, not two different needs. One shared radius, reused by all
   three, the same helper the dock already uses, is the fix: not a new
   rule, the existing one applied to the surfaces that had been skipping
   it. The three panels already shared bg/border/text colors with each
   other; this just brings their shape in line with everything else too. */
/* GUI_FLYOUT_RADIUS (14) lives in desktop.h since 4.20.6, shared with kernel/klog.c's panel. */

static int gui_menu_row_h(int i){ return GUI_MENU_LABELS[i][0] == '-' ? GUI_MENU_SEP_H : GUI_MENU_ROW_H; }
static int gui_menu_total_h(void){ int h = 0; for (int i = 0; i < GUI_MENU_ITEM_COUNT; i++) h += gui_menu_row_h(i); return h; }
/* v66: real padding, not a stray number. The hover highlight on the first
   and last row is a plain rect inset only 2px from the panel's own edges;
   drawn flush against the panel's new rounded top/bottom (GUI_FLYOUT_RADIUS
   above), its hard corner visibly poked out past the panel's own curve,
   confirmed with a real zoomed capture, a worse seam than the square panel
   this pass just removed. The fix real menus use is exactly this: padding
   above the first row and below the last so no highlightable row ever
   reaches the curved part of the panel. 8px is the minimum that clears it:
   solving the same circle gui_rounded_rect_on_wallpaper draws with for the
   row where a 2px-inset flat edge first stays inside the curve gives 7,
   rounded up. */
#define GUI_MENU_PAD_V 8

/* Returns the item index under (mx,my), -2 for a separator row (a real
   hit, but not an actionable one), or -1 if outside the menu entirely. */
int gui_menu_hit_test(int mx, int my){
    int y = GUI_MENUBAR_H + GUI_MENU_PAD_V, total_h = gui_menu_total_h();
    if (mx < GUI_MENU_X0 || mx >= GUI_MENU_X0 + GUI_MENU_W || my < y || my >= y + total_h) return -1;
    for (int i = 0; i < GUI_MENU_ITEM_COUNT; i++){
        int rh = gui_menu_row_h(i);
        if (my < y + rh) return GUI_MENU_LABELS[i][0] == '-' ? -2 : i;
        y += rh;
    }
    return -1;
}

void gui_draw_apple_menu(int hover_item){
    int y0 = GUI_MENUBAR_H, total_h = gui_menu_total_h() + 2 * GUI_MENU_PAD_V;
    unsigned int bg = 0x002C2C2E, text = 0x00F5F5F7;
    gui_rounded_rect_on_wallpaper(GUI_MENU_X0, y0, GUI_MENU_W, total_h, bg, GUI_FLYOUT_RADIUS);
    int ry = y0 + GUI_MENU_PAD_V;
    for (int i = 0; i < GUI_MENU_ITEM_COUNT; i++){
        int rh = gui_menu_row_h(i);
        if (GUI_MENU_LABELS[i][0] == '-') { window_rect(GUI_MENU_X0 + 8, ry + rh / 2, GUI_MENU_W - 16, 1, 0x00545458); ry += rh; continue; }
        if (i == hover_item) window_rect(GUI_MENU_X0 + 2, ry, GUI_MENU_W - 4, rh, 0x00555555);
        font_draw_string(GUI_MENU_LABELS[i], GUI_MENU_X0 + 12, ry + 5, text, -1);
        ry += rh;
    }
}


/* v53: the weather text opens a dropdown too, same open-on-press/
   dismiss-on-next-release contract as the clock's notif panel above, same
   panel chrome (colors, border-drawing, font). Shows only real fields the
   existing weather_fetch() already parses (temp, WMO code -> condition
   word, and as of v71 the real ip-api.com city/lat/lon it queried, no
   longer a fixed literal), no invented humidity/wind/forecast rows the
   data doesn't have. */
/* WEATHER_W: kernel/desktop.h */
void gui_draw_weather_panel(void){
    int x0 = weather_hit_x0 >= 0 ? weather_hit_x0 : (int)window_width() - WEATHER_W - 200;
    if (x0 + WEATHER_W > (int)window_width() - 4) x0 = (int)window_width() - WEATHER_W - 4;
    int y0 = GUI_MENUBAR_H;
    unsigned int bg = 0x002C2C2E, text = 0x00F5F5F7, dim = 0x00A0A0A6;

    int total_h = 10 + (geo_city[0] ? 5 : 4) * 20 + 6; /* v71: one extra row for the looked-up city name when ip-api gave one */
    gui_rounded_rect_on_wallpaper(x0, y0, WEATHER_W, total_h, bg, GUI_FLYOUT_RADIUS);

    int y = y0 + 8;
    if (!weather_have) {
        font_draw_string("No weather yet", x0 + 12, y, dim, -1);
        return;
    }
    font_draw_string(weather_text[0] ? weather_text : "Weather", x0 + 12, y, text, -1); y += 20;

    char line[40]; int p;
    p = 0; line[p++]='T'; line[p++]='e'; line[p++]='m'; line[p++]='p'; line[p++]=':'; line[p++]=' ';
    { int t = weather_temp_c; if (t < 0) { line[p++]='-'; t=-t; } if (t>=10) line[p++]='0'+t/10; line[p++]='0'+t%10; line[p++]=(char)0xF8; line[p++]='C'; }
    line[p]=0; font_draw_string(line, x0 + 12, y, dim, -1); y += 20;

    p = 0; line[p++]='C'; line[p++]='o'; line[p++]='d'; line[p++]='e'; line[p++]=':'; line[p++]=' ';
    { int c = weather_code10 / 10; if (c >= 100) line[p++]='0'+c/100; if (c>=10) line[p++]='0'+(c/10)%10; line[p++]='0'+c%10; }
    line[p]=0; font_draw_string(line, x0 + 12, y, dim, -1); y += 20;

    /* v71: the real, looked-up location (ip-api.com, see geo_fetch), not
       the old fixed literal. City first when ip-api gave one, then the
       exact lat/lon text the Open-Meteo URL was actually built from. */
    if (geo_city[0]) { font_draw_string(geo_city, x0 + 12, y, dim, -1); y += 20; }
    p = 0;
    for (const char *s = geo_lat; *s && p < 16; s++) line[p++] = *s;
    line[p++] = ','; line[p++] = ' ';
    for (const char *s = geo_lon; *s && p < 36; s++) line[p++] = *s;
    line[p]=0; font_draw_string(line, x0 + 12, y, dim, -1);
}
