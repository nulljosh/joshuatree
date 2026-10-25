#ifndef WALLPAINT_H
#define WALLPAINT_H
/* kernel/wallpaint.c: the i386 wallpaper painters and the day/night tint, split out of kernel.c. */
extern int wind_phase;   /* -256..256, the sway gui_run's wind tick steps */
void daynight_calc(int hour, int *night_out, int *day_out);
unsigned int gui_daynight_tint_pct(unsigned int rgb, int night, int day);
unsigned int gui_daynight_tint(unsigned int rgb);
unsigned int gui_wallpaper_color(int row);
unsigned int gui_wallpaper_sample(int px, int py, int sway);
void gui_draw_wallpaper_rows(int y_from, int y_to);
void gui_draw_wallpaper_rows_sway(int y_from, int y_to, int sway);
void gui_draw_wallpaper_rows_sway_ex(int y_from, int y_to, int sway, int ex, int ey, int ew, int eh);
void gui_draw_wallpaper(void);
void gui_draw_wallpaper_rect(int x, int y, int w, int h);
void gui_daynight_wallpaper_rect(int x, int y, int w, int h);
void gui_wind_base_build(void);
void gui_wallpaint_caches_drop(void);
#endif
