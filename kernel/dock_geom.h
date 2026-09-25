#ifndef DOCK_GEOM_H
#define DOCK_GEOM_H

/* Dock geometry and hit-testing, split out of kernel.c. Pure layout math:
   how big a tile is, where the dock's rect sits, which slot a point falls
   in. No drawing here, that stays in kernel.c's gui_draw_dock* code. */

#define GUI_ICON_COUNT 11

/* v36 (0.36.0): the icon size is *derived* from how many icons there are,
   instead of a constant that silently overflows the screen. DOCK_BUDGET is
   the widest the dock may ever draw, leaving a real margin on both sides
   of the 800px screen. */
#define DOCK_BUDGET     740
#define DOCK_GAP        6
#define DOCK_PAD        10
#define DOCK_MARGIN_BOT 24
#define DOCK_TRAY_COLOR 0x00EFEBE4 /* the one surface colour every dock tile is blended against */

/* The user-adjustable Settings knob (5-25%), defined non-static in
   kernel.c so gui_dock_icon() here can read it. */
extern int dock_scale_pct;

int gui_dock_icon(void);
#define DOCK_ICON (gui_dock_icon())

int gui_dock_w(void);
int gui_dock_x0(void);
int gui_dock_y0(void);
int gui_slot_x(int slot);
int gui_slot_at(int mx);
int gui_dock_hit_test(int mx, int my);

#endif
