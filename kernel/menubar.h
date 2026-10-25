#ifndef MENUBAR_H
#define MENUBAR_H
/* kernel/menubar.c: the i386 menu bar, split out of kernel.c. */
extern int weather_hit_x0, weather_hit_x1;   /* the menu-bar weather text, -1 when nothing is drawn */
extern int gui_menubar_last_min;   /* the minute last drawn; gui_run redraws the cursor when it changes */
void gui_menubar_force_redraw(void);
void gui_draw_menubar(void);
#endif
