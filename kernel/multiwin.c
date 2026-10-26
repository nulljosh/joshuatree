/* The desktop's window manager: up to GUI_MULTIWIN_MAX windows back to front, opening, closing, focus, drag-snapping
 to halves and quarters, the Cmd+Tab switcher, and screenshots. Moved out of kernel.c in 4.19.1. */
#include "libc.h"
#include "console.h"
#include "serial.h"
#include "kheap.h"
#include "irq.h"
#include "vfs.h"
#include "window.h"
#include "font.h"
#include "app.h"
#include "gui_prims.h"
#include "gui_paint.h"
#include "dock_geom.h"
#include "memmap.h"
#include "desktop.h"
#include "multiwin.h"

/* v0.73.0: phase 1 of real multi-window, per ROADMAP.md's "Multi-window,
   honestly scoped" entry (grep for it: real windows were estimated as
   "3-5 sessions of unstarted work" before this pass, and confirmed still
   unstarted immediately before this one, every app a blocking function and
   window_open() called exactly once at boot). This is real, not cosmetic:
   a genuine window list, and two windows genuinely open and drawn on
   screen at the same time, each redrawn from its own real state on every
   repaint, neither frozen nor a fake snapshot.

   Phase 2 (click-to-focus, z-order hit testing) landed in v0.73.6; the
   2-window cap (GUI_MULTIWIN_MAX) is the one scoping choice still here. */
/* 1.9.23: ring-3 apps that open as compositor windows (ring3app_launch_window)
   instead of the blocking viewport. Reminders is the proof; one row moves another. */
int ring3app_launch_window(const char *name, unsigned int w, unsigned int h);
void ring3app_window_reaped(int task, int status);
void ring3app_window_blit(int task, int vw, int vh); int ring3app_is_windowable(const char *name);
int gui_ring3_windowed(int icon){ return icon >= 0 && icon < GUI_APP_COUNT && ring3app_is_windowable(APPS[icon].name); } /* every RING3_APPS row; a full window table or failed launch falls back to the blocking path */
gui_window_t gui_windows[GUI_MULTIWIN_MAX];
int gui_window_count = 0; /* gui_windows[0..gui_window_count-1] are the real open windows, back-to-front */

int gui_multiwin_supported(int icon){ return icon >= 0 && icon < GUI_APP_COUNT && (APPS[icon].draw || gui_ring3_windowed(icon)); }
int gui_multiwin_dock_ok(int icon){ return gui_multiwin_supported(icon); } /* apps with a draw hook or a ring-3 window */

/* v0.75.0 (batch 2): Mail has real per-keystroke interaction (its list,
   read and compose modes), unlike Files/Weather's static viewers; Reminders
   (1.9.9) and Calendar (1.9.12) left this set when they became ring-3
   programs. gui_run's input loop below only ever forwards a keystroke to
   the app whose window is currently topmost/focused (the same "topmost
   owns input" rule click-to-focus already established for clicks). */
int gui_multiwin_interactive(int icon){ return icon >= 0 && icon < GUI_APP_COUNT && (APPS[icon].key || gui_ring3_windowed(icon)); } /* apps with a key hook; Files for its 1/2 view-switch keys, Weather for R-to-retry */

/* Window 0 keeps the exact single-window rect the existing dock-app tests
   already assert against (gui_launch_from_dock's own x=70,y=40,w=820,h=385;
   appclose-check.py/app-interact-check.py hard-code CLOSE_X,CLOSE_Y=94,56,
   which is this same rect's close-circle centre, x+24,y+16). A second,
   concurrently-open window is offset so both titlebars and both close
   buttons stay fully on screen and visually distinct, not stacked exactly
   on top of each other. */
int gui_bleed_fits(void){ return (unsigned)window_width() * (unsigned)window_height() * 4u <= JT_USER_FB_BYTES; } /* a ring-3 window buffer is 2.1 MB (960x540): a bigger screen gets her in an ordinary window instead of a refused open */
int gui_window_bleed(int icon){ return !jt_phone_mode() && gui_bleed_fits() && icon >= 0 && icon < GUI_APP_COUNT && APPS[icon].open == samantha_ring3_open; } /* Samantha's window is her face, full bleed (portfolio mode: under the menu bar too); Esc closes it */
int gui_blocking_icon = -1;   /* 4.26.0: the app a blocking dock launch is running (kernel.c gui_launch_from_dock), -1 none */
int gui_app_open(int icon){   /* 4.26.0: has this app a window open? the dock's running-app dot */
    if (icon == gui_blocking_icon) return 1;
    for (int i = 0; i < gui_window_count; i++) if (gui_windows[i].icon == icon) return 1;
    return 0;
}
int gui_bleed_open(void){ for (int i = 0; i < gui_window_count; i++) if (gui_window_bleed(gui_windows[i].icon)) return 1; return 0; }
void gui_multiwin_geom(int slot_index, int *x, int *y, int *w, int *h){
    if (jt_phone_mode()) { *x = -8; *y = 8; *w = (int)window_width() + 16; *h = (int)window_height(); return; } /* 2.0 gate 5: one window, full screen under the back chevron strip (content rect 0,40,W,H-40) */
    if (slot_index == 0) { *x = 70; *y = 40; *w = 820; *h = 385; }
    else { *x = 70 + 60; *y = 40 + 60; *w = 820; *h = 385; }
    gui_clamp_win_rect(x, y, w, h); /* Mail/Calendar/Reminders/Files/Weather open here, not gui_launch_from_dock -- phone screens need the same clamp */
    if (*y + *h > gui_dock_band_top()) *h = gui_dock_band_top() - *y;   /* the second window stops above the dock band, as a drag does (appview.c): a hover label repaints that band from the wallpaper and would cut into it */
}

/* Magnet-style window snapping (title-bar drag to an edge/corner). All five
   multi-window apps draw their content through window_set_viewport(x+8,
   y+32, w-16, h-40) and lay it out with window_width()/window_height(),
   not hard-coded 820x385 numbers, so any w/h this hands them is real, not
   clipped or scaled after the fact.

   Zones, checked corners-first so a near-corner drag never mistakenly
   reads as a plain edge: 0=left half, 1=right half, 2..5=quarters (TL,
   TR, BL, BR), 6=full (top edge, like Magnet's maximize), -1=no zone. The
   playable area is the desktop strip between the menu bar and the dock,
   the same area gui_multiwin_geom's own windows already live inside. */
int gui_snap_area(int *top, int *bottom){
    *top = GUI_MENUBAR_H;
    *bottom = gui_dock_y0() - 10;
    return (int)window_width();
}
int gui_snap_zone(int mx, int my){
    int top, bottom; int w = gui_snap_area(&top, &bottom);
    const int corner = 40, edge = 12;
    if (mx <= corner && my <= top + corner) return 2;
    if (mx >= w - corner && my <= top + corner) return 3;
    if (mx <= corner && my >= bottom - corner) return 4;
    if (mx >= w - corner && my >= bottom - corner) return 5;
    if (my <= top + edge) return 6;
    if (mx <= edge) return 0;
    if (mx >= w - edge) return 1;
    return -1;
}
void gui_snap_target(int zone, int *x, int *y, int *w, int *h){
    int top, bottom; int sw = gui_snap_area(&top, &bottom);
    int areaH = bottom - top;
    switch (zone) {
        case 0: *x = 0;      *y = top;             *w = sw / 2;      *h = areaH; break;
        case 1: *x = sw / 2; *y = top;             *w = sw - sw / 2; *h = areaH; break;
        case 2: *x = 0;      *y = top;             *w = sw / 2;      *h = areaH / 2; break;
        case 3: *x = sw / 2; *y = top;             *w = sw - sw / 2; *h = areaH / 2; break;
        case 4: *x = 0;      *y = top + areaH / 2; *w = sw / 2;      *h = areaH - areaH / 2; break;
        case 5: *x = sw / 2; *y = top + areaH / 2; *w = sw - sw / 2; *h = areaH - areaH / 2; break;
        default:*x = 0;      *y = top;             *w = sw;          *h = areaH; break; /* 6: full */
    }
}
/* One-pixel border, drawn straight onto the framebuffer, never onto a
   window (there's nothing under it to preserve while dragging: the
   dragged window itself isn't moved live, only this preview outline is
   drawn, see gui_run's drag_win handling). */
void gui_snap_outline(int zone){
    if (zone < 0) return;
    int x, y, w, h; gui_snap_target(zone, &x, &y, &w, &h);
    unsigned int c = 0x00307FE2;
    window_rect(x, y, w, 1, c);
    window_rect(x, y + h - 1, w, 1, c);
    window_rect(x, y, 1, h, c);
    window_rect(x + w - 1, y, 1, h, c);
}

/* v0.76.18: chrome and content split so a keystroke repaints only the
   content viewport, never the whole alpha-blended frame (mwkeyflash-check.sh). */
void gui_multiwin_draw_chrome(const gui_window_t *win){
    if (gui_window_bleed(win->icon)) return; /* no frame, no traffic lights: his face is the screen */
    serial_puts("mwchrome\n"); /* discriminating marker for tools/checks/mwkeyflash-check.sh */
    int x = win->x, y = win->y, w = win->w, h = win->h;
    gui_draw_window_frame(x, y, w, h, APPS[win->icon].name);
}
void gui_multiwin_draw_content_only(const gui_window_t *win){
    int x = win->x, y = win->y, w = win->w, h = win->h;
    window_set_viewport(x + 8, y + 32, (unsigned int)(w - 16), (unsigned int)(h - 40));
    /* v0.76.19: gui_draw_app_titlebar skips its own traffic lights only
       under this flag; without it every content redraw drew a second set. */
    gui_app_windowed = 1;
    /* Real per-repaint content, not a cached bitmap: each call re-derives
       the window's content from the same live state its single-window
       counterpart reads (vfs_list for Files, weather_text for Weather),
       so a second window opening never leaves the first one's content
       stale or frozen. */
    if (win->task >= 0) {
        /* 1.9.23: a ring-3 window: blit its private framebuffer through
           window_pixel, the same clipped primitive every in-kernel app
           draws with, at this window's position. The program never
           touches the screen. */
        ring3app_window_blit(win->task, w - 16, h - 40);
    } else if (gui_multiwin_supported(win->icon)) APPS[win->icon].draw();
    gui_app_windowed = 0;
    window_clear_viewport();
}
void gui_multiwin_draw_one(const gui_window_t *win){
    gui_multiwin_draw_chrome(win);
    gui_multiwin_draw_content_only(win);
}


/* Called from gui_run's own full-repaint branch, right alongside the
   menu/notif/weather overlay draws it already does there, so every open
   window is genuinely redrawn on top of the desktop on every real repaint
   this kernel does, back-to-front, list order. */
void gui_multiwin_draw_all(void){
    for (int i = 0; i < gui_window_count; i++) gui_multiwin_draw_one(&gui_windows[i]);
}

/* v0.73.6 (phase 2): the one real choke point that moves a window to the
   top of the z-order list. Both gui_multiwin_open's "already open, refocus
   it" path and the new click-to-focus path below now go through this
   instead of each keeping its own copy of the same shift loop, so draw
   order (gui_multiwin_draw_all, back-to-front over the list) and input
   hit-testing (gui_multiwin_hit_test, topmost-first over the same list)
   can never disagree about which window is "on top": there is exactly one
   piece of z-order state, gui_windows[0..count-1]'s own order. */
void gui_multiwin_focus(int idx){
    if (idx < 0 || idx >= gui_window_count || idx == gui_window_count - 1) return;
    gui_window_t tmp = gui_windows[idx];
    for (int j = idx; j < gui_window_count - 1; j++) gui_windows[j] = gui_windows[j + 1];
    gui_windows[gui_window_count - 1] = tmp;
}

/* App switcher: Alt+Tab (Ctrl+Tab too, since QEMU/v86's Alt delivery to a
   guest kernel is the flaky one to bet on) cycles the open windows, the
   same macOS Cmd+Tab shape -- hold the modifier and tap Tab, a small
   centered panel lists every open window with the next one highlighted;
   releasing the modifier focuses it. Walks whatever's really open
   (gui_window_count, capped at GUI_MULTIWIN_MAX like the window list
   itself already is), never a hard-coded count. */
int gui_switcher_open = 0;
int gui_switcher_idx = 0;

void gui_switcher_draw(int hi){
    int rows = gui_window_count;
    if (rows <= 0) return;
    int row_h = 28, pad_v = 10, w = 220;
    int h = pad_v * 2 + rows * row_h;
    int x = ((int)window_width() - w) / 2, y = ((int)window_height() - h) / 2;
    unsigned int bg = 0x002C2C2E, text = 0x00F5F5F7; /* same flyout colors the Apple menu/notif/weather panels already share */
    gui_rounded_rect_on_wallpaper(x, y, w, h, bg, 14);
    int ry = y + pad_v;
    for (int i = 0; i < rows; i++) {
        if (i == hi) window_rect(x + 6, ry, w - 12, row_h - 4, 0x00555555);
        int icon = gui_windows[i].icon;
        const char *label = (icon >= 0 && icon < GUI_APP_COUNT) ? APPS[icon].name : "?";
        font_draw_string(label, x + 18, ry + 6, text, -1);
        ry += row_h;
    }
    window_present();
}

/* Screenshot: Ctrl+Shift+3, the reliable one -- real PrintScreen sends an
   E0-prefixed 4-byte make sequence (E0 2A E0 37) that shares its 0xE0
   lead byte with every arrow key's own extended sequence, which the
   multiwin key readers just above are already mid-decoding whenever a
   window with arrow-key input (Files, Calendar) is open; peeking for it
   here risked eating a real arrow keystroke, so this ships the one
   hotkey that can't collide. Saves the live framebuffer as an
   uncompressed 24-bit BMP (no PNG encoder exists in this tree, drivers/
   png.c only decodes) named SHOT0001.BMP, SHOT0002.BMP, ... in the files
   root, so it shows up in Files like any other saved file. */
void gui_write_u32le(unsigned char *p, unsigned int v){ p[0]=v&0xFF; p[1]=(v>>8)&0xFF; p[2]=(v>>16)&0xFF; p[3]=(v>>24)&0xFF; }
void gui_write_u16le(unsigned char *p, unsigned int v){ p[0]=v&0xFF; p[1]=(v>>8)&0xFF; }

int gui_screenshot_next_name(char *out /* 13 bytes, "SHOTNNNN.BMP\0" */){
    for (int n = 1; n <= 9999; n++) {
        char name[13];
        int i = 0; name[i++]='S'; name[i++]='H'; name[i++]='O'; name[i++]='T';
        name[i++]='0'+(n/1000)%10; name[i++]='0'+(n/100)%10; name[i++]='0'+(n/10)%10; name[i++]='0'+n%10;
        name[i++]='.'; name[i++]='B'; name[i++]='M'; name[i++]='P'; name[i]=0;
        unsigned char probe[1];
        if (vfs_read_file(name, probe, 1) < 0) { for (int j = 0; j <= i; j++) out[j] = name[j]; return 1; }
    }
    return 0;
}

/* Returns the real byte count written (54-byte header + padded pixel
   rows) on success, 0 on failure -- a headless check can grep the exact
   figure straight out of the serial marker below instead of having to
   parse the FAT image itself to prove the file's real size. */
unsigned int gui_screenshot_save(char *name_out /* 13 bytes */){
    int sc = (int)window_scale(); if (sc < 1) sc = 1;
    int w = (int)window_width() * sc, h = (int)window_height() * sc;
    if (w <= 0 || h <= 0) return 0;
    unsigned int row_bytes = (unsigned int)w * 3;
    unsigned int pad = (4 - (row_bytes % 4)) % 4;
    unsigned int data_size = (row_bytes + pad) * (unsigned int)h;
    unsigned int file_size = 54 + data_size;
    unsigned char *buf = (unsigned char *)kmalloc(file_size);
    if (!buf) return 0;
    buf[0]='B'; buf[1]='M';
    gui_write_u32le(buf + 2, file_size);
    gui_write_u32le(buf + 6, 0);
    gui_write_u32le(buf + 10, 54);
    gui_write_u32le(buf + 14, 40);
    gui_write_u32le(buf + 18, (unsigned int)w);
    gui_write_u32le(buf + 22, (unsigned int)h); /* positive height: bottom-up rows, standard BMP */
    gui_write_u16le(buf + 26, 1);
    gui_write_u16le(buf + 28, 24);
    gui_write_u32le(buf + 30, 0);
    gui_write_u32le(buf + 34, data_size);
    gui_write_u32le(buf + 38, 2835);
    gui_write_u32le(buf + 42, 2835);
    gui_write_u32le(buf + 46, 0);
    gui_write_u32le(buf + 50, 0);
    unsigned char *px = buf + 54;
    for (int y = 0; y < h; y++) {
        int src_y = h - 1 - y; /* bottom-up */
        unsigned char *row = px + (unsigned int)y * (row_bytes + pad);
        for (int x = 0; x < w; x++) {
            unsigned int c = window_get_pixel_phys(x, src_y);
            row[x*3+0] = (unsigned char)(c & 0xFF);
            row[x*3+1] = (unsigned char)((c >> 8) & 0xFF);
            row[x*3+2] = (unsigned char)((c >> 16) & 0xFF);
        }
        for (unsigned int p = 0; p < pad; p++) row[row_bytes + p] = 0;
    }
    char name[13];
    int ok = gui_screenshot_next_name(name);
    int saved = ok && vfs_write_file(name, buf, file_size) == 1; /* vfs_write_file (drivers/vfs.h), not fat_write_file directly, so this lands in whichever backend is actually active -- the FAT disk when one's attached, the RAM fallback (ramfs.c) in every headless/no-disk boot -- the same choice Files/Notes already make instead of hard-wiring FAT. Returns exactly 1 on success, 0 on failure, never a byte count. */
    kfree(buf);
    if (saved) { for (int j = 0; j < 13; j++) name_out[j] = name[j]; }
    return saved ? file_size : 0;
}

/* A brief centered confirmation banner, same flyout colors as the
   switcher panel above, up for ~0.6s (60 PIT ticks) then erased by the
   next real desktop repaint -- the same "flash" every other save
   confirmation in this kernel (editor.h's status line) already does,
   just as its own overlay instead of a status line, since the desktop
   itself has none. */
void gui_screenshot_flash(const char *name){
    char msg[24]; int i = 0; const char *s = "Saved "; while (*s) msg[i++] = *s++;
    s = name; while (*s && i < 23) msg[i++] = *s++;
    msg[i] = 0;
    int tw = font_string_width(msg);
    int w = tw + 40, h = 40;
    int x = ((int)window_width() - w) / 2, y = 70;
    gui_rounded_rect_on_wallpaper(x, y, w, h, 0x002C2C2E, 14);
    font_draw_string(msg, x + 20, y + 12, 0x00F5F5F7, -1);
    window_present();
    unsigned int until = ticks() + 60;
    while ((int)(ticks() - until) < 0) __asm__ volatile ("hlt");
}

int gui_multiwin_open(int icon){
    for (int i = 0; i < gui_window_count; i++) {
        if (gui_windows[i].icon == icon) {
            /* Already open: focus it (move to the end of the list, so the
               back-to-front draw puts it on top) instead of opening a
               duplicate. */
            gui_multiwin_focus(i);
            return gui_window_count - 1;
        }
    }
    if (gui_window_count >= GUI_MULTIWIN_MAX) return -1; /* the real cap this pass proves, see the comment above */
    int slot = gui_window_count;
    gui_windows[slot].icon = icon;
    gui_windows[slot].task = -1;
    gui_windows[slot].shown = 0;
    gui_multiwin_geom(slot, &gui_windows[slot].x, &gui_windows[slot].y, &gui_windows[slot].w, &gui_windows[slot].h);
    if (gui_window_bleed(icon)) { gui_windows[slot].x = -8; gui_windows[slot].y = -32; gui_windows[slot].w = (int)window_width() + 16; gui_windows[slot].h = (int)window_height() + 40; } /* content rect = the whole screen */
    if (gui_ring3_windowed(icon)) {
        /* 1.9.23: the program is scheduled now and draws into its own
           buffer; this loop keeps running. -1 is refused by the caller
           (gui_refuse_open), never a blocking takeover. */
        if (APPS[icon].open == notes_ring3_open) notes_migrate_legacy(); /* the window path never calls APPS[].open, which is where the one-time NOTES.TXT move into the default folder lives (editor.h) */
        int t = ring3app_launch_window(APPS[icon].name, (unsigned int)(gui_windows[slot].w - 16), (unsigned int)(gui_windows[slot].h - 40));
        if (t < 0) return -1;
        gui_windows[slot].task = t;
    }
    gui_window_count++;
    return slot;
}

/* v0.73.6 (phase 2): real click-to-focus hit-testing. Checks every open
   window, topmost-drawn first (gui_windows[count-1] down to [0], the exact
   reverse of gui_multiwin_draw_all's back-to-front draw order), and returns
   the first (i.e. topmost) whose rect contains the click -- the same
   "topmost visible thing wins" rule every real windowing system's hit
   test uses (X11 stacking order, Win32 Z-order, etc). Replaces the old
   phase-1 gui_multiwin_focused_click_hit, which only ever tested the
   single most-recently-opened window and left a click on a visible
   background window doing nothing. -1 means the click hit neither
   window's rect at all. */
int gui_multiwin_hit_test(int mx, int my){
    for (int i = gui_window_count - 1; i >= 0; i--) {
        const gui_window_t *w = &gui_windows[i];
        if (mx >= w->x && mx < w->x + w->w && my >= w->y && my < w->y + w->h) return i;
    }
    return -1;
}

void gui_multiwin_close(int idx){
    if (idx < 0 || idx >= gui_window_count) return;
    int bleed = gui_window_bleed(gui_windows[idx].icon); for (int j = idx; j < gui_window_count - 1; j++) gui_windows[j] = gui_windows[j + 1]; gui_window_count--;
    if (bleed) { gui_menubar_force_redraw(); gui_draw_menubar(); } /* the bar was held back while his face covered it */
}

/* v0.75.0 (batch 2): the same SC[]/extended-0xE0 decode get_key_or_click
   already does, but never hlt-waits -- called at most once per gui_run
   frame, for the one focused interactive window (gui_multiwin_interactive),
   so a real keystroke reaches Mail/Calendar/Reminders' own on_key handler
   without blocking the compositor the way the old gui_wait_close-shaped
   loops did. A truly split extended sequence (the second byte of an arrow
   key) just drops this frame rather than block waiting for it -- the IRQ
   handler fills both bytes of the ring within microseconds of each other,
   well inside one frame at this poll rate, so in practice this never
   drops a real arrow keystroke. */
int gui_multiwin_key_nonblock(void){
    int sc = kbd_pop();
    if (sc < 0) return -1;
    /* 2.0.0: the selection keys (Shift+arrow, Ctrl+A) go to a ring-3 window only; an in-kernel app keeps plain arrows */
    int sel_ok = gui_window_count > 0 && gui_windows[gui_window_count - 1].task >= 0;
    if (sc == 0xE0) {
        int sc2 = kbd_pop();
        if (sc2 < 0) return -1; int sh = sel_ok && kbd_shift; /* shift+arrow extends a selection only in a ring-3 window */
        if (sc2 == 0x48) return sh ? KEY_SUP : KEY_UP;    if (sc2 == 0x50) return sh ? KEY_SDOWN : KEY_DOWN;
        if (sel_ok && kbd_ctrl && gui_windows[gui_window_count - 1].icon == GUI_APP_PANES) { if (sc2 == 0x4B) return KEY_CTL_LEFT; if (sc2 == 0x4D) return KEY_CTL_RIGHT; }
        if (sc2 == 0x4B) return sh ? KEY_SLEFT : KEY_LEFT; if (sc2 == 0x4D) return sh ? KEY_SRIGHT : KEY_RIGHT;
        if (sc2 == 0x47) return KEY_HOME;
        if (sc2 == 0x4F) return KEY_END;
        if (sc2 == 0x53) return KEY_DELETE;
        return -1;
    }
    if (sc == 0xBC) return KEY_F2_UP;
    if (sc & 0x80) return -1; /* key release */
    if (sc == 0x3C) return KEY_F2;
    if (kbd_ctrl && (sc & 0x7F) == 0x1F) return KEY_SAVE;
    if (kbd_ctrl && (sc & 0x7F) == 0x31) return KEY_NEW;
    if (kbd_ctrl) { int k = sc & 0x7F; if (k == 0x2E) return KEY_COPY; if (k == 0x2D) return KEY_CUT; if (k == 0x2F) return KEY_PASTE; if (sel_ok && k == 0x1E) return KEY_SELALL; if (sel_ok && gui_windows[gui_window_count - 1].icon == GUI_APP_PANES && key_ctl_code(k)) return key_ctl_code(k); } /* clipboard and select-all keys reach ring-3 windows too */
    char c = kbd_map(sc);
    if (c == '\n') return KEY_ENTER;
    if (c == 27)   return KEY_ESC;
    if (c) return (int)(unsigned char)c;
    return -1;
}
