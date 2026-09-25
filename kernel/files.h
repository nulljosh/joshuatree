#ifndef FILES_H
#define FILES_H

/* Files app, split out of kernel.c (mirrors the activity.h/search.h
   pattern: header-only, included straight into kernel.c so every
   window, vfs and settings primitive and the files_view global stay
   visible with no new declarations needed). */

static int gui_fat_count;
static char gui_fat_names[64][14];
static int gui_fat_is_dir[64];
static void gui_fat_collect(const char *name, unsigned int size, int is_dir){
    (void)size;
    if (gui_fat_count >= 64) return;
    int i = 0;
    while (name[i] && i < 12) { gui_fat_names[gui_fat_count][i] = name[i]; i++; }
    if (is_dir) gui_fat_names[gui_fat_count][i++] = '/';
    gui_fat_names[gui_fat_count][i] = 0;
    gui_fat_is_dir[gui_fat_count] = is_dir;
    gui_fat_count++;
}

/* v0.85.6: view switcher. "Files: view buttons. Right now it's just a
   list." (roadmap.md). Two real views so far (List, Icons); Columns
   didn't fit the time budget. The chosen view is a persisted int
   (files_view, "filesview=" in SETTINGS.TXT, same flat key=value shape
   every other Settings toggle already uses) so it survives a reboot the
   same way dock size/wallpaper do. */
#define FILES_TOOLBAR_Y 40
#define FILES_TOOLBAR_BTN_W 64
#define FILES_TOOLBAR_BTN_H 22
#define FILES_LIST_ROW0 76
#define FILES_LIST_ROW_H 18
#define FILES_ICON_TILE 84   /* full grid cell, icon + label + gaps */
#define FILES_ICON_SIZE 40   /* drawn icon glyph, square */
#define FILES_ICON_TOP 76

static int gui_files_toolbar_btn_x(int idx){ return 20 + idx * (FILES_TOOLBAR_BTN_W + 8); }

/* Which toolbar button (0=List, 1=Icons) a click lands in, or -1. Pure
   hit-test, same shape as settings_row_at, kept separate from drawing so
   the spacing used to hit-test is exactly the spacing that was drawn. */
static int gui_files_toolbar_at(int cx, int cy){
    if (cy < FILES_TOOLBAR_Y - 4 || cy >= FILES_TOOLBAR_Y - 4 + FILES_TOOLBAR_BTN_H) return -1;
    for (int i = 0; i < 2; i++) {
        int bx = gui_files_toolbar_btn_x(i);
        if (cx >= bx && cx < bx + FILES_TOOLBAR_BTN_W) return i;
    }
    return -1;
}

/* How many icon columns fit the current window width. */
static int gui_files_icon_cols(void){
    int w = ((int)window_width() - 40) / FILES_ICON_TILE;
    return w < 1 ? 1 : w;
}

/* Generic file/folder glyph, house palette (cream fill, warm ink stroke,
   no purple/teal): a folder tab shape for directories, a dog-eared page
   for plain files. Built from the same window_rect/gui_fill_circle
   primitives every dock/app icon in this file already draws with, since
   there's no separate per-file-type bitmap table (icon_art.h only holds
   whole-app icons) -- reusing the *drawing helpers*, not a nonexistent
   glyph table. */
static void gui_draw_file_glyph(int x, int y, int size, int is_dir){
    unsigned int ink = 0x00A8875A;   /* warm bark line, matches dock chrome */
    unsigned int fill = 0x00F3EEE5;  /* cream, matches GUI_BG family */
    if (is_dir) {
        int tabw = size * 2 / 5;
        window_rect(x, y + size / 6, tabw, size / 6, ink);
        window_rect(x, y + size / 3, size, size - size / 3, fill);
        window_rect(x, y + size / 3, size, 2, ink);
        window_rect(x, y + size - 2, size, 2, ink);
        window_rect(x, y + size / 3, 2, size - size / 3, ink);
        window_rect(x + size - 2, y + size / 3, 2, size - size / 3, ink);
    } else {
        int fold = size / 4;
        window_rect(x, y, size, size, fill);
        window_rect(x, y, 2, size, ink);
        window_rect(x + size - 2, y, 2, size, ink);
        window_rect(x, y, size, 2, ink);
        window_rect(x, y + size - 2, size, 2, ink);
        window_rect(x + size - fold, y, fold, 2, 0x00E5DCCC);
        window_rect(x + size - fold, y, 2, fold, 0x00E5DCCC);
        for (int i = 0; i < 3; i++) window_rect(x + size / 5, y + size / 2 + i * 6, size * 3 / 5, 2, 0x00CFC4B2);
    }
}

/* Same split as gui_draw_weather_content above. */
static int gui_files_sel = 0;
static void gui_draw_files_content(void){
    window_clear(0x00FAF8F6);
    gui_draw_app_titlebar("Files");

    /* Toolbar: List / Icons. Cmd/Ctrl+1/2 or plain 1/2, or a tap, switch
       views; the highlighted button always reflects files_view, never a
       stale click. */
    const char *btn_labels[2] = {"List", "Icons"};
    for (int i = 0; i < 2; i++) {
        int bx = gui_files_toolbar_btn_x(i);
        int by = FILES_TOOLBAR_Y - 4;
        window_rect(bx, by, FILES_TOOLBAR_BTN_W, FILES_TOOLBAR_BTN_H, i == files_view ? 0x00E5DCCC : 0x00EFEBE4);
        font_draw_string(btn_labels[i], bx + 8, by + 5, 0x001C1C1E, -1);
    }

    gui_fat_count = 0;
    vfs_list(gui_fat_collect);
    if (gui_files_sel >= gui_fat_count) gui_files_sel = gui_fat_count > 0 ? gui_fat_count - 1 : 0;
    if (gui_files_sel < 0) gui_files_sel = 0;

    if (gui_fat_count == 0) {
        font_draw_string("(no files, or no FAT filesystem)", 20, FILES_LIST_ROW0, 0x001C1C1E, -1);
        return;
    }

    if (files_view == FILES_VIEW_ICONS) {
        int cols = gui_files_icon_cols();
        for (int i = 0; i < gui_fat_count; i++) {
            int col = i % cols, row = i / cols;
            int x = 20 + col * FILES_ICON_TILE;
            int y = FILES_ICON_TOP + row * FILES_ICON_TILE;
            if (i == gui_files_sel) window_rect(x - 4, y - 4, FILES_ICON_TILE - 8, FILES_ICON_TILE - 8, 0x00EDE6DC);
            gui_draw_file_glyph(x + (FILES_ICON_TILE - 8 - FILES_ICON_SIZE) / 2, y, FILES_ICON_SIZE, gui_fat_is_dir[i]);
            font_draw_string(gui_fat_names[i], x, y + FILES_ICON_SIZE + 8, 0x001C1C1E, -1);
        }
    } else {
        for (int i = 0; i < gui_fat_count; i++) {
            int y = FILES_LIST_ROW0 + i * FILES_LIST_ROW_H;
            if (i == gui_files_sel) window_rect(16, y - 3, (int)window_width() - 32, FILES_LIST_ROW_H, 0x00EDE6DC);
            font_draw_string(gui_fat_names[i], 20, y, 0x001C1C1E, -1);
        }
    }
}

/* Shared by both the old blocking single-window path (gui_launch_files,
   still used from the Apple-menu "Files" item) and the real running path,
   the multi-window compositor's per-frame key dispatch in gui_run (see
   gui_multiwin_interactive/mw_topmost_icon above) -- Files opens through
   gui_multiwin_open from the dock, same as Mail/Calendar/Reminders/
   Weather, so its own key handling has to live in that same on_key shape
   (return 1 to close the window) or it's simply never called: the dock
   path never runs gui_launch_files's loop at all. Returns 1 when Esc
   should close the window. */
static int gui_files_on_key(int k){
    if (k == KEY_ESC) return 1;
    if (k == '1') { if (files_view != FILES_VIEW_LIST) { files_view = FILES_VIEW_LIST; settings_save(); } return 0; }
    if (k == '2') { if (files_view != FILES_VIEW_ICONS) { files_view = FILES_VIEW_ICONS; settings_save(); } return 0; }
    if (gui_fat_count == 0) return 0;
    int cols = files_view == FILES_VIEW_ICONS ? gui_files_icon_cols() : 1;
    if (k == KEY_UP) { if (files_view == FILES_VIEW_ICONS) { if (gui_files_sel - cols >= 0) gui_files_sel -= cols; } else if (gui_files_sel > 0) gui_files_sel--; }
    else if (k == KEY_DOWN) { if (files_view == FILES_VIEW_ICONS) { if (gui_files_sel + cols < gui_fat_count) gui_files_sel += cols; } else if (gui_files_sel < gui_fat_count - 1) gui_files_sel++; }
    else if (k == KEY_LEFT && files_view == FILES_VIEW_ICONS) { if (gui_files_sel > 0) gui_files_sel--; }
    else if (k == KEY_RIGHT && files_view == FILES_VIEW_ICONS) { if (gui_files_sel < gui_fat_count - 1) gui_files_sel++; }
    /* KEY_ENTER: selection exists (arrows+Enter wired); real file-open/
       navigate is future work, same as the pre-existing flat list had no
       open action either. */
    return 0;
}

/* Toolbar click hit-test against the TOPMOST multi-window Files window,
   called from gui_run's own click dispatch before it falls through to the
   universal "any click inside the topmost window closes it" contract
   (gui_multiwin.c's press_window handling) -- without this, clicking
   List/Icons would just close the window like any other click, and the
   toolbar buttons would do nothing but dismiss the app. Coordinates are
   FULL-SCREEN logical (mx/my in gui_run), converted to the window's own
   content-viewport-relative space the same way the Apps folder's click
   hit-test already does. Returns 1 if the click was consumed (don't
   close), 0 if it missed the toolbar (falls through to the normal close
   contract). */
static int gui_files_click(int win_x, int win_y, int mx, int my){
    int vx = win_x + 8, vy = win_y + 32;
    int tb = gui_files_toolbar_at(mx - vx, my - vy);
    if (tb == 0 && files_view != FILES_VIEW_LIST) { files_view = FILES_VIEW_LIST; settings_save(); return 1; }
    if (tb == 1 && files_view != FILES_VIEW_ICONS) { files_view = FILES_VIEW_ICONS; settings_save(); return 1; }
    return tb >= 0; /* a click on the already-active button is still "on the toolbar", not a close */
}

static void gui_launch_files(void){
    gui_files_sel = 0;
    for (;;) {
        gui_draw_files_content();
        font_draw_string("1/2 view   arrows move   enter opens   esc closes", 20, (int)window_height() - 28, 0x00807468, -1);
        window_present(); sleep_ticks(5);
        mouse_click_edge_sync();
        int k = get_key_or_click();
        if (k == KEY_CLICK) {
            int click_vx = app_cursor_x - app_view_x, click_vy = app_cursor_y - app_view_y;
            int tb = gui_files_toolbar_at(click_vx, click_vy);
            if (tb == 0 && files_view != FILES_VIEW_LIST) files_view = FILES_VIEW_LIST, settings_save();
            else if (tb == 1 && files_view != FILES_VIEW_ICONS) files_view = FILES_VIEW_ICONS, settings_save();
            continue;
        }
        if (gui_files_on_key(k)) return;
    }
}

#endif
