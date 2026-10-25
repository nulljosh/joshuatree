#ifndef DESKTOP_H
#define DESKTOP_H
/* What kernel.c's desktop shares with the units split out of it (kernel/wxtest.c, kernel/guitest.c): the menu bar and weather panel
   geometry, and the day/night grade. kernel.c defines all of these. */
#define GUI_MENUBAR_H   26
#define WEATHER_W 220
#define GUI_FLYOUT_RADIUS 14   /* the menu-bar flyouts' corner radius, the dock's own (see kernel.c) */
#define GUI_APP_COUNT   34 /* 32 real apps + the Apps folder + Trash; Books (31) pushed Apps/Trash to 32/33; 2.2 Music (24) and Movies (25) pushed Apps/Trash to 26/27, 2.7 Hamurapi (26) to 27/28, 2.8 Windgate (27) to 28/29, 2.11 Panes (28) to 29/30, 2.14 Claude (29) to 30/31, Mines (30) to 31/32 */
#define GUI_APP_PANES   28 /* the one app that gets Ctrl chords as KEY_CTL_* (kernel/app.h) */
#define GUI_APP_PORTFOLIO 21 /* hidden from the Apps folder and phone home unless the boot line says "portfolio" (his site embed); the public OS ships without it */
#include "app.h"
extern const struct app APPS[GUI_APP_COUNT];   /* the app registry */
extern int gui_app_windowed;                    /* an app is drawing inside a window right now */
int jt_phone_mode(void);                        /* booted as the phone */
void gui_draw_menubar(void);
void gui_menubar_force_redraw(void);
void samantha_ring3_open(void);
void notes_ring3_open(void);
void notes_migrate_legacy(void);                /* editor.h: the one-time NOTES.TXT move */
char kbd_map(int sc);
extern int weather_hit_x0, weather_hit_x1;   /* the menu-bar weather's hit box, set when the bar is drawn */
void daynight_calc(int hour, int *night_out, int *day_out);   /* pure: the night and day blend for an hour */
unsigned int gui_daynight_tint_pct(unsigned int rgb, int night, int day);
unsigned int gui_daynight_tint(unsigned int rgb);            /* at the current hour */
void gui_draw_weather_panel(void);
void gui_draw_wallpaper_rows(int y_from, int y_to);
void gui_draw_wallpaper_rect(int x, int y, int w, int h);   /* repaint a rectangle of wallpaper */
int gui_apps_n(void);                    /* apps the folder shows (Portfolio only in portfolio mode) */
int gui_app_at(int k);                   /* folder position to APPS[] slot */
int gui_aa_advance(unsigned char c);                   /* the UI face's advance for c, physical px */
void gui_aa_char(unsigned char c, int px, int py, unsigned int fg, int bg, int cell);   /* one UI-face glyph */
#endif
