#ifndef SETTINGS_UI_H
#define SETTINGS_UI_H
/* kernel/settings_ui.c: the i386 Settings app (sidebar and grouped rows). */
int settings_row_at(int cx, int cy, int ww, int cur_section);
int settings_sidebar_at(int cx, int cy);
void gui_launch_settings(void);
#endif
