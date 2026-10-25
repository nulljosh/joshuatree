/* The i386 app viewport, out of kernel.c since 4.23.8: gui_app_windowed (an app drawing in a dock window, not
   full screen) and its content shift, the live title-bar drag gui_app_mouse_tick runs while a blocking app waits
   for input, and the full-screen title strip. i386 only. */
#include "libc.h"
#include "window.h"
#include "font.h"
#include "mouse.h"
#include "serial.h"
#include "gui_prims.h"
#include "gui_paint.h"
#include "dock_geom.h"
#include "desktop.h"
#include "wallpaint.h"
#include "phone_home.h"
#include "appview.h"

/* App viewers have their own input loops. Keep the pointer alive while one
   is open, drawing it in screen coordinates outside the app viewport. */
int gui_app_windowed = 0;
/* Vertical shift for an app's own content. Full screen, an app draws its
   own title strip across the top 40px and starts content at y=52. In a
   dock window the frame already draws the title bar above the viewport,
   so the same layout moves up by that strip, the same 32px Stocks has
   always saved through stx_top(). Add it to every content y. */
int gui_app_dy(void){ return gui_app_windowed ? -32 : 0; }
/* Live title-bar drag for the blocking single-window apps (everything
   that opens through gui_launch_from_dock: Notes, Terminal, Chat, the
   fleet apps, Settings...). Direct request: "app windows should be
   draggable". Only the five multi-window apps could move before, and
   only as a snap-preview outline. These apps block inside their own
   input loops, so the one place that runs while they wait is
   gui_app_mouse_tick below: it watches the left button, arms on a press
   in the title band right of the traffic lights, and on every pointer
   move shifts the whole window (chrome and content) inside the back
   buffer with window_move_rect, repaints the one or two wallpaper strips
   the window just uncovered, and slides the app's own viewport along, so
   whatever the app draws next lands at the new place. The press that
   starts a drag is swallowed from mouse_click_edge, so it is never also
   the app's "click anywhere closes"; a press on the lights or the
   content keeps every contract it had. Clamped to the desktop strip
   (below the menu bar, above the dock band) so the uncovered area is
   always plain wallpaper, nothing else needs redrawing. */
int app_win_x = 0, app_win_y = 0, app_win_w = 0, app_win_h = 0;
int app_drag_held = 0, app_drag_on = 0, app_drag_gx = 0, app_drag_gy = 0;
int app_view_x, app_view_y; int app_view_w, app_view_h;
int app_cursor_x, app_cursor_y;
void gui_app_mouse_tick(void){
    if (!gui_app_windowed) return;
    int dx = 0, dy = 0, buttons = 0;
    int moved = mouse_get_delta(&dx, &dy, &buttons);
    if (!moved && cursor_saved_x >= 0) return;
    window_clear_viewport();
    gui_cursor_restore();
    app_cursor_x += dx; app_cursor_y += dy;
    mouse_get_absolute(&app_cursor_x, &app_cursor_y, (int)window_width(), (int)window_height()); /* v62: absolute pointer wins over the relative walk when the backdoor is live; viewport already cleared above, so this is the full screen */
    if (app_cursor_x < 0) app_cursor_x = 0;
    if (app_cursor_y < 0) app_cursor_y = 0;
    if (app_cursor_x > (int)window_width() - CURSOR_W) app_cursor_x = (int)window_width() - CURSOR_W;
    if (app_cursor_y > (int)window_height() - CURSOR_H) app_cursor_y = (int)window_height() - CURSOR_H;
    if (jt_phone_mode()) phone_back_zone_tick(buttons, app_drag_held, app_cursor_x, app_cursor_y); /* phone_home.h: back-chevron tap, no Esc key on a phone */ /* Live window drag (app_win_x's comment). Press edge: arm only in the
       title band, right of the three lights (x + 80 on), so the red
       close light and the app's own content keep their click semantics. */
    int held = buttons & 1;
    if (held && !app_drag_held) {
        app_drag_held = 1;
        if (app_win_w > 0 && app_cursor_y >= app_win_y && app_cursor_y < app_win_y + 30
            && app_cursor_x >= app_win_x + 80 && app_cursor_x < app_win_x + app_win_w) {
            app_drag_on = 1;
            app_drag_gx = app_cursor_x - app_win_x; app_drag_gy = app_cursor_y - app_win_y;
            mouse_click_edge(); /* consumed: this press is a grab, not the app's click */
        }
    } else if (!held && app_drag_held) {
        app_drag_held = 0; app_drag_on = 0;
    }
    if (app_drag_on) {
        int nx = app_cursor_x - app_drag_gx, ny = app_cursor_y - app_drag_gy;
        int max_x = (int)window_width() - app_win_w, max_y = gui_dock_band_top() - app_win_h;
        if (nx > max_x) nx = max_x;  if (nx < 0) nx = 0;
        if (ny > max_y) ny = max_y;  if (ny < GUI_MENUBAR_H) ny = GUI_MENUBAR_H;
        int mdx = nx - app_win_x, mdy = ny - app_win_y;
        if (mdx || mdy) {
            window_move_rect(app_win_x, app_win_y, app_win_w, app_win_h, mdx, mdy);
            /* the strips the window no longer covers: one per axis moved */
            if (mdx > 0)      gui_daynight_wallpaper_rect(app_win_x, app_win_y, mdx, app_win_h);
            else if (mdx < 0) gui_daynight_wallpaper_rect(app_win_x + app_win_w + mdx, app_win_y, -mdx, app_win_h);
            if (mdy > 0)      gui_daynight_wallpaper_rect(app_win_x, app_win_y, app_win_w, mdy);
            else if (mdy < 0) gui_daynight_wallpaper_rect(app_win_x, app_win_y + app_win_h + mdy, app_win_w, -mdy);
            app_win_x = nx; app_win_y = ny;
            app_view_x += mdx; app_view_y += mdy;
            serial_puts("windrag\n"); /* marker for tools/checks/windowdrag-check.py */
        }
    }
    if (!jt_phone_mode()) { gui_cursor_save(app_cursor_x, app_cursor_y); gui_draw_cursor(app_cursor_x, app_cursor_y); } /* phone_home.h: touch has no cursor, never draw the desktop arrow over an open app */
    window_set_viewport(app_view_x, app_view_y, (unsigned int)app_view_w, (unsigned int)app_view_h);
}

/* A real macOS-style traffic light, not a fake one: red is a genuine close
   affordance, clicking anywhere already closes the app view (gui_wait_close
   polls for exactly that), this just gives that real behavior the familiar
   visual target instead of an invisible whole-screen hitbox. Yellow and
   green are drawn unlit on purpose: this kernel has no real windowing
   system yet, apps are always full-screen with no minimize/restore state
   to actually go to, so a lit, clickable minimize or maximize button would
   be a fake control that looks like it does something it doesn't. Real
   minimize/maximize wait on the actual windowing system already queued in
   ROADMAP.md's later product ideas, not a shortcut bolted on here. */
void gui_draw_app_titlebar(const char *title){
    if (gui_app_windowed) return;
    if (jt_phone_mode()) { phone_app_titlebar_draw(title); return; } /* kernel/phone_home.h: back chevron, no Esc key on a phone */
    gui_fill_circle(26, 20, 6, 0x00FF5F57, 0x00FAF8F6);
    gui_fill_circle(46, 20, 6, 0x00FFD64A, 0x00FAF8F6);
    gui_fill_circle(66, 20, 6, 0x00D8D4CE, 0x00FAF8F6);
    gui_close_glyph(26, 20, 0x00FF5F57);
    font_draw_string("-", 43, 12, 0x00624A20, -1);
    font_draw_string(title, 84, 12, 0x00555555, -1);
}
