#ifndef GUI_PAINT_H
#define GUI_PAINT_H

/* The desktop painters both builds share (kernel/gui_paint.c): the i386
   kernel and the ARM64 port link the same object source. Integer only.
   They draw through window.h's physical-pixel calls and the platform's
   gui_wallpaper_sample, which each build provides. */

/* The default dock, slot by slot: the Apps folder, Burrow, Mail, Calendar,
   Notes, Reminders, Terminal, Chat, Weather, Stocks, Trash. Indices are
   APPS[] and ICON_ART[] slots. */
#define GUI_APPS_FOLDER 30 /* not an app: the dock tile that opens the folder */
#define GUI_TRASH       31
#define GUI_DOCK_DEFAULT_ORDER {GUI_APPS_FOLDER, 0, 1, 2, 3, 4, 5, 6, 7, 18, GUI_TRASH}

/* Channel-wise average and t/max interpolation of two 0x00RRGGBB colours. */
unsigned int gui_blend(unsigned int a, unsigned int b);
unsigned int gui_lerp(unsigned int a, unsigned int b, int t, int max);

/* Provided by the platform: the wallpaper's colour at physical (px, py).
   i386 samples its wallpaper cache; ARM reads the framebuffer, so the dock
   has to be painted while only the wallpaper sits under it. */
unsigned int gui_wallpaper_sample(int px, int py, int sway);

/* A rounded rect whose antialiased corners blend into the wallpaper. */
void gui_rounded_rect_on_wallpaper(int x, int y, int w, int h, unsigned int color, int r);
/* A one physical pixel rule along logical row y. */
void gui_hairline_h(int x, int y, int w, unsigned int color);
/* The dock's tray: its soft shadow, rounded body and hairline edges, at
   the geometry dock_geom.h computes for the current screen. */
void gui_draw_dock_tray(void);
/* The soft contact shadow under one dock icon. */
void gui_draw_icon_shadow(int cx_center, int cy_bottom, int size);
/* The authored icon artwork's edge in pixels: icon_art.h's ICON_ART_SIZE,
   which kernel.c checks against this at compile time. */
#define GUI_ICON_ART_SIZE 148
/* Scales decoded ICON_ART_SIZE RGBA artwork to a pw x pw tile composited
   over `under` (area filter down, bilinear up). */
void gui_icon_art_scale(const unsigned char *art, unsigned int *out, int pw, unsigned int under);
void gui_icon_art_bilinear(const unsigned char *art, unsigned int *out, int pw, unsigned int under);
/* Draws a finished tile at logical (x, y), skipping its `under` corners. */
void gui_blit_tile(const unsigned int *tile, int x, int y, int size, unsigned int under);

#endif
