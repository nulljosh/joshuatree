/* v0.88.0: Activity, a real Activity Monitor-shaped app scoped honestly
   against what this kernel's scheduler actually tracks (kernel/task.h /
   task.c): task_used/task_max/task_kill/task_current are the exact same
   real primitives the shell's own `ps`/`kill`/`mem` commands already call
   (kernel.c's "ps"/"kill"/"mem" cases), reused here as-is, never
   reimplemented. There is no per-task name string anywhere in `struct
   task` (task.c), so this shows the one honest label the kernel actually
   has: slot 0 is always the shell/GUI's own stack (tasks_init's own
   comment: "the currently-running boot/shell stack", nothing else ever
   runs the GUI loop), every other used slot is labeled "Task N" by its
   real slot id, not a fabricated process name. TASK_SLOTS is a fixed,
   small constant (6), so all six rows are always shown, no scrolling.

   Same file/UI shape as search.h/reminders.h: gui_prompt.h's chrome/
   content split (titlebar + instructions drawn once, the stats/list/
   button redrawn every refresh), a fixed-size static row buffer, no
   persistence of its own -- there's nothing to persist, it mirrors the
   real scheduler and pmm state live. Refreshes about once a second
   (100 PIT ticks, the same 100Hz `ticks()` every other uptime/sleep
   command in this kernel already assumes) whether or not a key is
   pressed, so the task list and free-memory figure never go stale while
   the window sits open. */

#define ACTIVITY_ROWS TASK_SLOTS /* always show every real scheduler slot, free or used */

static int activity_sel;          /* selected row, 0..ACTIVITY_ROWS-1 */
static char activity_msg[64];     /* transient feedback line: kill result or refusal */
static unsigned int activity_msg_until; /* ticks() deadline; 0 means no message showing */

static void activity_itoa(unsigned int v, char *buf) {
    /* Same hand-rolled reverse-and-flip digit extraction the shell's own
       `ps` case already uses inline (kernel.c), pulled out here since
       this file needs it in three places (pid, uptime, mem). */
    char tmp[12]; int ti = 0;
    if (v == 0) tmp[ti++] = '0';
    while (v) { tmp[ti++] = '0' + v % 10; v /= 10; }
    int n = 0;
    while (ti) buf[n++] = tmp[--ti];
    buf[n] = 0;
}

/* Redraws only the stats line, the column header, the six task rows and
   the Kill button -- the gui_prompt.h chrome/content split search.h/
   reminders.h already established, never the titlebar/instructions
   above it. Returns nothing; called once up front and again every ~1s
   from the main loop below. */
static void activity_draw_content(void) {
    int w = (int)window_width();
    window_rect(16, 60, w - 32, (int)window_height() - 60, GUI_BG); /* clear the whole content band first: row count/text length both change every redraw */

    char num[12];
    font_draw_string("Uptime:", 20, 62, 0x0075726E, -1);
    activity_itoa(ticks() / 100, num);
    font_draw_string(num, 84, 62, 0x001C1C1E, -1);
    font_draw_string("s", 84 + font_string_width(num), 62, 0x001C1C1E, -1);

    font_draw_string("Memory:", 180, 62, 0x0075726E, -1);
    activity_itoa(pmm_free_frames() * 4, num);
    int mx = 244;
    font_draw_string(num, mx, 62, 0x001C1C1E, -1); mx += font_string_width(num);
    font_draw_string("K free /", mx, 62, 0x001C1C1E, -1); mx += font_string_width("K free /") + 6;
    activity_itoa(pmm_total_frames() * 4, num);
    font_draw_string(num, mx, 62, 0x001C1C1E, -1); mx += font_string_width(num);
    font_draw_string("K total", mx, 62, 0x001C1C1E, -1);

    font_draw_string("PID", 20, 88, 0x0075726E, -1);
    font_draw_string("NAME", 80, 88, 0x0075726E, -1);
    font_draw_string("STATE", 200, 88, 0x0075726E, -1);

    for (int i = 0; i < ACTIVITY_ROWS; i++) {
        int y = 108 + i * 22;
        if (i == activity_sel) window_rect(16, y - 4, w - 32, 20, 0x00EDE6DC);
        activity_itoa((unsigned int)i, num);
        font_draw_string(num, 20, y, 0x001C1C1E, -1);
        int current = (i == task_current());
        if (i == 0) font_draw_string("Shell/GUI", 80, y, 0x001C1C1E, -1);
        else if (task_used(i)) { char nm[16] = "Task "; activity_itoa((unsigned int)i, nm + 5); font_draw_string(nm, 80, y, 0x001C1C1E, -1); }
        else font_draw_string("--", 80, y, 0x00807468, -1);
        if (current) font_draw_string("running (this)", 200, y, 0x001C1C1E, -1);
        else if (task_used(i)) font_draw_string("running", 200, y, 0x00375A4A, -1);
        else font_draw_string("free", 200, y, 0x00807468, -1);
    }

    int btn_y = 108 + ACTIVITY_ROWS * 22 + 14;
    window_rect(20, btn_y, 96, 24, 0x00A13F3F);
    font_draw_string("Kill", 20 + (96 - font_string_width("Kill")) / 2, btn_y + 6, 0x00F5F0EB, -1);

    if (activity_msg_until && ticks() < activity_msg_until)
        font_draw_string(activity_msg, 130, btn_y + 6, 0x00A13F3F, -1);

    serial_puts("activitycontent\n"); /* discriminating marker for tools/checks/activity-check.py */
}

/* The Kill button's own fixed rect, in this app's content-relative
   coordinates -- shared between the click hit-test below and the draw
   call above so they can never drift apart. */
static int activity_kill_hit(int vx, int vy) {
    int btn_y = 108 + ACTIVITY_ROWS * 22 + 14;
    return vx >= 20 && vx < 20 + 96 && vy >= btn_y && vy < btn_y + 24;
}

/* Kills the selected slot through task_kill, the exact same real primitive
   the shell's own `kill` command calls (kernel.c's "kill" case) -- never a
   second implementation. Refuses the kernel/GUI's own task (slot 0, the
   only task this whole app is drawn from) and any already-free slot, both
   with a real, visible message rather than a silent no-op, even though
   task_kill itself already silently no-ops on id == current (task.c). */
static void activity_kill_selected(void) {
    int id = 0;
    while (activity_msg[id]) { activity_msg[id] = 0; id++; } /* clear any stale message before deciding the new one */
    if (activity_sel == task_current()) {
        const char *m = "can't kill the current task"; int i = 0; while (m[i]) { activity_msg[i] = m[i]; i++; } activity_msg[i] = 0;
    } else if (!task_used(activity_sel)) {
        const char *m = "that slot is already free"; int i = 0; while (m[i]) { activity_msg[i] = m[i]; i++; } activity_msg[i] = 0;
    } else {
        task_kill(activity_sel);
        const char *m = "kill signal sent"; int i = 0; while (m[i]) { activity_msg[i] = m[i]; i++; } activity_msg[i] = 0;
    }
    activity_msg_until = ticks() + 200; /* ~2s, plenty of time to read it before the next ~1s refresh repaints */
}

/* Not get_key_or_click(): that shared helper hlt-loops forever until a
   real key or click arrives, exactly right for every other list app here
   (Search, Contacts, ...) since none of them show live data that needs to
   move on its own. Activity does (uptime, free memory, task state), so
   its own event loop needs to wake up and redraw on a timer even when
   nobody touches the keyboard or mouse -- the one real difference from
   get_key_or_click's own logic, inlined here rather than changing a
   function every other app in this kernel already relies on blocking.
   PIT IRQ0 (100Hz) wakes hlt on its own every ~10ms regardless of input,
   the exact same real mechanism sleep_ticks() already leans on elsewhere
   in this kernel, so this costs nothing extra: the loop below already
   re-checks ticks() every real timer tick, not on some busy poll. */
static void gui_launch_activity(void) {
    activity_sel = 0;
    activity_msg[0] = 0;
    activity_msg_until = 0;
    mouse_click_edge_sync();

    /* Chrome drawn once: titlebar + instructions never change while the
       app is open, same split search.h already uses. */
    window_clear(GUI_BG);
    gui_draw_app_titlebar("Activity");
    font_draw_string("up/down select   k kills   esc closes", 20, 42, 0x0075726E, -1);

    unsigned int next_refresh = ticks();

    for (;;) {
        if (ticks() >= next_refresh) { activity_draw_content(); next_refresh = ticks() + 100; } /* ~once a second, the real 100Hz PIT rate every uptime/sleep command here assumes */

        gui_app_mouse_tick();
        int k = -1;
        int sc = kbd_pop();
        if (sc >= 0) {
            if (sc == 0xE0) {
                int sc2; do { sc2 = kbd_pop(); if (sc2 < 0) { window_present(); __asm__ volatile ("hlt"); } } while (sc2 < 0);
                if (sc2 == 0x48) k = KEY_UP;
                else if (sc2 == 0x50) k = KEY_DOWN;
                else if (sc2 == 0x4B) k = KEY_LEFT;
                else if (sc2 == 0x4D) k = KEY_RIGHT;
            } else if (!(sc & 0x80)) {
                char c = kbd_map(sc);
                gui_close_was_click = 0;
                if (c == '\n') k = KEY_ENTER;
                else if (c == 27) k = KEY_ESC;
                else if (c) k = c;
            }
        } else if (mouse_click_edge()) { gui_close_was_click = 1; k = KEY_CLICK; }
        else { int wheel = mouse_get_wheel(); if (wheel) k = wheel > 0 ? KEY_WHEEL_UP : KEY_WHEEL_DOWN; }

        if (k < 0) { window_present(); __asm__ volatile ("hlt"); continue; }

        if (k == KEY_ESC) return;
        if (k == KEY_UP) { if (activity_sel > 0) activity_sel--; activity_draw_content(); continue; }
        if (k == KEY_DOWN) { if (activity_sel < ACTIVITY_ROWS - 1) activity_sel++; activity_draw_content(); continue; }
        if (k == 'k') { activity_kill_selected(); activity_draw_content(); continue; }
        if (k == KEY_CLICK) {
            int click_vx = app_cursor_x - app_view_x, click_vy = app_cursor_y - app_view_y;
            if (activity_kill_hit(click_vx, click_vy)) { activity_kill_selected(); activity_draw_content(); continue; }
            int hit = -1;
            for (int i = 0; i < ACTIVITY_ROWS; i++) {
                int y = 108 + i * 22;
                if (click_vx >= 16 && click_vx < (int)window_width() - 16 && click_vy >= y - 4 && click_vy < y + 16) { hit = i; break; }
            }
            if (hit < 0) return; /* click anywhere else closes, the same contract every other list app here has */
            activity_sel = hit;
            activity_draw_content();
            continue;
        }
    }
}
