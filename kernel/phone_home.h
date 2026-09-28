/* kernel/phone_home.h -- roadmap 1.8: a real phone home screen for
   boot_to_phone, instead of the desktop's menu bar + dock + floating
   windows squeezed into 430px (direct report: "still shows a laptop
   desktop squeezed into a phone... too squished"). #included into
   kernel.c (same shape as chat.h/clock.h/etc above it, all real logic
   living in a file kernel.c textually includes rather than a second
   translation unit) so it reads every static the desktop already built:
   APPS[], gui_draw_one_icon_on, gui_apps_launch, weather_text,
   cmos_read_time_stable, GUI_BG. Only active when boot_to_phone; desktop
   mode never calls anything in here, so it stays pixel-identical.

   The app-open half is not new code: gui_apps_launch's existing
   "!was_windowed" branch (used when the Apps folder itself is opened
   un-windowed) already IS a full-screen, no-chrome-margins app host --
   window_set_viewport(0, 40, vw, vh), gui_draw_app_titlebar draws just an
   x/name strip up top, and every app's own gui_wait_close loop already
   treats a click anywhere (or Esc) as "go back". That is exactly items 2
   and 3 of the roadmap ask, for both in-kernel apps and ring-3 apps
   (ring3app.c's viewport is that same app_view_w/app_view_h), so the
   grid below calls it directly instead of re-deriving the same host. */

#define PHONE_HOME_COLS 5
#define PHONE_STATUS_H  44

/* v1.8.0 roadmap items 2/2: a phone has no Esc key, so the desktop's
   three traffic lights on an open app (gui_draw_app_titlebar's own
   branch) are replaced with a single tappable back chevron for
   boot_to_phone, forward-declared near boot_to_phone's own definition so
   gui_draw_app_titlebar and gui_app_mouse_tick (both defined well above
   this #include) can call them. */
static void phone_app_titlebar_draw(const char *title){
    /* The home grid's status bar (its clock, centered at this same y)
       and this titlebar share the same top strip, and nothing else
       clears it before drawing -- without this the app's title rendered
       on top of the stale clock digits underneath (caught in the first
       screenshot of a real app open). */
    window_rect(0, 0, (int)window_width(), 40, GUI_BG);
    font_draw_string("<", 18, 10, 0x001C1C1E, -1);
    int tw = font_string_width(title);
    font_draw_string(title, ((int)window_width() - tw) / 2, 12, 0x00555555, -1);
}
/* Called every gui_app_mouse_tick while an app is open on phone. The
   chevron lives in the top 40px strip, outside every app's own content
   viewport (which starts at y=40), so no app ever sees a tap up here as
   its own click -- a tap consumes the click edge itself (same as the
   desktop's traffic-light drag-arm does for its own zone, right below
   this call in gui_app_mouse_tick) and injects the real ESC make code,
   so the app closes through the exact kbd_pop()==27 path a keyboard's
   Esc key already drives: one close path, not a second one bolted on
   for touch. */
static void phone_back_zone_tick(int buttons, int app_drag_held, int cursor_x, int cursor_y){
    if ((buttons & 1) && !app_drag_held && cursor_y < 40 && cursor_x < 60) {
        mouse_click_edge(); /* consumed here: the app underneath never sees this tap */
        kbd_inject(0x01);
    }
}

/* Status bar: same data gui_draw_menubar reads (cmos_read_time_stable,
   weather_text), phone-styled -- centered weight, no Apple-menu logo (a
   phone home screen has no menu to drop down), house ink-on-cream, no
   translucency trick (that trick samples the desktop's photo/map
   wallpaper per row; the home screen sits on a flat cream field). */
static void phone_status_bar_draw(void){
    window_rect(0, 0, (int)window_width(), PHONE_STATUS_H, GUI_BG);
    window_rect(0, PHONE_STATUS_H - 1, (int)window_width(), 1, 0x00DDD9D3);
    u8 h, m, wd, dom, mon;
    cmos_read_time_stable(&h, &m, &wd, &dom, &mon);
    u8 hv = (h & 0x0F) + ((h >> 4) * 10), mv = (m & 0x0F) + ((m >> 4) * 10);
    int h12 = hv % 12; if (h12 == 0) h12 = 12;
    const char *ampm = hv < 12 ? "AM" : "PM";
    char clock[16]; int p = 0;
    if (h12 >= 10) clock[p++] = '0' + h12 / 10;
    clock[p++] = '0' + h12 % 10; clock[p++] = ':';
    clock[p++] = '0' + mv / 10; clock[p++] = '0' + mv % 10;
    clock[p++] = ' '; clock[p++] = ampm[0]; clock[p++] = ampm[1]; clock[p] = 0;
    int cw = font_string_width(clock);
    font_draw_string(clock, ((int)window_width() - cw) / 2, 14, 0x001C1C1E, -1);
    if (weather_text[0]) {
        int wl = font_string_width(weather_text);
        font_draw_string(weather_text, (int)window_width() - wl - 14, 14, 0x001C1C1E, -1);
    }
}

/* 4 columns, large retina-crisp tiles, labels under them, iOS-shaped
   spacing -- same gui_draw_one_icon_on the desktop's own Apps-folder grid
   and dock already render icons with, just laid out for a 430px-wide
   phone instead of an 848px desktop panel. Real apps only (0..
   GUI_APPS_FOLDER-1): the Apps-folder and Trash tiles are dock-only meta
   entries with no meaning on a screen that already shows every app. */
static void phone_home_grid_geom(int *x0, int *y0, int *cell_w, int *cell_h, int *tile){
    *cell_w = (int)window_width() / PHONE_HOME_COLS;
    /* v1.8.0: 4 columns put GUI_APPS_FOLDER's 26 real apps at 7 rows, and
       row 6 (Activity, Clock) silently never drew -- Joshua's screenshot
       review caught the two icons missing off the bottom, unreachable by
       any tap. 5 columns instead makes it 6 rows (26/5 rounded up), which
       clears the 760px phone screen with real margin to spare (last row's
       label bottom lands well above 760 -- see the comment on the row-fit
       constant below), not just barely inside it, so this isn't another
       boundary case waiting to reproduce the same bug from a slightly
       taller status bar or a future 27th app. 48px tiles at 86px-wide
       cells still read as real retina-crisp icons (close to the desktop
       dock's own tile size), not shrunk-to-fit thumbnails. */
    *cell_h = 96;
    *tile = 48;
    *x0 = 0;
    *y0 = PHONE_STATUS_H + 12;
}
static void phone_home_draw_grid(void){
    int x0, y0, cell_w, cell_h, tile;
    phone_home_grid_geom(&x0, &y0, &cell_w, &cell_h, &tile);
    for (int i = 0; i < GUI_APPS_FOLDER; i++) {
        int row = i / PHONE_HOME_COLS, col = i % PHONE_HOME_COLS;
        int cx = x0 + col * cell_w + cell_w / 2;
        int cy = y0 + row * cell_h + tile;
        if (cy + 24 > (int)window_height()) break; /* off the bottom of a real phone's 760px: nothing to scroll to yet, every app still reachable at 4 cols x 6+ rows */
        gui_draw_one_icon_on(i, cx, cy, tile, GUI_BG);
        int lw = font_string_width(APPS[i].name);
        font_draw_string(APPS[i].name, cx - lw / 2, cy + 10, 0x001C1C1E, -1);
    }
}
static void phone_home_full_repaint(void){
    window_clear(GUI_BG);
    phone_status_bar_draw();
    phone_home_draw_grid();
    serial_puts("phonehomerepaint\n"); /* discriminating marker for phone-boot-check.py */
    mouse_click_edge_sync();
}

/* Home screen's own loop: draw, wait for a tap, hit-test the grid, open
   the app full screen through gui_apps_launch (which already owns the
   whole "full screen, Esc or a tap anywhere closes, back to caller"
   contract for both in-kernel and ring-3 apps), then repaint the grid.
   Never returns -- boots straight here after Samantha, in place of the
   desktop's own dock loop, so desktop mode's gui_run below is untouched.

   Mouse tracked by hand here (mouse_get_delta/mouse_get_absolute/
   mouse_click_edge, the same three primitives gui_app_mouse_tick itself
   is built on) rather than through app_cursor_x: that global only moves
   inside gui_app_mouse_tick, which no-ops while gui_app_windowed is 0 --
   exactly the state a not-yet-opened home screen is in, on purpose, so
   that gui_apps_launch(hit) below still takes its real full-screen-open
   branch (the one that draws a fresh titlebar and sets up the viewport)
   instead of the nested-inside-an-open-window branch. */
static void phone_home_run(void){
    gui_app_windowed = 0;
    int mx = (int)window_width() / 2, my = (int)window_height() / 2;
    for (;;) {
        /* v1.8.0: the desktop menu bar's own weather fetch (same
           weather_fetch(), same weather_text the status bar reads) lives
           inside gui_run's dock loop below, which phone mode never
           reaches -- phone_status_bar_draw was reading weather_text but
           nothing here ever populated it, so it stayed permanently blank
           on a real network boot, not just headless. Same "once, then
           every ten minutes" gate as the desktop, so a tap-heavy home
           screen doesn't refetch on every repaint. */
        if (!weather_tried_once || ticks() - weather_last_tick > 100 * 600) {
            weather_tried_once = 1;
            weather_fetch();
        }
        phone_home_full_repaint();
        window_present(); sleep_ticks(5);
        mouse_click_edge_sync();
        int clicked = 0;
        for (;;) {
            int dx = 0, dy = 0, buttons = 0;
            mouse_get_delta(&dx, &dy, &buttons);
            mx += dx; my += dy;
            mouse_get_absolute(&mx, &my, (int)window_width(), (int)window_height());
            if (mx < 0) mx = 0; if (my < 0) my = 0;
            if (mx >= (int)window_width()) mx = (int)window_width() - 1;
            if (my >= (int)window_height()) my = (int)window_height() - 1;
            if (mouse_click_edge()) { clicked = 1; break; }
            window_present(); __asm__ volatile ("hlt");
        }
        if (!clicked) continue; /* no keyboard on a phone; a tap is the only real input the home screen itself needs */
        int x0, y0, cell_w, cell_h, tile;
        phone_home_grid_geom(&x0, &y0, &cell_w, &cell_h, &tile);
        int hit = -1;
        for (int i = 0; i < GUI_APPS_FOLDER; i++) {
            int row = i / PHONE_HOME_COLS, col = i % PHONE_HOME_COLS;
            int cx = x0 + col * cell_w + cell_w / 2;
            int cy = y0 + row * cell_h + tile;
            if (cy + 24 > (int)window_height()) break;
            int cell_x0 = cx - cell_w / 2, cell_x1 = cell_x0 + cell_w;
            int cell_y0 = cy - tile - 6, cell_y1 = cy + 24;
            if (mx >= cell_x0 && mx < cell_x1 && my >= cell_y0 && my < cell_y1) { hit = i; break; }
        }
        if (hit >= 0) { app_cursor_x = mx; app_cursor_y = my; gui_apps_launch(hit); } /* returns here on Esc or a tap anywhere in the app: back to the grid */
    }
}
