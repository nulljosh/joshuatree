#ifndef DOCK_DRAW_H
#define DOCK_DRAW_H

#include "dock_geom.h"

/* Dock pixel drawing and its hover/drag animation, split out of kernel.c
   to keep it under the godfile-check.sh ceiling. dock_geom.h already
   covers pure layout math (tile size, slot rects, hit-testing); this is
   the band cache, the tray, the icons and the hover label drawn on top
   of that geometry. */

void gui_draw_dock(int hover_slot, int drag_slot, int drag_mx, int drag_my);
void gui_redraw_dock_band(int hover_slot, int drag_slot, int drag_mx, int drag_my);
void gui_dock_prewarm(void);
int gui_dock_band_top(void);
/* Frees the band cache/frame buffers; called once by gui_run() on exit so
   the GUI heap allocations don't linger past the session. */
void gui_dock_band_cache_free(void);

/* dock_band_cache_top: non-static so kernel.c's wall_caches_drop() can
   invalidate the band cache when the wallpaper theme or resolution
   changes. Owned by dock_draw.c. */
extern int dock_band_cache_top;

/* dock_presented_hover: which hover slot's pixels were last actually
   painted. Set inside gui_redraw_dock_band; also reset by kernel.c's
   gui_run alongside dock_hover on session start. */
extern int dock_presented_hover;

/* dock_hover / gui_order: owned by kernel.c (the gui_run event loop
   drives hover state and drag/drop reordering); read here to know what
   to draw. */
extern int dock_hover;
extern int gui_order[GUI_ICON_COUNT];

#endif
