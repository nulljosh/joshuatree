#ifndef PHONE_HOME_H
#define PHONE_HOME_H
/* kernel/phone_home.c: phone mode's home screen, status bar and back chevron. */
void phone_app_titlebar_draw(const char *title);
void phone_back_zone_tick(int buttons, int app_drag_held, int cursor_x, int cursor_y);
void phone_window_run(int icon);
void phone_home_run(void);
#endif
