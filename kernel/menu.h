#ifndef MENU_H
#define MENU_H
/* kernel/menu.c: the Apple menu off the tree mark, and the menu-bar weather dropdown (gui_draw_weather_panel, declared in desktop.h). */
int gui_menu_hit_test(int mx, int my);   /* the row under the pointer, -1 for none */
void gui_draw_apple_menu(int hover);
#endif
