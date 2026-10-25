#ifndef ICONS_H
#define ICONS_H
/* kernel/icons.c: the i386 app icon painters (the APPS table in kernel.c names them). */
void gui_icon_activity(int cx, int cy, int s, unsigned int bg);
void gui_icon_apps(int cx, int cy, int s, unsigned int bg);
void gui_icon_book(int cx, int cy, int s, unsigned int bg);
void gui_icon_calculator(int cx, int cy, int s, unsigned int bg);
void gui_icon_calendar(int cx, int cy, int s, unsigned int bg);
void gui_icon_chat(int cx, int cy, int s, unsigned int bg);
void gui_icon_contacts(int cx, int cy, int s, unsigned int bg);
void gui_icon_fieldbook(int cx, int cy, int s, unsigned int bg);
void gui_icon_folder(int cx, int cy, int s, unsigned int bg);
void gui_icon_hikko(int cx, int cy, int s, unsigned int bg);
void gui_icon_keyrate(int cx, int cy, int s, unsigned int bg);
void gui_icon_mail(int cx, int cy, int s, unsigned int bg);
void gui_icon_notes(int cx, int cy, int s, unsigned int bg);
void gui_icon_pin(int cx, int cy, int s, unsigned int bg);
void gui_icon_quotes(int cx, int cy, int s, unsigned int bg);
void gui_icon_reminders(int cx, int cy, int s, unsigned int bg);
void gui_icon_search(int cx, int cy, int s, unsigned int bg);
void gui_icon_stocks(int cx, int cy, int s, unsigned int bg);
void gui_icon_terminal(int cx, int cy, int s, unsigned int bg);
void gui_icon_tonchi(int cx, int cy, int s, unsigned int bg);
void gui_icon_toroid(int cx, int cy, int s, unsigned int bg);
void gui_icon_trash(int cx, int cy, int s, unsigned int bg);
void gui_icon_weather(int cx, int cy, int s, unsigned int bg);
void gui_draw_one_icon_on(int icon, int cx_center, int cy_bottom, int size, unsigned int under);
void gui_draw_one_icon(int icon, int cx_center, int cy_bottom, int size);
void gui_fill_triangle_down(int cx, int y0, int half_w, int h, unsigned int color);   /* kernel.c */
#endif
