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
#define GUI_CALENDAR     2 /* APPS[] slot whose tile gets the live date face, gui_calendar_face */
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

/* The circle and capsule primitives every icon glyph, the hover label and
   the window chrome use. gui_aa_band is their logical AA width (5); the
   i386 icon renderer widens it while it draws into a supersampled buffer.
   At window_scale() 2 with no offscreen target they work in physical
   pixels instead. */
extern int gui_aa_band;
int gui_isqrt(int n);
void gui_fill_circle(int cx, int cy, int r, unsigned int color, unsigned int into);
void gui_capsule_phys(int pcx0, int pcy0, int pcx1, int pcy1, int pr, unsigned int color);
void gui_draw_capsule(int x0, int y0, int x1, int y1, int r, unsigned int color, unsigned int into);

/* Provided by the platform: one line of UI text (DejaVu Sans) with its
   line box's top-left at logical (x, y), and its width in logical pixels.
   i386 answers with font_draw_string and font_string_width; ARM with
   arch/arm64/text.c. */
void gui_text(const char *s, int x, int y, unsigned int fg);
int gui_text_width(const char *s);

#define DOCK_LABEL_BG   0x00F4F1EC /* hover label capsule fill */
#define DOCK_LABEL_EDGE 0x00BDB4A8 /* its hairline edge */
/* The hovered tile's name, centred over cx_center, above the tray whose
   top is logical row y0 (gui_dock_y0()). */
void gui_draw_dock_label(int cx_center, int y0, const char *name);
/* One app window's frame at logical (x, y, w, h): rounded body on the
   wallpaper, content well from y + 30, a hairline under the title band,
   the three traffic lights and the centred name. */
void gui_draw_window_frame(int x, int y, int w, int h, const char *name);

/* Provided by the platform: bold DejaVu Sans with its line box's top-left at logical (lx, ly), face 0..3 a 16, 20,
   24 or 28 physical pixel face, mul an integer upscale; and the width of that string in logical pixels, rounded up.
   i386 answers with wx_text, ARM with arch/arm64/text.c. */
void gui_icon_text(const char *s, int lx, int ly, int face, int mul, unsigned int fg);
int gui_icon_text_w(const char *s, int face, int mul);
/* The Calendar tile's live face, over its blank page art: the month in red and the day in ink, for month 1..12 and
   day 1..31. Month 0 means the date is unknown (the Pi has no battery clock), and the tile shows a red header dash and
   an ink dash instead of a made-up date. */
void gui_calendar_face(int cx_center, int cy_bottom, int size, int month, int day);

#endif
