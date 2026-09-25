#include "dock_geom.h"
#include "window.h"

int gui_dock_icon(void){
    int by_height = (int)window_height() * dock_scale_pct / 100;
    int max_by_width = (DOCK_BUDGET - 2 * DOCK_PAD - (GUI_ICON_COUNT - 1) * DOCK_GAP) / GUI_ICON_COUNT;
    if (by_height > max_by_width) by_height = max_by_width;
    if (by_height < 16) by_height = 16; /* below this the vector glyphs stop being legible at all */
    return by_height;
}

int gui_dock_w(void){ return GUI_ICON_COUNT * DOCK_ICON + (GUI_ICON_COUNT - 1) * DOCK_GAP + 2 * DOCK_PAD; }
int gui_dock_x0(void){ return ((int)window_width() - gui_dock_w()) / 2; }
int gui_dock_y0(void){ return (int)window_height() - DOCK_ICON - 2 * DOCK_PAD - DOCK_MARGIN_BOT; }
int gui_slot_x(int slot){ return gui_dock_x0() + DOCK_PAD + slot * (DOCK_ICON + DOCK_GAP); }

/* Which dock slot a point falls in, clamped to the nearest end rather than
   returning "none": once a drag has started, the icon should track the
   cursor even past the dock's own edge, the same way a real dock does. */
int gui_slot_at(int mx){
    /* v63: was `- DOCK_ICON / 2`, since v15. That put every slot boundary
       at the CENTRE of a drawn tile, so the left half of each icon (and
       the gap before it) hit-tested as the previous slot: the magnified
       icon sat one tile to the left of the cursor half the time, caught
       in a real framebuffer dump (cursor over Reminders, Notes lifted).
       Half a gap either side of each tile now belongs to that tile. */
    int rel = mx - (gui_dock_x0() + DOCK_PAD) + DOCK_GAP / 2;
    int slot = rel / (DOCK_ICON + DOCK_GAP);
    if (rel < 0) slot = 0;
    if (slot < 0) slot = 0;
    if (slot >= GUI_ICON_COUNT) slot = GUI_ICON_COUNT - 1;
    return slot;
}

/* Only counts as being "over the dock" within its actual drawn rect,
   unlike gui_slot_at (used once a drag is already underway, where the
   dragged icon should keep tracking the cursor even briefly outside it). */
int gui_dock_hit_test(int mx, int my){
    int y0 = gui_dock_y0(), h = DOCK_ICON + 2 * DOCK_PAD;
    if (my < y0 - 20 || my >= y0 + h) return -1;
    int x0 = gui_dock_x0(), w = gui_dock_w();
    if (mx < x0 || mx >= x0 + w) return -1;
    return gui_slot_at(mx);
}
