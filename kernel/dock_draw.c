#include "dock_draw.h"
#include "dock_geom.h"
#include "gui_prims.h"
#include "gui_paint.h"
#include "window.h"
#include "font.h"
#include "app.h"
#include "kheap.h"

/* Primitives that stay defined in kernel.c (used well beyond the dock:
   every flyout, icon glyph and window chrome leans on them too) but that
   this file needs, so they are non-static there and declared here. The
   tray, the contact shadow and the colour math come from gui_paint.h. */
void gui_draw_wallpaper_rows(int y_from, int y_to);
void gui_draw_one_icon(int icon, int cx_center, int cy_bottom, int size);
void gui_draw_capsule(int x0, int y0, int x1, int y1, int r, unsigned int color, unsigned int into);

/* hover_slot: which slot shows the magnify+label (-1 none). drag_slot: the
   slot currently being dragged, drawn separately so it can float free of
   the row under the cursor instead of at its slot position. */
#define DOCK_LABEL_BG   0x00F4F1EC /* hover label capsule fill */
#define DOCK_LABEL_EDGE 0x00BDB4A8 /* its hairline edge */
#define DOCK_LABEL_SPAN 48         /* px either side of a slot a hover change repaints: the widest label plus its capsule */

/* v0.79.x: the dock splits into the half that never changes while the
   pointer moves (the tray's shadow and its rounded body) and the half that
   does (the icons, their contact shadows and the hover label). Measured,
   one hover frame: the tray half is 4442 us of a 9842 us band compose, all
   of it redrawing pixels identical to the ones already there. Baking it
   into the band cache alongside the wallpaper rows it sits on costs
   nothing extra (the cache is built once per resolution) and takes it off
   every single animation frame. Both halves read the wallpaper through
   gui_wallpaper_sample, never through the framebuffer, so a cached tray is
   the same pixels as a freshly drawn one, not an approximation of them. */
static void gui_draw_dock_icons(int drag_slot, int drag_mx, int drag_my);

/* v40: repaint only the dock band: the wallpaper rows behind it, then the
   dock itself. This is what a hover change costs now, instead of a full
   456,000-pixel photo blit plus eight supersampled icons. */
static unsigned int *dock_band_cache = 0;
static unsigned int *dock_band_frame = 0;
int dock_band_cache_top = -1;
int dock_presented_hover = -1;
static void gui_dock_band_cache_build(void){
    int sc = (int)window_scale();
    int top = gui_dock_band_top(), h = (int)window_height() - top;
    int pw = (int)window_width() * sc, ph = h * sc;
    if (dock_band_cache && dock_band_cache_top == top) return;
    if (dock_band_cache) kfree(dock_band_cache);
    if (dock_band_frame) kfree(dock_band_frame);
    dock_band_cache = (unsigned int *)kmalloc((unsigned int)(pw * ph) * sizeof(unsigned int));
    dock_band_frame = (unsigned int *)kmalloc((unsigned int)(pw * ph) * sizeof(unsigned int));
    dock_band_cache_top = top;
    if (dock_band_cache && dock_band_frame) {
        window_push_screen_band(dock_band_cache, top * sc, (unsigned int)ph);
        gui_draw_wallpaper_rows(top, (int)window_height());
        gui_draw_dock_tray();
        window_pop_screen_band();
    }
}

/* Everything the first hover of a session would otherwise pay for mid
   animation: the band cache above (a full-width wallpaper render plus the
   tray), and the magnified tile for every icon, whose cache miss path
   decodes a PNG. Measured, that first hover showed one single size where
   a warm one shows six, and the second showed three, because the work
   landed inside the sixty milliseconds the animation had to run in. Doing
   it here, while the desktop's first frame is already up and nothing is
   animating, costs a boot moment nobody is watching and allocates nothing
   a hover sweep would not have allocated seconds later anyway. */
void gui_dock_prewarm(void){
    gui_dock_band_cache_build();
}

void gui_dock_band_cache_free(void){
    if (dock_band_cache) { kfree(dock_band_cache); dock_band_cache = 0; }
    if (dock_band_frame) { kfree(dock_band_frame); dock_band_frame = 0; }
}

void gui_redraw_dock_band(int hover_slot, int drag_slot, int drag_mx, int drag_my){
    /* v43: the wallpaper rows behind the dock never change, so bilinear
       them once and copy thereafter. ~400k physical samples per hover
       change was the other half of the flash. */
    int sc = (int)window_scale();
    int top = gui_dock_band_top(), h = (int)window_height() - top;
    int pw = (int)window_width() * sc, ph = h * sc;
    gui_dock_band_cache_build();
    if (dock_band_cache && dock_band_frame) {
        for (int i = 0; i < pw * ph; i++) dock_band_frame[i] = dock_band_cache[i];
        window_push_screen_band(dock_band_frame, top * sc, (unsigned int)ph);
        gui_draw_dock_icons(drag_slot, drag_mx, drag_my);
        window_pop_screen_band();
        /* Only present slots whose icon size changed. Copying the whole
           2 MB band on every hover step visibly exposed the half-drawn
           frame even though composition itself was offscreen. */
        for (int slot = 0; slot < GUI_ICON_COUNT; slot++) {
            if ((slot == dock_presented_hover) == (slot == dock_hover)) continue;
            int left = (gui_slot_x(slot) - DOCK_LABEL_SPAN) * sc;
            int right = (gui_slot_x(slot) + DOCK_ICON + DOCK_LABEL_SPAN) * sc;
            if (left < 0) left = 0;
            if (right > pw) right = pw;
            for (int py = 0; py < ph; py++) {
                unsigned int *dst = window_phys_row(top * sc + py);
                for (int px = left; px < right; px++) {
                    unsigned int next = dock_band_frame[py * pw + px];
                    if (dst[px] != next) dst[px] = next;
                }
            }
            /* v0.78.x: this is the one caller that writes through a raw row
               pointer, so it has to declare what it touched. Measured: the
               old "assume the whole row" guess damaged 1920 columns per row
               to change the ~174 this actually writes, which is what made a
               dock hover present 1.96M pixels instead of ~86k. */
            window_damage(left, top * sc, right - left, ph);
            }
        dock_presented_hover = dock_hover;
    } else {
        gui_draw_wallpaper_rows(top, (int)window_height());
        gui_draw_dock(hover_slot, drag_slot, drag_mx, drag_my);
    }
}

static void gui_draw_dock_icons(int drag_slot, int drag_mx, int drag_my){
    int y0 = gui_dock_y0();

    for (int slot = 0; slot < GUI_ICON_COUNT; slot++) {
        if (slot == drag_slot) continue; /* drawn last, floating at the cursor */
        int icon = gui_order[slot];
        int size = DOCK_ICON;
        int cx_center = gui_slot_x(slot) + DOCK_ICON / 2;
        int cy_bottom = y0 + DOCK_PAD + DOCK_ICON;
        gui_draw_icon_shadow(cx_center, cy_bottom, size);
        gui_draw_one_icon(icon, cx_center, cy_bottom, size);
        if (slot == dock_hover) {
            int label_w = font_string_width(APPS[icon].name);
            int ly = y0 - 21; /* capsule spans ly-3 .. ly+19: clear of the tray's top edge, inside the band (y0 - 24) */
            /* Dark text on a light capsule with a hairline edge, the macOS
               dock tooltip, in the tray's own cream. Bare light text read
               on dark wallpaper but vanished on bright map tiles and
               collided with an open window's bottom edge (QA tour,
               2026-09-21); the hairline keeps the capsule distinct over a
               light window. It stays inside the band gui_dock_band_top()
               composes and the per-slot present span DOCK_LABEL_SPAN. */
            int lx0 = cx_center - label_w / 2 - 2, lx1 = cx_center + label_w / 2 + 2;
            gui_draw_capsule(lx0, ly + 8, lx1, ly + 8, 11, DOCK_LABEL_EDGE, DOCK_LABEL_EDGE);
            gui_draw_capsule(lx0, ly + 8, lx1, ly + 8, 10, DOCK_LABEL_BG, DOCK_LABEL_BG);
            font_draw_string(APPS[icon].name, cx_center - label_w / 2, ly, 0x001C1C1E, -1);
        }
    }
    if (drag_slot >= 0) {
        int icon = gui_order[drag_slot];
        gui_draw_one_icon(icon, drag_mx, drag_my + DOCK_ICON / 2, DOCK_ICON);
    }
}

void gui_draw_dock(int hover_slot, int drag_slot, int drag_mx, int drag_my){
    (void)hover_slot;
    gui_draw_dock_tray();
    gui_draw_dock_icons(drag_slot, drag_mx, drag_my);
}
