/* The Apps folder (Launchpad), out of kernel.c since 4.20.8: every app as a grid on glass over the wallpaper, driven by
   arrows and Enter as well as the mouse. */
#include "libc.h"
#include "serial.h"
#include "window.h"
#include "font.h"
#include "mouse.h"
#include "gui_prims.h"
#include "gui_paint.h"
#include "dock_geom.h"
#include "dock_draw.h"
#include "desktop.h"
#include "icons.h"
#include "appsfolder.h"
#include "gui_shapes.h"
#include "irq.h"
#include "task.h"
#include "hint.h"

#include "clockicon.h"

/* v37: the Apps folder. Every real app, laid out as a grid, so the dock
   can stay a short pinned list instead of growing until the icons are too
   small to read. Arrow keys + enter drive it as well as the mouse: the
   headless test harness can't inject usable mouse events (see
   guitest.sh's header for that whole trace), and an app screen only
   reachable by mouse would be an app screen this project can never
   regression-test. */
#include "apps_geom.h"
int apps_vis_rows = 3; /* rows the open folder shows; set by gui_launch_from_dock, see apps_geom.h */
/* The framebuffer has no alpha channel. Blend each glass pixel against the
   wallpaper already underneath it, keeping the real photo visible. */
static void gui_apps_glass(int x, int y, int w, int h){
    int sc = (int)window_scale(), radius = 24 * sc;
    int px0 = x * sc, py0 = y * sc, pw = w * sc, ph = h * sc;
    for (int py = 0; py < ph; py++) {
        for (int px = 0; px < pw; px++) {
            int cx = px < radius ? radius : (px >= pw - radius ? pw - radius - 1 : -1);
            int cy = py < radius ? radius : (py >= ph - radius ? ph - radius - 1 : -1);
            if (cx >= 0 && cy >= 0) {
                int dx = px - cx, dy = py - cy;
                if (dx * dx + dy * dy > radius * radius) continue;
            }
            unsigned int below = window_get_pixel_phys(px0 + px, py0 + py);
            unsigned int tint = gui_lerp(0x00F7F1EA, 0x00D8CAD0, py, ph);
            unsigned int glass = gui_lerp(below, tint, 76, 100);
            if (py < 2 * sc) glass = gui_lerp(glass, 0x00FFFFFF, 45, 100);
            window_pixel_phys(px0 + px, py0 + py, glass);
        }
    }
}
static void gui_apps_draw_grid(const struct apps_geom *g, int scroll_offset, int sel){
    for (int k = 0; k < gui_apps_n(); k++) {
        int i = gui_app_at(k);
        int row = k / APPS_COLS - scroll_offset;
        int col = k % APPS_COLS;
        if (row < 0 || row >= g->vis) continue; /* by row count: a row past the panel would spill and leave stale pixels */
        int cx = g->x0 + col * g->cell_w + g->cell_w / 2;
        int cy = g->y0 + row * g->cell_h;
        if (k == sel) gui_rounded_rect_gradient(cx - g->tile / 2 - 12, cy - 8, g->tile + 24, g->cell_h - 4,
                                                 0x00FFF8F1, 0x00E5D8D0, 0x00E9DEE0, 12);
        gui_draw_one_icon_on(i, cx, cy + g->tile, g->tile, 0x00E9DEE0);
        int lw = font_string_width(APPS[i].name);
        font_draw_string(APPS[i].name, cx - lw / 2, cy + g->tile + 8, 0x001C1C1E, -1);
    }
}
static void gui_apps_redraw_panel(const struct apps_geom *g, int scroll_offset, int sel){
    gui_draw_wallpaper_rect(g->px, g->py, g->pw, g->ph);
    gui_apps_glass(g->px, g->py, g->pw, g->ph);
    /* The title bar already reads "Apps"; only the key hint sits inside the panel. */
    gui_draw_hint(g->x0, g->py + APPS_PAD + 2, "arrow keys to move   enter opens   esc closes", 0x006A6064);
    gui_apps_draw_grid(g, scroll_offset, sel);
    serial_puts("appsgridrepaint\n");
}
int gui_multiwin_open(int icon); void gui_refuse_open(int icon);
/* 2.0 gate 5: the Apps folder has no window of its own to host an app, so a launch closes the folder and opens the app as a compositor window, exactly a dock click (full table: the same refusal notice). */
static void gui_apps_launch(int icon){ window_clear_viewport(); /* the window is sized and clamped against the whole screen, not the folder's smaller viewport */ if (gui_multiwin_open(icon) < 0) gui_refuse_open(icon); }

/* One serial line per full repaint with the real layout, so the checks measure what was drawn (tools/checks/launchpad-centered-check.py). */
static void gui_apps_log_geom(const struct apps_geom *g){
    const char *k[] = {" vis=", " px=", " py=", " pw=", " ph=", " x0=", " y0=", " cw=", " ch=", " tile=", " vx=", " vy="};
    int v[] = {g->vis, g->px, g->py, g->pw, g->ph, g->x0, g->y0, g->cell_w, g->cell_h, g->tile, app_view_x, app_view_y};
    char b[12];
    serial_puts("appsgeom");
    for (int n = 0; n < 12; n++) { serial_puts(k[n]); app_utoa((unsigned)v[n], b); serial_puts(b); }
    serial_puts("\n");
}
static void gui_launch_apps_run(void){
    int sel = 0;
    int rows = (gui_apps_n() + APPS_COLS - 1) / APPS_COLS;
    int vw = (int)window_width(), vis, py;
    if (gui_app_windowed) { vis = apps_vis_rows; py = APPS_MARGIN; } /* boxed: the window was sized and centered around the panel */
    else { int top = GUI_MENUBAR_H, bot = gui_dock_y0(); vis = apps_rows_fit(top, bot); py = apps_panel_y(top, bot, vis); } /* bare desktop: centered between menu bar and dock */
    struct apps_geom G = apps_geom_make(vw, vis, py);
    const struct apps_geom *g = &G;
    int scroll_offset = 0; /* v0.77.0: mouse wheel scroll support, apps offset by row */

    /* The wallpaper behind this folder never changes while it is open, so it
       is painted once here and again only after an app has drawn over the
       screen. Every other change (selection, scroll) repaints the panel rect
       alone through gui_apps_redraw_panel. Repainting the wallpaper on every
       poll tick is what made this screen flash while scrolling or typing. */
    int full = 1;
    int clock_min_seen = -1;
    (void)rows;

    for (;;) {
        if (full) {
            full = 0;
            serial_puts("appsfullrepaint\n");
            gui_apps_log_geom(g);
            window_clear(0x00201922);
            /* Not gui_draw_wallpaper(): that helper starts at GUI_MENUBAR_H,
               skipping the top strip to leave room for the desktop's own
               system menu bar. This folder runs inside gui_launch_from_
               dock's viewport, which already excludes the window's own
               title bar (drawn outside the viewport) and has no menu bar
               of its own, so local y=0 is real content, not chrome.
               Starting at GUI_MENUBAR_H left that strip as whatever
               window_clear above set it to: a dead black band under the
               title bar, never painted with wallpaper at all. */
            gui_draw_wallpaper_rows(0, (int)window_height()); if (!gui_app_windowed) { gui_menubar_force_redraw(); gui_draw_menubar(); } /* 2.6.34: full-screen Launchpad keeps the bar */
            gui_apps_redraw_panel(g, scroll_offset, sel);
            /* The click that opened this folder (or closed the app launched
               from it) is the baseline, not a fresh click. Synced here, once
               per real repaint, never per loop pass: a per-pass sync threw
               away every click that landed during the present+sleep below,
               so on a host where wheel/selection events kept the loop
               turning, the folder could not be closed by the pointer at all
               (CI run 35523..., Apps: still open after 5 clicks over 20s). */
            mouse_click_edge_sync();
        }

        /* The same two v86/touch accommodations gui_wait_close documents:
           a few real ticks of settle time so the emulator's canvas sampler
           actually catches this frame before we block, and a click/tap
           counting as input so a phone can leave this screen at all. */
        window_present(); sleep_ticks(5);
        if (gui_clock_tick(&clock_min_seen)) gui_apps_redraw_panel(g, scroll_offset, sel); /* Clock hands follow the minute */
        /* v0.77.0: mouse wheel scroll to browse all apps, one row per scroll. */
        int k = get_key_or_click_until(ticks() + 100); /* wakes each second */
        if (k == KEY_WHEEL_UP || k == KEY_WHEEL_DOWN) {
            /* The view scrolls where the wheel says, full stop. Snapping the
               offset back to keep the selection on screen (what the first cut
               of this did) made the wheel look broken: one notch scrolled and
               the next frame jumped right back. Selection follows the view on
               the keyboard path below, not the other way round. */
            int old_offset = scroll_offset;
            int max_scroll = rows - g->vis;
            if (max_scroll < 0) max_scroll = 0;
            scroll_offset += (k == KEY_WHEEL_UP) ? -1 : 1;
            if (scroll_offset < 0) scroll_offset = 0;
            if (scroll_offset > max_scroll) scroll_offset = max_scroll;
            if (scroll_offset != old_offset)
                gui_apps_redraw_panel(g, scroll_offset, sel);
            continue;
        }
        if (k == KEY_ESC) return;
        if (k == KEY_CLICK) {
            /* v86 (0.71.0) real bug, confirmed by reading this function:
               unlike the dock (whose tile clicks are hit-tested by
               gui_dock_hit_test in gui_run's main loop) or the Apps-folder
               TILE itself on the dock (also hit-tested the same way),
               every click reaching this screen used to be treated as
               "close the folder" unconditionally, with zero hit test
               against the grid cells drawn just above. Clicking a fleet
               app tile inside the launchpad therefore never opened it,
               it just dismissed the launchpad, matching the exact report
               ("clicking a fleet app inside it does nothing except
               dismiss the launchpad"). Only the keyboard path
               (arrows+Enter, or digits '1'-'9' for the first 9 of 22
               apps) ever actually launched anything from here. Real fix:
               hit-test app_cursor_x/y (already tracked live by
               gui_app_mouse_tick, the same position get_key_or_click's
               own KEY_CLICK just fired from) against each cell's real
               drawn bounds (same cx/cy/tile/cell_w/cell_h math as the
               draw loop above) and launch that app on a hit, exactly
               the same "click a tile to open it" contract the dock
               already has; a click outside every cell still closes the
               folder, unchanged behavior for the "tap anywhere to leave"
               phone case. */
            /* app_cursor_x/y are FULL-SCREEN logical coordinates (see
               gui_app_mouse_tick: it calls mouse_get_absolute() in the
               brief window between window_clear_viewport() and the
               matching window_set_viewport(), when window_width()/
               window_height() report the full screen, the same
               convention CLOSE_X/CLOSE_Y in landing/v86/embed.js already
               rely on), while x0/y0/cx/cy below are VIEWPORT-relative
               (this function draws through the viewport gui_launch_from_
               dock already set up). Subtract the viewport origin before
               comparing, or every hit test here silently misses. */
            int click_vx = app_cursor_x - app_view_x, click_vy = app_cursor_y - app_view_y;
            int hit = -1;
            for (int k = 0; k < gui_apps_n(); k++) {
                int i = gui_app_at(k);
                int row = k / APPS_COLS - scroll_offset;
                int col = k % APPS_COLS;
                /* Skip rows that are scrolled off-screen. Bounded by row
                   count (APPS_VIS_ROWS), not a repeated pixel-height
                   literal: this hit test used to compare against the
                   panel's old, wrong 375px height (see APPS_PANEL_H's own
                   note), a second copy of the exact bug class
                   gui_apps_draw_grid's row bound was already fixed for. */
                if (row < 0 || row >= g->vis) continue;
                int cx = g->x0 + col * g->cell_w + g->cell_w / 2;
                int cy = g->y0 + row * g->cell_h;
                int cell_x0 = cx - g->cell_w / 2, cell_y0 = cy - 8, cell_x1 = cell_x0 + g->cell_w, cell_y1 = cy + APPS_LABEL_H + 4;
                if (click_vx >= cell_x0 && click_vx < cell_x1 && click_vy >= cell_y0 && click_vy < cell_y1) { hit = i; break; }
            }
            if (hit >= 0) { gui_apps_launch(hit); return; }
            return; /* a tap outside every tile still closes the folder: with no keyboard there is no other way out */
        }
        if (k == KEY_ENTER) { gui_apps_launch(gui_app_at(sel)); return; }
        int old_sel = sel;
        if ((k == 'a' || k == KEY_LEFT) && sel > 0) sel--;                 /* left (the hint says arrow keys) */
        else if ((k == 'd' || k == KEY_RIGHT) && sel < gui_apps_n() - 1) sel++; /* right */
        else if ((k == 'w' || k == KEY_UP) && sel >= APPS_COLS) sel -= APPS_COLS;
        else if ((k == 's' || k == KEY_DOWN) && sel + APPS_COLS < gui_apps_n()) sel += APPS_COLS;
        else if (k >= '1' && k <= '9' && (k - '1') < gui_apps_n()) { gui_apps_launch(gui_app_at(k - '1')); return; }
        if (sel != old_sel) {
            /* Keyboard selection drags the view with it, the direction that is
               not surprising: move past the last visible row and the grid
               follows. */
            int sel_row = sel / APPS_COLS;
            if (sel_row < scroll_offset) scroll_offset = sel_row;
            if (sel_row >= scroll_offset + g->vis) scroll_offset = sel_row - g->vis + 1;
        }
        if (sel != old_sel) gui_apps_redraw_panel(g, scroll_offset, sel);
    }
}

/* The web demo forwards Esc to the kernel only while a screen of ours is up that Esc closes, so the open and close of the Launchpad are announced on serial (landing/v86/embed.js, tools/checks/launchpad-exit-check.py). */
void gui_launch_apps(void){ serial_puts("appsopen\n"); gui_launch_apps_run(); serial_puts("appsclose\n"); }
