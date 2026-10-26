#ifndef DESKTOP_H
#define DESKTOP_H
/* What kernel.c's desktop shares with the units split out of it (kernel/wxtest.c, kernel/guitest.c): the menu bar and weather panel
   geometry, and the day/night grade. kernel.c defines all of these. */
#define GUI_MENUBAR_H   26
#define WEATHER_W 220
#define GUI_FLYOUT_RADIUS 14   /* the menu-bar flyouts' corner radius, the dock's own (see kernel.c) */
#define GUI_APP_COUNT   40 /* 38 real apps + the Apps folder + Trash; Chess (37) pushed Apps/Trash to 38/39; Snake (36) pushed Apps/Trash to 37/38; Depths (35) to 36/37; Bricks (34) to 35/36; Paddle (33) to 34/35; Solitaire (32) to 33/34; Books (31) pushed Apps/Trash to 32/33; 2.2 Music (24) and Movies (25) pushed Apps/Trash to 26/27, 2.7 Hamurapi (26) to 27/28, 2.8 Windgate (27) to 28/29, 2.11 Panes (28) to 29/30, 2.14 Claude (29) to 30/31, Mines (30) to 31/32 */
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
extern int portfolio_dock;              /* booted with `portfolio`: the hidden Portfolio app shows */
int gui_apps_n(void);                    /* apps the folder shows (Portfolio only in portfolio mode) */
int gui_app_at(int k);
void gui_launch(int icon);               /* kernel.c: open an app, folder or Trash by APPS[] slot */
void gui_refuse_open(int icon);          /* kernel.c: say why an app cannot open */
unsigned char cmos(unsigned char reg);   /* kernel.c: one RTC register */
void cmos_read_time_stable(unsigned char *h, unsigned char *m, unsigned char *wd, unsigned char *dom, unsigned char *mon);   /* kernel.c: the RTC */                   /* folder position to APPS[] slot */
int gui_aa_advance(unsigned char c);                   /* the UI face's advance for c, physical px */
void gui_aa_char(unsigned char c, int px, int py, unsigned int fg, int bg, int cell);   /* one UI-face glyph */
/* kernel.c's boot state and helpers that kernel/desktop.c's gui_run reads (4.23.9). */
extern int boot_to_samantha, boot_to_phone, boot_res_w, boot_res_h;   /* the boot line's samantha, phone and WxH */
extern const char *gui_notice_name;   /* the app named by the "is already open" notice, 0 for none */
void gui_notice_draw(void);
void gui_order_init(void);   /* the dock's slot order, the portfolio build's or the default */
extern const char SC[128];   /* PS/2 set 1 scancode to ASCII */
void r3stress_desktop_round(void);   /* kernel/r3stress.c */
void pdestress_desktop_round(void);
void boot_chime(void);   /* kernel.c: the PC speaker's boot notes (4.25.1: kmain.c plays them) */
char getch(void);        /* kernel.c: the next key off the IRQ ring, halting between ticks */
void r3stress_arm(const char *cl);   /* kernel/r3stress.c: reads its boot-line flag */
#endif
