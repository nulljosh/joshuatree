#ifndef APPVIEW_H
#define APPVIEW_H
/* kernel/appview.c: the i386 app viewport, split out of kernel.c. */
extern int gui_app_windowed;   /* the open app draws in a dock window */
extern int app_win_x, app_win_y, app_win_w, app_win_h;   /* that window's frame, set by gui_launch_from_dock */
extern int app_drag_held, app_drag_on, app_drag_gx, app_drag_gy;
extern int app_view_x, app_view_y, app_view_w, app_view_h;
extern int app_cursor_x, app_cursor_y;
int gui_app_dy(void);
void gui_app_mouse_tick(void);
void gui_draw_app_titlebar(const char *title);
#endif
