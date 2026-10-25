#ifndef MULTIWIN_H
#define MULTIWIN_H
/* kernel/multiwin.c: the desktop's window manager, which gui_run in kernel.c drives. Moved out of kernel.c in 4.19.1. */
#define GUI_MULTIWIN_MAX 2
typedef struct {
    int icon;
    int x, y, w, h;
    int task; /* 1.9.23: the ring-3 task drawing this window, -1 for an in-kernel draw hook */
    int shown; /* 1.9.23: 0 until the compositor has drawn it once (an open= boot launch lands before the first frame) */
} gui_window_t;
int ring3app_launch_window(const char *name, unsigned int w, unsigned int h);   /* ring3app.c: the compositor windows */
void ring3app_window_reaped(int task, int status);
void ring3app_window_blit(int task, int vw, int vh);
int ring3app_is_windowable(const char *name);
int gui_ring3_windowed(int icon);
extern gui_window_t gui_windows[GUI_MULTIWIN_MAX];
extern int gui_window_count;
int gui_multiwin_supported(int icon);
int gui_multiwin_dock_ok(int icon);
int gui_multiwin_interactive(int icon);
int gui_bleed_fits(void);
int gui_window_bleed(int icon);
int gui_bleed_open(void);
void gui_multiwin_geom(int slot_index, int *x, int *y, int *w, int *h);
int gui_snap_area(int *top, int *bottom);
int gui_snap_zone(int mx, int my);
void gui_snap_target(int zone, int *x, int *y, int *w, int *h);
void gui_snap_outline(int zone);
void gui_multiwin_draw_chrome(const gui_window_t *win);
void gui_multiwin_draw_content_only(const gui_window_t *win);
void gui_multiwin_draw_one(const gui_window_t *win);
void gui_multiwin_draw_all(void);
void gui_multiwin_focus(int idx);
extern int gui_switcher_open;
extern int gui_switcher_idx;
void gui_switcher_draw(int hi);
void gui_write_u32le(unsigned char *p, unsigned int v);
void gui_write_u16le(unsigned char *p, unsigned int v);
int gui_screenshot_next_name(char *out /* 13 bytes, "SHOTNNNN.BMP\0" */);
unsigned int gui_screenshot_save(char *name_out /* 13 bytes */);
void gui_screenshot_flash(const char *name);
int gui_multiwin_open(int icon);
int gui_multiwin_hit_test(int mx, int my);
void gui_multiwin_close(int idx);
int gui_multiwin_key_nonblock(void);
#endif
