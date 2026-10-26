/* The i386 desktop's event loop, out of kernel.c since 4.23.9: gui_run (the pointer, the dock, the menu bar and
   its flyouts, keys and chords, the wind tick, the weather refresh, launching apps) and the two menu actions it
   owns, the lock screen and the Apple menu's items. i386 only; the Pi's loop is arch/arm64/boot.c. */
#include "gdt.h"
#include "idt.h"
#include "irq.h"
#include "pic.h"
#include "pmm.h"
#include "paging.h"
#include "kheap.h"
#include "picker.h"
#include "task.h"
#include "ring3.h"
#include "syscall.h"
#include "ata.h"
#include "fat.h"
#include "vfs.h"
#include "blockdev.h"
#include "ramdisk.h"
#include "trash.h"
#include "ramfs.h"
#include "exec.h"
#include "libc.h"
#include "pci.h"
#include "vbe.h"
#include "mouse.h"
#include "vmmouse.h"
#include "window.h"
#include "ttf.h"
#include "font.h"
#include "rtl8139.h"
#include "net.h"
#include "gui_prims.h"
#include "dock_geom.h"
#include "dock_draw.h"
#include "gui_paint.h"
#include "http.h"
#include "wallmap.h"
#include "gui_shapes.h"
#include "shell.h"
#include "selftest.h"
#include "desktop.h"
#include "console.h"
#include "multiwin.h"
#include "walltint.h"
#include "settings.h"
#include "serial.h"
#include "sb16.h"
#include "speak.h"
#include "app.h"
#include "png.h"
#include "jpeg.h"
#include "version.h"
#include "ring3app.h"
#include "weather.h"
#include "json.h"
#include "html.h"
#include "klog.h"
#include "hint.h"
#include "phone_home.h"
#include "wallpaint.h"
#include "menubar.h"
#include "icons.h"
#include "clockicon.h"
#include "appview.h"
#include "auth.h"
#include "uitext.h"
#include "apps_geom.h"
#include "appsfolder.h"
#include "settings_ui.h"
#include "stocks.h"
#include "bench.h"
#include "bootscreen.h"
#include "menu.h"


static void gui_lock_screen(void){
    if (!auth_gate_would_prompt()) {
        /* No accounts to lock with. Close menu, show brief message, return to desktop. */
        gui_draw_desktop(-1, -1, 0, 0);
        /* Display message for ~1 second: show a notification-style message on screen */
        int msg_w = font_string_width("No accounts to lock with");
        int msg_x = ((int)window_width() - msg_w) / 2;
        int msg_y = (int)window_height() / 2;
        int box_x = msg_x - 10, box_y = msg_y - 10, box_w = msg_w + 20, box_h = 25;
        window_rect(box_x, box_y, box_w, box_h, 0x00FAF8F6);  /* fill */
        /* v0.85.3: a second full-size window_rect in the border color used to
           sit directly on top of this fill (same x/y/w/h), painting the whole
           box grey and burying the grey message text on a now-identical grey
           background -- a real headless dump showed the box with no text at
           all. A 1px outline on all four edges reads as a border without
           erasing the fill underneath it. */
        window_rect(box_x, box_y, box_w, 1, 0x00555555);              /* top */
        window_rect(box_x, box_y + box_h - 1, box_w, 1, 0x00555555);  /* bottom */
        window_rect(box_x, box_y, 1, box_h, 0x00555555);              /* left */
        window_rect(box_x + box_w - 1, box_y, 1, box_h, 0x00555555);  /* right */
        font_draw_string("No accounts to lock with", msg_x, msg_y, 0x00555555, -1);
        window_present();
        sleep_ticks(100);  /* 1 second at 100 ticks/sec */
        gui_draw_boot_screen();
    } else {
        /* Account exists. Force the login screen even though already logged in. */
        serial_puts("auth: locked\n");
        auth_logged_in = 0;  /* Reset the flag to force re-authentication */
        auth_login_screen();
        gui_draw_boot_screen();
    }
}

static void gui_menu_run_item(int item){
    if (item == 0) gui_launch_about();
    else if (item == 1) gui_launch_from_dock(0);
    else if (item == 2) gui_launch_from_dock(3);
    else if (item == 3) gui_launch_settings();
    else if (item == 4) gui_lock_screen();
    else if (item == 6) reboot();
    else if (item == 7) {
        window_clear(0x00111111);
        font_draw_string("It's now safe to turn off this computer.", 20, (int)window_height() / 2, 0x00F5F5F7, -1);
        __asm__ volatile ("cli");
        for (;;) __asm__ volatile ("hlt");
    }
}
/* phone_home.h (included above): phone mode's real home screen, kernel/phone_home.c since 4.20.18 */
void gui_run(void){
    /* v42: 16:9, 960x540 logical at 2x = 1920x1080 physical, the native
       size of the monitor this actually runs fullscreen on. QEMU's cocoa
       zoom-to-fit stretches without preserving aspect, so a 4:3 mode on a
       16:9 panel came out visibly skewed; matching the panel's own shape
       means fullscreen is pixel-exact with no scaling at all. */
    /* "phone" boots a real portrait phone mode at scale 1 instead: Bochs
       VBE takes any size, and 430x760 is a real phone's own logical
       pixels, not the desktop's 960x540 shrunk to fit. */
    if (boot_to_phone) { if (!window_open_scaled(430, 760, 32, 2)) { puts("no VGA device found or out of page tables\n"); return; } }
    else if (!(boot_res_w && window_open_scaled(boot_res_w / 2, boot_res_h / 2, 32, 2)) && !window_open_scaled(960, 540, 32, 2)) { puts("no VGA device found or out of page tables\n"); return; } /* res= (gui_parse_res) first, else the default */
    font_set_aa(gui_aa_char, gui_aa_advance); /* v44: real typeface for every string from here on */
    font_set_aa_mono(gui_aa_char_mono); /* term-mono: mono face for the terminal grid and Keyrate's typed line */
    /* v46: no wind in the browser, decided up front rather than measured
       after the fact. The slow-frame gate still exists, but even the two
       frames it takes to trip blocked the kernel long enough that v86's
       PS/2 queue overflowed and the demo tour's paced cursor packets were
       dropped (tourtest failed twice, alone, on this build). The BIOS-font
       check from v38 is the reliable "this is v86" signal. */
    if (font_is_fallback()) wind_enabled = 0;
    /* One real check that the back buffer is both present and actually
       interposed, reported over serial rather than assumed; the
       "backbuffer=none" case is the honest, still-correct fallback a
       machine too small to allocate one (v86's 32MB demo) takes. */
    serial_puts(window_backbuffer_selftest() ? "backbuffer=ok\n"
                : (window_has_back_buffer() ? "backbuffer=broken\n" : "backbuffer=none\n"));
    auth_gate(); /* v0.77: real login screen, once per session, before the desktop ever paints */
    gui_draw_boot_screen();
    gui_order_init();
    if (boot_to_samantha) { boot_to_samantha = 0; serial_puts("samopen\n"); if (boot_to_phone) { gui_app_windowed = 0; phone_window_run(6); } else gui_launch_from_dock(6); } /* 1.9.26: ring-3 Samantha is the first screen; phone mode lands on the home grid when she closes */
    else if (!boot_to_phone) serial_puts("guidesktop\n"); /* discriminating marker for tools/checks/samantha-boot-check.py: the icon desktop drew first, samantha mode never reaches here before her avatar; phone mode never draws this desktop at all (see below), so it must not claim it did */
    if (boot_to_phone) { phone_home_run(); return; } /* v1.8.0: leaving Samantha lands on a real home screen, not the desktop's dock squeezed into 430px; never returns */
    dock_hover = dock_presented_hover = -1;
    int mx = 400, my = 300, buttons = 0, prev_buttons = 0;
    /* press_slot: the slot the mouse went down on, latched until release.
       drag_slot: only set once the mouse has actually moved past a small
       threshold while held, so a plain click (down, no movement, up)
       never gets mistaken for a drag onto its own slot. */
    int press_slot = -1, press_x = 0, press_y = 0, drag_slot = -1, press_window = -1;
    int r3drag_on = 0, r3drag_x = 0, r3drag_y = 0;   /* a press held and moved inside a ring-3 window's content: JT_EV_DRAG steps */
    /* Window title-bar drag: win_drag_armed latches on a title-bar press
       (the same "record it, only promote to a real drag past a small
       threshold" shape drag_slot above already uses for dock reordering).
       drag_win is only set once the threshold is crossed, so a plain
       click on the title bar still falls through to press_window's
       existing click-anywhere-closes contract instead of being eaten by
       a drag that never really happened. drag_zone/last_drag_zone track
       which snap zone (if any) the pointer is over, redrawn only on a
       real zone change, not on every mouse-moved event. */
    int win_drag_armed = 0, drag_win = -1, drag_grab_dx = 0, drag_grab_dy = 0;
    int drag_zone = -1;
    /* menu_open: the Apple-menu-style dropdown off the tree logo.
       menu_opening: true for exactly the one release that completes the
       same click that opened it, so that release doesn't also count as
       the "pick an item or dismiss" click, real macOS menu behavior
       (open on press, stays open past that first release, a real
       second click either picks something or dismisses it). */
    int menu_open = 0, menu_opening = 0;
    int notif_open = 0, notif_opening = 0, notif_draw_pending = 0; /* v42: the clock's panel, same open-on-press/dismiss-on-next-release contract as the Apple menu */
    int weather_open = 0, weather_opening = 0, weather_draw_pending = 0; /* v53: same contract, off the weather text */

    int last_mx = mx, last_my = my, last_hover = -1, last_drag = -1, last_menu_open = 0, last_menu_hover = -2;
    gui_menubar_force_redraw(); /* this GUI session's first frame, the minute-change gate must not skip it */
    /* v0.76.51: real bug, direct report ("tree shows for a second on
       startup/login") -- the very first desktop paint below ran before
       wall_apply() had EVER been called this session, so wall_src still
       held its static initial value (wallpaper_rgb, the tree photo)
       regardless of wall_theme's real default (WALL_SAT). v0.76.45's dark-
       fallback fix only kicked in once something later called wall_apply,
       which never happened before this first frame. One call here applies
       the same want-map-but-no-fetch-yet logic to the actual first paint,
       not just every paint after it. */
    wall_apply(wall_theme != WALL_PHOTO);
    gui_draw_desktop(-1, -1, 0, 0);
    cursor_saved_x = cursor_saved_y = -1;
    gui_cursor_save(mx, my);
    gui_draw_cursor(mx, my);
    gui_dock_prewarm(); ring3app_autoopen_run(mx, my); /* `open=keyrate` / `open=toroid` boot flag, if set */
    (void)mouse_take_wheel(); /* a wheel step banked while booting (the PS/2 wake-up packet) is not input: a window under the pointer must not open already scrolled. Not mouse_get_wheel: that pumps the vmmouse queue and can eat the first pointer packet on a slow host */
    for (;;) {
        window_present(); r3stress_desktop_round(); pdestress_desktop_round(); __asm__ volatile ("hlt"); /* 1.9.23: stress=r3 hook, dead unless armed */
        /* v0.76.17: direct request ("time in top right needs live reload
           accuracy, right now it doesn't load when the minute or hour
           changes"). Root cause: gui_draw_menubar() already self-gates on
           a real minute change (gui_menubar_last_min below), but it was
           only ever CALLED from mouse-in-menubar paths or this loop's own
           ten-minute weather cycle -- an idle desktop with the cursor
           elsewhere could sit with a stale clock for up to ten minutes.
           Calling it unconditionally every iteration (~100Hz, this hlt
           wakes on the PIT) is cheap: cmos() reads plus an integer
           compare on every frame but the one where the minute actually
           ticks over, where it does the real (already-existing) redraw. */
        {
            int min_before = gui_menubar_last_min;
            gui_draw_menubar();
            if (gui_menubar_last_min != min_before && my < GUI_MENUBAR_H) { gui_cursor_save(mx, my); gui_draw_cursor(mx, my); }
        }
        /* v43: weather, after the desktop is already on screen so the
           fetch never delays the first frame, then every ten minutes. */
        if (!weather_tried_once || ticks() - weather_last_tick > 100 * 600) {
            weather_tried_once = 1;
            char before[24]; for (int i = 0; i < 24; i++) before[i] = weather_text[i];
            weather_fetch();
            if (strcmp(before, weather_text) != 0) { gui_menubar_force_redraw(); gui_draw_menubar(); if (my < GUI_MENUBAR_H) { gui_cursor_save(mx, my); gui_draw_cursor(mx, my); } }
            /* v75: the map wallpaper rides the same ten-minute cycle, right
               after the geo lookup it depends on. One fetch per session
               once it lands (the mosaic is kept), retried each cycle
               until then; a failure leaves the photo up, never a blank. */
            if (WALL_THEME_FETCHES() && !wall_map && geo_have && wall_fetch()) { wall_apply(1); gui_draw_desktop(-1, -1, 0, 0); gui_cursor_save(mx, my); gui_draw_cursor(mx, my); }
        }
        /* 1.9.26: SYS_LAUNCH_REQUEST pickup. The syscall only stored an index; the launch runs here,
           IF on, through the same two paths a dock click takes (window first, blocking fallback). */
        int sys_launched = 0;
        { int ra = 0, rk = jt_refresh_take(&ra); /* SYS_REFRESH pickup: fetch outside the gate, the app sees data_stamp move */
          if (rk == JT_REFRESH_WEATHER) { weather_tried_once = 1; weather_fetch(); gui_menubar_force_redraw(); }
          else if (rk == JT_REFRESH_STOCKS) { stx_range_hint = ra & 0xFF; stx_sel_hint = (ra >> 8) & 0xFF; stocks_fetch(stx_range_hint); } }
        { int li = jt_launch_take();
          if (li >= 0) {
              serial_puts("launchreq=pickup\n");
              if (gui_multiwin_dock_ok(li)) { if (gui_multiwin_open(li) < 0) gui_refuse_open(li); }
              else { gui_launch_from_dock(li); mx = app_cursor_x; my = app_cursor_y; }
              for (int wi = 0; wi < gui_window_count; wi++) gui_windows[wi].shown = 1;
              sys_launched = 1;
          } }
        /* v45: wind, 4 frames a second, only while the desktop itself is
           what's on screen. Timed on its first frame; if that frame took
           longer than a tenth of a second the machine is too slow for
           this (v86 in a browser) and it switches itself off for good. */
        {
            static unsigned int wind_last = 0; static int wind_dir = 1;
            if (wind_enabled && !menu_open && !notif_open && !weather_open && drag_slot < 0 && gui_window_count == 0 && ticks() - wind_last >= 5) { /* cached wallpaper: ~3 ticks per frame, leaving input time at 20 fps; v0.73.0: also off while a multi-window app is open, same reason as the other overlay states, its wallpaper-row redraw would paint straight over an open window's content since neither the wind sway path nor the window list know about each other yet */
                wind_last = ticks();
                wind_phase += wind_dir * 3; /* same slow sway period at the higher frame rate */ if (wind_phase >= 256 || wind_phase <= -256) wind_dir = -wind_dir;
                unsigned int t0 = ticks();
                int sc = (int)window_scale();
                int cx0 = cursor_saved_x, cy0 = cursor_saved_y;
                if (cx0 >= 0) gui_draw_wallpaper_rows_sway_ex(WIND_TOP_ROW, WIND_HORIZON_ROW, 1, cx0 * sc, cy0 * sc, CURSOR_W * sc, CURSOR_H * sc);
                else gui_draw_wallpaper_rows_sway(WIND_TOP_ROW, WIND_HORIZON_ROW, 1);
                /* the backup under the cursor must track the sway too, or the
                   next real cursor move would restore a pre-wind patch */
                if (cx0 >= 0) { int csc = gui_cursor_scale(), pw = CURSOR_W * csc, ph = CURSOR_H * csc; /* physical-res backup, same layout as gui_cursor_save */
                    for (int j = 0; j < ph; j++) { int py = cy0 * csc + j; int ly = py / csc; if (ly < WIND_TOP_ROW || ly >= WIND_HORIZON_ROW) continue;
                        for (int i = 0; i < pw; i++) cursor_backup[j * pw + i] = gui_wallpaper_sample(cx0 * csc + i, py, 1); } }
                /* v65: rain/snow, drawn on top of the freshly repainted sway
                   band, right here on purpose: that repaint is what erases
                   last frame's particles, so drawing before it would just
                   get overwritten, and drawing it here keeps the cost
                   inside the same dt budget check right below, the same
                   self-throttle the wind redraw itself already relies on. */
                weather_fx_tick((int)window_width());
                /* two slow frames in a row, not one: the first frame under
                   QEMU includes the JIT translating this very loop and can
                   trip a single-frame gate falsely */
                { static int slow = 0, logged = 0; unsigned int dt = ticks() - t0;
                  if (!logged) { logged = 1; char b[24]; int i = 0; b[i++]='w'; b[i++]='i'; b[i++]='n'; b[i++]='d'; b[i++]='='; if (dt >= 10) b[i++]='0'+dt/10%10; b[i++]='0'+dt%10; b[i++]='t'; b[i++]='\n'; b[i]=0; serial_puts(b); }
                  if (dt > 25) wind_enabled = 0; else if (dt > 12) { if (++slow >= 2) wind_enabled = 0; } else slow = 0; }
            }
        }
        /* v0.75.0 (batch 2): when the topmost open multiwin window is one
           of the real-input apps (Mail/Calendar/Reminders), a keystroke
           routes to its own on_key handler instead of the old global
           "esc quits the whole GUI" check -- exactly the same
           "topmost/focused window owns input" rule click-to-focus
           already established for clicks (gui_multiwin_hit_test above).
           Only one kbd_pop() happens per frame either way, so the two
           branches can't double-consume the same scancode. */
        int mw_topmost_icon = gui_window_count > 0 ? gui_windows[gui_window_count - 1].icon : -1;
        int mw_key_repaint = 0;
        int r3_dirty_top = 0; /* 1.9.23: the focused ring-3 window presented a frame; cheap content-only blit below */
        for (int i = 0; i < gui_window_count; i++) {
            if (!gui_windows[i].shown) { gui_windows[i].shown = 1; mw_key_repaint = 1; }
            if (gui_windows[i].task < 0) continue;
            if (!task_used(gui_windows[i].task)) {
                /* The program exited or was reaped (idt.c): close only its
                   window. Everything else on screen stays. */
                ring3app_window_reaped(gui_windows[i].task, task_last_exit_code());
                gui_multiwin_close(i); i--; mw_key_repaint = 1;
                continue;
            }
            unsigned int fw, fh; int dirty = 0;
            syscall_window_fb(gui_windows[i].task, &fw, &fh, &dirty);
            if (dirty) { if (i == gui_window_count - 1) r3_dirty_top = 1; else mw_key_repaint = 1; }
        }
        /* App switcher hotkey, checked before the per-app dispatch just
           below gets its own single kbd_pop() this frame. kbd_peek()
           (irq.c) only tells us what's next without eating it, so a plain
           Alt+Tab with one or zero windows open -- or any other scancode
           entirely -- falls straight through untouched. Only a real
           Tab-make while Alt/Ctrl is down, or the matching modifier
           release while the panel is open, is ever actually popped here. */
        if (gui_window_count > 1) {
            int sw_pk = kbd_peek();
            if (sw_pk == 0x0F && (kbd_alt || kbd_ctrl)) {
                kbd_pop();
                if (!gui_switcher_open) { gui_switcher_open = 1; gui_switcher_idx = gui_window_count - 1; }
                gui_switcher_idx = (gui_switcher_idx + 1) % gui_window_count;
                gui_switcher_draw(gui_switcher_idx);
                serial_puts("SWITCHER:tab\n");
            } else if (gui_switcher_open && (sw_pk == 0xB8 || sw_pk == 0x9D)) {
                kbd_pop();
                gui_switcher_open = 0;
                gui_multiwin_focus(gui_switcher_idx);
                mw_key_repaint = 1; /* the panel is gone and the z-order changed: needs the real full repaint, same as any other focus/close change */
                serial_puts("SWITCHER:focus\n");
            }
        }
        /* Screenshot: Ctrl+Shift+3. Same peek-first discipline as the
           switcher above -- '3' is only ever consumed here when both
           modifiers are already down, which real typing never does, so a
           lone '3' keystroke elsewhere on the desktop is untouched. */
        if (kbd_ctrl && kbd_shift) {
            int ss_pk = kbd_peek();
            if (ss_pk == 0x04) {
                kbd_pop();
                char shot_name[13];
                unsigned int shot_bytes = gui_screenshot_save(shot_name);
                if (shot_bytes) {
                    char nbuf[12]; int ni = 0; unsigned int v = shot_bytes;
                    do { nbuf[ni++] = (char)('0' + v % 10); v /= 10; } while (v);
                    serial_puts("SHOT:");
                    serial_puts(shot_name);
                    serial_puts(":");
                    while (ni) { char d[2] = { nbuf[--ni], 0 }; serial_puts(d); }
                    serial_puts("\n");
                    gui_screenshot_flash(shot_name);
                    mw_key_repaint = 1; /* the flash banner drew over the desktop; needs a real repaint to erase it */
                }
            }
        }
        /* Text shell: Ctrl+Alt+Backspace, the deliberate way off the
           desktop now that a bare Esc there is a no-op. */
        if (kbd_ctrl && kbd_alt) {
            int shell_pk = kbd_peek();
            if (shell_pk == 0x0E) { kbd_pop(); break; }
        }
        if (gui_multiwin_interactive(mw_topmost_icon)) {
            int mwk = gui_multiwin_key_nonblock();
            if (mwk >= 0 && gui_windows[gui_window_count - 1].task >= 0) {
                /* 1.9.23: the focused window is a ring-3 program: the key
                   goes into its event ring and nowhere else. It redraws
                   through JT_POLL_PRESENT, which the dirty pass below picks up. */
                syscall_window_push_event(gui_windows[gui_window_count - 1].task, JT_EV_KEY, mwk, 0);
            } else if (mwk >= 0) {
                int mw_should_close = 0, mw_count_before_key = gui_window_count;
                mw_should_close = APPS[mw_topmost_icon].key(mwk);
                if (mw_should_close) {
                    gui_multiwin_close(gui_window_count - 1);
                    mw_key_repaint = 1; /* the window left the screen: needs the real full desktop repaint to erase it, the same cost every open/close already pays */
                } else if (gui_window_count != mw_count_before_key) {
                    mw_key_repaint = 1; /* v1.9.0: opened another window as a side effect (Mail's 'c'); its chrome was never drawn, needs a full repaint like any window-count change */
                } else {
                    /* v0.75.0: a cheap, scoped repaint tier, the same
                       "cheapest repaint that's correct" discipline
                       cursor_only/dock_only below already established.
                       Forcing the FULL desktop repaint (wallpaper photo
                       blit + menubar + dock, the expensive path those
                       two tiers exist to avoid) on every single keystroke
                       while typing (adding a reminder, composing mail)
                       is real, measurable overkill batch-2 would
                       otherwise add -- and not just cosmetic: a real
                       bug this pass caught and fixed before shipping, a
                       fast multi-character type burst forcing a full
                       photo-blit redraw on every keystroke could fall
                       behind the keyboard IRQ ring's fill rate, dropping
                       real keystrokes and intermittently failing
                       tools/checks/app-interact-check.py's Reminders/
                       Mail/Calendar interaction steps (confirmed: this
                       exact non-scoped `launched=1` version reproduced
                       the flakiness live, headless, multiple runs). The
                       window's own rect is self-contained -- it always
                       draws its own full chrome + content top to bottom
                       -- so redrawing just that window, patching the
                       cursor around it the same way cursor_only/
                       dock_only do, is the whole real fix: nothing
                       outside the window rect changed.

                       v0.76.18: tightened further, direct report ("keystroke
                       re-rendering glitch still present"). This tier was
                       already scoped to one window instead of the whole
                       desktop, but still called the OLD gui_multiwin_draw_one,
                       which redrew that window's full chrome (the rounded-
                       rect wallpaper blend + all three traffic lights + the
                       title) on every keystroke even though none of it
                       changes while typing -- only the content viewport
                       does. gui_multiwin_draw_content_only skips exactly
                       that unchanged part, the same "cheapest repaint that's
                       correct" cut cursor_only/dock_only/menu_only already
                       make for their own chrome. */
                    gui_cursor_restore();
                    gui_multiwin_draw_content_only(&gui_windows[gui_window_count - 1]);
                    gui_cursor_save(last_mx, last_my);
                    gui_draw_cursor(last_mx, last_my);
                }
            }
        } else {
            int sc = kbd_pop();
            if (sc >= 0 && !(sc & 0x80)) {
                char c = SC[sc & 0x7F];
                if (c == 27) {
                    /* Esc closes the window; a no-op on a bare desktop (Ctrl+Alt+Backspace reaches the shell). */
                    if (gui_window_count == 0) { /* no-op */ }
                    else { gui_multiwin_close(gui_window_count - 1); mw_key_repaint = 1; }
                } else if (c == '\n' && gui_window_count == 0) {
                    gui_launch_apps(); /* direct, not gui_launch_from_dock's boxed frame (moves the a11y close pixel) */
                }
            }
        }
        int dx = 0, dy = 0;
        int moved_mouse = mouse_get_delta(&dx, &dy, &buttons);
        if (moved_mouse) {
            mx += dx; my += dy;
            mouse_get_absolute(&mx, &my, (int)window_width(), (int)window_height()); /* v62: a tap lands exactly here, no travel */
            if (mx < 0) mx = 0; if ((unsigned)mx >= window_width())  mx = (int)window_width() - 1;
            if (my < 0) my = 0; if ((unsigned)my >= window_height()) my = (int)window_height() - 1;
        }
        int held = buttons & 1;
        int just_pressed = held && !(prev_buttons & 1);
        int just_released = !held && (prev_buttons & 1);
        int bleed_now = gui_bleed_open(); /* a full-screen window hides the menu bar and dock, so their hot spots go dead too: its red dot sits where the logo was */
        int logo_here = !bleed_now && !menu_open && !notif_open && !weather_open && mx >= 4 && mx <= 28 && my < GUI_MENUBAR_H;
        int clock_here = !bleed_now && !menu_open && !notif_open && !weather_open && mx >= (int)window_width() - 200 && my < GUI_MENUBAR_H;
        int weather_here = !bleed_now && !menu_open && !notif_open && !weather_open && weather_hit_x0 >= 0 && mx >= weather_hit_x0 && mx <= weather_hit_x1 && my < GUI_MENUBAR_H;
        int slot_here = (bleed_now || menu_open || notif_open || weather_open) ? -1 : gui_dock_hit_test(mx, my); /* the dock is inert while a panel covers it */
        int win_hit_here = (menu_open || notif_open || weather_open) ? -1 : gui_multiwin_hit_test(mx, my); /* v0.73.6: real hit test against every open window, topmost first, see gui_multiwin_hit_test */
        int win_close_here = (win_hit_here >= 0 && win_hit_here == gui_window_count - 1) ? win_hit_here : -1; /* only the already-focused (topmost) window's own click-anywhere-closes contract; a click on a background window is click-to-focus, not close, handled below */
        int win_focus_changed = 0; /* set below when a click raises a background window; folded into `launched` once it's declared, so the z-order change gets a real full repaint this same frame */
        {   /* The wheel goes to the ring-3 window under the pointer (JT_EV_WHEEL, +1 up, -1 down), the way a click does. Only the blocking launcher ever read it before, so no window app saw a wheel step. */
            int wz = mouse_take_wheel();   /* not mouse_get_wheel: that pumps the vmmouse queue again and swallows a click whose press and release land in one frame (v86) */
            if (wz && win_hit_here >= 0 && gui_windows[win_hit_here].task >= 0)
                syscall_window_push_event(gui_windows[win_hit_here].task, JT_EV_WHEEL, wz > 0 ? 1 : -1, 0);
        }

        if (just_pressed) {
            if (logo_here) { menu_open = 1; menu_opening = 1; }
            else if (weather_here) { weather_open = 1; weather_opening = 1; weather_draw_pending = 1; }
            else if (clock_here) { notif_open = 1; notif_opening = 1; notif_draw_pending = 1; }
            else if (win_close_here >= 0) {
                press_window = win_close_here;
                /* A press inside the topmost window's title bar (chrome
                   band, not the close circle itself) is also a drag
                   candidate; press_window stays set so a plain click
                   (no movement past the threshold below) still closes
                   the window exactly as it always has. */
                const gui_window_t *pw = &gui_windows[win_close_here];
                int in_titlebar = my >= pw->y && my < pw->y + 30;
                press_x = mx; press_y = my;   /* a content press remembers where, for the drag steps below */
                int cx = pw->x + 24, cy = pw->y + 16, ddx = mx - cx, ddy = my - cy;
                int on_close = (ddx * ddx + ddy * ddy) <= 9 * 9;
                if (in_titlebar && !on_close) {
                    win_drag_armed = 1; press_x = mx; press_y = my;
                    drag_grab_dx = mx - pw->x; drag_grab_dy = my - pw->y;
                }
            }
            else if (win_hit_here >= 0) {
                /* v0.73.6: real click-to-focus. The click landed inside a
                   visible BACKGROUND window's rect (win_close_here above
                   was -1, so it's not the topmost one). Raise it to the
                   front of the real z-order list right now, on press, and
                   swallow the click here -- it neither closes the window it
                   hit (that's not the "click anywhere closes" contract
                   until a SECOND click lands on it now that it's topmost)
                   nor falls through to the dock/app-launch paths below.
                   This is the one behavioural gap phase 1 explicitly left
                   open: "clicking the background window does nothing yet." */
                gui_multiwin_focus(win_hit_here);
                win_focus_changed = 1; /* z-order changed; force the full repaint below so the newly-front window is genuinely redrawn on top */
            }
            else if (slot_here >= 0) { press_slot = slot_here; press_x = mx; press_y = my; drag_slot = -1; }
        }

        if (held && press_slot >= 0 && drag_slot < 0) {
            int moved = (mx > press_x ? mx - press_x : press_x - mx) + (my > press_y ? my - press_y : press_y - my);
            if (moved > 8) drag_slot = press_slot; /* threshold crossed: this is a drag, not a click */
        }
        if (held && win_drag_armed && drag_win < 0) {
            int moved = (mx > press_x ? mx - press_x : press_x - mx) + (my > press_y ? my - press_y : press_y - my);
            if (moved > 8) { drag_win = press_window; press_window = -1; } /* real drag now: the release logic below moves/snaps instead of closing */
        }

        if (held && press_window >= 0 && !win_drag_armed && gui_windows[press_window].task >= 0 && press_y >= gui_windows[press_window].y + 30
            && (r3drag_on || mx != press_x || my != press_y)) {
            /* The pointer moving with the button held inside a ring-3 window's content: JT_EV_DRAG with the viewport position, the press's
               own first so the program knows what it grabbed, then one per new pixel, so a seek or volume knob follows the hand. A plain
               click sends nothing new, and the release still sends its click. */
            const gui_window_t *pw = &gui_windows[press_window];
            if (!r3drag_on) { r3drag_on = 1; r3drag_x = press_x; r3drag_y = press_y; syscall_window_push_drag(pw->task, press_x - (pw->x + 8), press_y - (pw->y + 32)); }
            if (mx != r3drag_x || my != r3drag_y) { r3drag_x = mx; r3drag_y = my; syscall_window_push_drag(pw->task, mx - (pw->x + 8), my - (pw->y + 32)); }
        }

        int launched = sys_launched || notif_draw_pending || weather_draw_pending || win_focus_changed || mw_key_repaint; notif_draw_pending = 0; weather_draw_pending = 0;
        if (just_released) {
            if (notif_open) {
                if (notif_opening) notif_opening = 0;
                else { notif_open = 0; launched = 1; } /* any release dismisses; force the full redraw that erases the panel */
            } else if (weather_open) {
                if (weather_opening) weather_opening = 0;
                else { weather_open = 0; launched = 1; } /* any release dismisses; force the full redraw that erases the panel */
            } else if (menu_open) {
                if (menu_opening) {
                    menu_opening = 0; /* this release just finishes the click that opened the menu; a real second click picks something or dismisses it */
                } else {
                    int item = gui_menu_hit_test(mx, my);
                    if (item >= 0) { gui_menu_run_item(item); launched = 1; } /* every real item takes over the screen or reboots/halts; force a fresh desktop redraw either way */
                    menu_open = 0;
                }
            } else if (drag_win >= 0) {
                /* Magnet-style drop: inside a zone, snap to that target
                   rect (the window's own w/h really change, re-derived
                   from the new size on the very next content redraw
                   below, not clipped or faked). Outside any zone, a real
                   free move to wherever it was dropped, same w/h -- safe
                   because every content function lays out from
                   window_width()/window_height(), not a hard-coded
                   position, so moving x/y alone never breaks layout. */
                int zone = gui_snap_zone(mx, my);
                gui_window_t *dw = &gui_windows[drag_win];
                if (zone >= 0) {
                    gui_snap_target(zone, &dw->x, &dw->y, &dw->w, &dw->h);
                } else {
                    int top, bottom; int sw = gui_snap_area(&top, &bottom);
                    int nx = mx - drag_grab_dx, ny = my - drag_grab_dy;
                    if (nx < 0) nx = 0; if (nx + dw->w > sw) nx = sw - dw->w;
                    if (ny < top) ny = top; if (ny + dw->h > bottom) ny = bottom - dw->h;
                    dw->x = nx; dw->y = ny;
                }
                launched = 1;
            } else if (press_window >= 0 && gui_windows[press_window].task >= 0
                       && !(mx >= gui_windows[press_window].x + 16 && mx <= gui_windows[press_window].x + 32
                            && my >= gui_windows[press_window].y + 8 && my <= gui_windows[press_window].y + 24)) {
                /* 1.9.23: a click inside a ring-3 window's content is the
                   program's, in viewport coordinates; only its red dot closes
                   it (task_kill, reaped on its next turn, window closed below). */
                syscall_window_push_event(gui_windows[press_window].task, JT_EV_CLICK,
                                          mx - (gui_windows[press_window].x + 8), my - (gui_windows[press_window].y + 32));
            } else if (press_window >= 0 && gui_windows[press_window].task >= 0) {
                task_kill(gui_windows[press_window].task);
            } else if (press_window >= 0) {
                /* v0.73.0: closing this window is exactly it, no reopen/
                   switch behaviour (that's v68's dock-tile close-and-open,
                   which only applies to the old blocking single-window
                   path); the other open window, if any, stays open and
                   drawn, proven by gui_multiwin_draw_all below still
                   iterating whatever's left in the list. */
                gui_multiwin_close(press_window);
                launched = 1;
            } else if (drag_slot >= 0) {
                int target = gui_slot_at(mx);
                int tmp = gui_order[drag_slot];
                gui_order[drag_slot] = gui_order[target];
                gui_order[target] = tmp;
            } else if (press_slot >= 0 && press_slot == slot_here && gui_multiwin_dock_ok(gui_order[press_slot])) {
                /* v0.73.0: real phase-1 multi-window path for Files/Weather,
                   see the big comment above gui_multiwin_open. Non-blocking
                   on purpose: adds/focuses the window in the real list and
                   returns immediately, so this same gui_run loop keeps
                   running (dock hover, the other open window's redraw,
                   everything) instead of blocking inside gui_wait_close the
                   way every other app still does. */
                editor_mouse_x = mx; editor_mouse_y = my;
                /* 2.0 gate 5: at the GUI_MULTIWIN_MAX cap (or a failed window launch) the click is an
                   honest refusal, "winrefuse" on serial and a notice on screen, never a blocking takeover. */
                if (gui_multiwin_open(gui_order[press_slot]) < 0) gui_refuse_open(gui_order[press_slot]);
                /* the launched=1 full repaint below draws this window's chrome; do not let the first-frame check draw it again */
                for (int wi = 0; wi < gui_window_count; wi++) gui_windows[wi].shown = 1;
                launched = 1;
            } else if (press_slot >= 0 && press_slot == slot_here) {
                editor_mouse_x = mx; editor_mouse_y = my;
                gui_launch_from_dock(gui_order[press_slot]);
                mx = app_cursor_x; my = app_cursor_y; /* v68: the app's own loop tracked the pointer while it was open; pick up where it really is, not where the launching click was */
                /* v86 (0.71.0), a real but narrow staleness bug found
                   while investigating the "Files dock icon stuck
                   hovering" report: slot_here above was computed from the
                   mouse position BEFORE this entire blocking app session
                   (the dock tile that was clicked to open it), and
                   nothing recomputed it against the cursor's real
                   post-close position before hover_slot (below) consumes
                   it this same frame. Confirmed harmless in the common
                   case only because dock_hover_extra was usually already
                   saturated at DOCK_MAGNIFY from hovering the tile before
                   the click, so the stale target rarely causes a visible
                   *increase*; it only delays this frame's decay-start by
                   one redraw, self-correcting on the very next frame once
                   slot_here is recomputed fresh at the top of the loop.
                   Fixed anyway (correctness, not cosmetics: a delayed
                   decay-start IS a real one-frame staleness bug) by
                   recomputing here with the same call already used to
                   build slot_here in the first place (line ~4326).
                   Headless pixel verification (QMP abs-pointer + a
                   settled pmemsave, the dockhover-check.py pattern) could
                   NOT reproduce a PERSISTENT stuck-lifted tile either
                   before or after this fix, dropped as a non-discriminating
                   test rather than kept as false evidence; see ROADMAP.md
                   for the honest state of the underlying report, this fix
                   narrows a real gap but is not confirmed to be the full
                   explanation for a hang lasting more than one frame. */
                slot_here = (menu_open || notif_open || weather_open) ? -1 : gui_dock_hit_test(mx, my);
                launched = 1; /* the app view just took over the whole screen; force a redraw below even if the cursor never moved */
            }
            press_slot = -1; drag_slot = -1; press_window = -1; r3drag_on = 0;
            win_drag_armed = 0; drag_win = -1; drag_zone = -1;
        }
        prev_buttons = buttons;

        int hover_slot = (drag_slot < 0) ? slot_here : -1;
        dock_hover = hover_slot;
        int menu_hover = menu_open ? gui_menu_hit_test(mx, my) : -2;
        /* Redraw only when something actually visible changed. A real,
           user-visible bug this fixed, not just a cosmetic worry: redrawing
           the whole screen unconditionally on every single timer tick
           (~100/sec, whether or not the mouse ever moved) produced visible
           tearing on a real display, this framebuffer has no vsync and no
           back buffer, so a full-screen redraw mid-scanout shows a torn
           frame. Only redrawing on an actual state change cuts redraws from
           ~100/sec down to roughly "as fast as a human can move a mouse",
           which doesn't eliminate tearing (still no double buffering, an
           honest, separate, larger limitation) but makes it rare instead
           of constant. */
        /* v40: three tiers of repaint, cheapest that's correct.
           Before this, ANY change, including a 1px cursor jitter, ran the
           full path below (whole photo blit + dock + eight supersampled
           icons) with no double buffer to hide it, which is exactly the
           "icons flash when I hover" report: the flashing was the
           repaint. */
        /* Window drag preview: only redraws when the snap zone the
           pointer is over actually changes (entering/leaving/switching a
           zone), never on every mouse-moved event -- the window itself
           is left exactly where it started until release, so this is the
           whole cost of the live preview. */
        int cur_snap_zone = drag_win >= 0 ? gui_snap_zone(mx, my) : -1;
        /* Live move (direct request, "app windows should be draggable"):
           the window itself follows the pointer now, redrawn from its own
           state at the new x/y on every pointer move, with the snap
           outline still drawn on top when a zone is in reach. The clamp
           is the same one the release path applies, so what the visitor
           sees mid-drag is exactly where the window lands. */
        int drag_moved = drag_win >= 0 && !launched && (mx != last_mx || my != last_my);
        if (drag_moved) {
            gui_window_t *dw = &gui_windows[drag_win];
            int top, bottom; int sw = gui_snap_area(&top, &bottom);
            int nx = mx - drag_grab_dx, ny = my - drag_grab_dy;
            if (nx < 0) nx = 0; if (nx + dw->w > sw) nx = sw - dw->w;
            if (ny < top) ny = top; if (ny + dw->h > bottom) ny = bottom - dw->h;
            dw->x = nx; dw->y = ny;
        }
        int drag_zone_only = drag_win >= 0 && !launched && (cur_snap_zone != drag_zone || drag_moved);
        if (drag_zone_only) {
            gui_cursor_restore();
            gui_draw_desktop(-1, -1, 0, 0);
            if (gui_window_count > 0) gui_multiwin_draw_all();
            gui_snap_outline(cur_snap_zone);
            gui_cursor_save(mx, my);
            gui_draw_cursor(mx, my);
            drag_zone = cur_snap_zone;
            last_mx = mx; last_my = my;
        }
        int cursor_only = !launched && !drag_zone_only && (mx != last_mx || my != last_my)
                          && hover_slot == last_hover && drag_slot == last_drag
                          && menu_open == last_menu_open && menu_hover == last_menu_hover;
        int dock_only = !launched && !cursor_only && drag_slot < 0 && last_drag < 0
                        && !menu_open && !last_menu_open
                        && hover_slot != last_hover;
        /* v0.76.17: direct report, the exact "icons flash when I hover"
           shape v40's own three tiers above were built to fix, just never
           extended to the Apple menu's own hover highlight -- switching
           which row is highlighted while the dropdown is open fell through
           to the full-repaint branch below (whole photo blit + dock +
           every open window redrawn) on every single row hovered, since
           cursor_only explicitly excludes any menu_hover change and
           dock_only requires the menu to be closed. gui_draw_apple_menu is
           already fully self-contained (its own rounded-rect background
           fill covers its whole rect every call, same "cheapest repaint
           that's correct" shape gui_redraw_dock_band already established
           for the dock band), so this is a direct copy of that same
           pattern, not a new mechanism. */
        int menu_only = !launched && !cursor_only && !dock_only && drag_slot < 0 && last_drag < 0
                        && menu_open && last_menu_open
                        && (menu_hover != last_menu_hover || mx != last_mx || my != last_my);
        if (cursor_only) {
            gui_cursor_restore();
            if (my < GUI_MENUBAR_H || last_my < GUI_MENUBAR_H) { gui_menubar_force_redraw(); gui_draw_menubar(); }
            gui_cursor_save(mx, my);
            gui_draw_cursor(mx, my);
            last_mx = mx; last_my = my;
        } else if (dock_only) {
            gui_cursor_restore();
            gui_redraw_dock_band(hover_slot, drag_slot, mx, my);
            gui_cursor_save(mx, my);
            gui_draw_cursor(mx, my);
            last_mx = mx; last_my = my; last_hover = hover_slot;
        } else if (menu_only) {
            serial_puts("menuonly\n"); /* discriminating marker for tools/checks/menuclock-check.sh */
            gui_cursor_restore();
            gui_draw_apple_menu(menu_hover);
            gui_cursor_save(mx, my);
            gui_draw_cursor(mx, my);
            last_mx = mx; last_my = my; last_menu_hover = menu_hover;
        } else if (launched || mx != last_mx || my != last_my || hover_slot != last_hover || drag_slot != last_drag || menu_open != last_menu_open || menu_hover != last_menu_hover) {
            serial_puts("fullrepaint\n"); /* discriminating marker for tools/checks/menuclock-check.sh */
            if (my < GUI_MENUBAR_H || last_my < GUI_MENUBAR_H) gui_menubar_force_redraw();
            cursor_saved_x = cursor_saved_y = -1; /* the full repaint replaces whatever the backup held */
            gui_draw_desktop(hover_slot, drag_slot, mx, my);
            dock_presented_hover = dock_hover;
            if (gui_window_count > 0) gui_multiwin_draw_all(); /* v0.73.0: real simultaneous redraw of every open window, back-to-front, on top of the desktop just drawn above */
            if (menu_open) gui_draw_apple_menu(menu_hover);
            if (notif_open) gui_draw_notif_panel();
            if (weather_open) gui_draw_weather_panel();
            if (gui_notice_name) gui_notice_draw();
            if (drag_slot < 0) { gui_cursor_save(mx, my); gui_draw_cursor(mx, my); }
            last_mx = mx; last_my = my; last_hover = hover_slot; last_drag = drag_slot;
            last_menu_open = menu_open; last_menu_hover = menu_hover;
        }
        else if (r3_dirty_top) {
            /* 1.9.23: the focused ring-3 window has a new frame: blit just
               its content, the same cheap tier a Mail keystroke takes. */
            gui_cursor_restore();
            gui_multiwin_draw_content_only(&gui_windows[gui_window_count - 1]);
            gui_cursor_save(mx, my);
            gui_draw_cursor(mx, my);
        }
    }
    window_close();
    /* v77: free GUI heap allocations (wind_base, dock_band cache/frame) when
       exiting so the heap is available for other uses after gui_run() returns.
       This is critical for heap growth after the GUI: without this, the large
       allocations consume virtual address space and prevent paging_map_region
       from mapping new heap frames beyond the base map. */
    wall_caches_drop();
    gui_dock_band_cache_free();
    clear();
    puts("back in text mode\n");
}
