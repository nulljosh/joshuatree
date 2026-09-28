/* 1.7.7: Keyrate as a real ring-3 process, and the supervisor around it.
   1.7.11: Toroid joins it, and the launcher became one table (RING3_APPS).
   1.7.12: Calculator joins it, the third app out.
   1.7.13: Homeqi joins it, the fourth app out.

   Roadmap 2.0 says apps leave the kernel, so a crash in one cannot take
   the machine down. Keyrate, the smallest real app, went first; Toroid
   and Calculator followed the same path. Each app's dock entry lands here,
   and everything else stays as it was: gui_launch_from_dock has already
   drawn the window chrome and set the viewport by the time this runs,
   just as for an in-kernel app.

   What "protected" means here, concretely: the program runs at CPL 3 with
   its own page directory and IOPL 0, reaches the kernel only through int
   0x80, and draws only into the framebuffer SYS_WINDOW_OPEN handed it. A
   bad pointer, a divide by zero, a privileged instruction: idt.c's
   ring-3 path reaps the task and the kernel keeps running. task_exit_with
   calls syscall_release_task, which drops the window, so by the time
   exec_user returns here the program is gone and its window with it,
   whichever way it ended. This function then logs what happened and
   returns to the desktop, which repaints. No panic, no reboot.

   Each binary rides along in kernel.elf (drivers/user_<app>.h, generated
   by the Makefile from the real built user/<app>.bin) and is seeded onto
   the VFS on first launch, the same way `usertest` seeds HELLO.BIN: the
   headless checks and the browser demo have no disk. */
#include "ring3app.h"
#include "exec.h"
#include "syscall.h"
#include "task.h"
#include "vfs.h"
#include "serial.h"
#include "user_keyrate.h"
#include "user_toroid.h"
#include "user_calculator.h"
#include "user_homeqi.h"
#include "user_fbpoke.h"
#include "app.h"
#include "irq.h"
#include "mouse.h"

static void put_dec(char *out, int v) {
    char tmp[12]; int i = 0, n = 0;
    unsigned u = v < 0 ? (unsigned)(-v) : (unsigned)v;
    if (v < 0) out[n++] = '-';
    do { tmp[i++] = (char)('0' + u % 10); u /= 10; } while (u);
    while (i) out[n++] = tmp[--i];
    out[n] = 0;
}

static const char *EXC_SHORT[32] = {
    "divide-by-zero", "debug", "NMI", "breakpoint", "overflow", "bound-range", "invalid-opcode",
    "device-not-available", "double-fault", "coprocessor-overrun", "invalid-TSS", "segment-not-present",
    "stack-fault", "general-protection", "page-fault", "reserved", "x87-fp", "alignment-check",
    "machine-check", "SIMD-fp", "virtualization", "control-protection", "reserved", "reserved",
    "reserved", "reserved", "reserved", "reserved", "reserved", "reserved", "reserved", "reserved",
};

int gui_app_view_size(unsigned int *w, unsigned int *h);

/* 1.7.11: one launcher for every app that runs at ring 3. A row is the
   app's name, the binary embedded in kernel.elf (drivers/user_<app>.h,
   generated from the real built user/<app>.bin) and the VFS filename it
   is seeded under on first launch. APPS[] in kernel.c points each ring-3
   app's `open` at the small wrapper below its row. */
struct ring3_app { const char *name; const unsigned char *bin; unsigned int len; const char *file; };
static const struct ring3_app RING3_APPS[] = {
    {"Keyrate",    user_keyrate,    USER_KEYRATE_LEN,    "KEYRATE.BIN"},
    {"Toroid",     user_toroid,     USER_TOROID_LEN,     "TOROID.BIN"},
    {"Calculator", user_calculator, USER_CALCULATOR_LEN, "CALC.BIN"},
    {"Homeqi",     user_homeqi,     USER_HOMEQI_LEN,     "HOMEQI.BIN"},
};

static void ring3app_launch(const struct ring3_app *a) {
    unsigned char probe[1];
    if (vfs_read_file(a->file, probe, 1) < 0 &&
        !vfs_write_file(a->file, a->bin, a->len)) {
        serial_puts("ring3app: could not seed "); serial_puts(a->file); serial_puts(", not started\n");
        return;
    }
    /* The viewport must fit the ring-3 framebuffer, or SYS_WINDOW_OPEN
       fails and the program exits before it draws a thing. Checked here,
       loudly, so a viewport that outgrows .userfb (boot/linker.ld) shows
       up as one serial line rather than an app that "never opens". */
    unsigned int vw = 0, vh = 0;
    if (!gui_app_view_size(&vw, &vh)) {
        /* gui_app_windowed is 0 or app_view_w/h are 0: the caller opened
           this app without going through gui_launch_from_dock's windowed
           setup (e.g. a keyboard path that called gui_launch_apps directly
           instead of gui_launch_from_dock(GUI_APPS_FOLDER)). Launching
           anyway means SYS_WINDOW_OPEN fails ENODEV and the app exits
           before it draws, which reads as "never opens". Refuse loudly and
           hand control back to the desktop instead. */
        serial_puts("ring3app: BUG no app viewport, not launching\n");
        return;
    }
    if (vw * vh * 4 > JT_USER_FB_BYTES) {
        serial_puts("ring3app: BUG app viewport does not fit JT_USER_FB, grow .userfb in boot/linker.ld\n");
        return;
    }
    serial_puts("ring3app: launching "); serial_puts(a->file); serial_puts(" at ring 3\n");
    int status = -1;
    const char *argv[] = { a->file };
    if (!exec_user(a->file, argv, 1, &status)) {
        serial_puts("ring3app: exec_user failed (not found, too big, or no free task slot)\n");
        return;
    }
    /* exec_user returned, so the task slot is free and syscall_release_task
       has already run for it. A negative status is idt.c's -(vector): the
       program crashed and was reaped. Zero or positive is its own exit
       code. Either way the desktop is what comes next. */
    char num[12]; put_dec(num, status);
    serial_puts("ring3app: "); serial_puts(a->file);
    if (status < 0 && -status < 32) {
        serial_puts(" crashed ("); serial_puts(EXC_SHORT[-status]);
        serial_puts("), window torn down, desktop alive\n");
    } else {
        serial_puts(" exited "); serial_puts(num); serial_puts(", window torn down, desktop alive\n");
    }
    if (syscall_window_owner() >= 0) serial_puts("ring3app: BUG window still owned after the task ended\n");
}
void keyrate_ring3_open(void)    { ring3app_launch(&RING3_APPS[0]); }
void toroid_ring3_open(void)     { ring3app_launch(&RING3_APPS[1]); }
void calculator_ring3_open(void) { ring3app_launch(&RING3_APPS[2]); }
void homeqi_ring3_open(void)     { ring3app_launch(&RING3_APPS[3]); }

/* 1.7.8: `fbpoke` boot flag. After the auto-opened Keyrate has exited,
   run user/fbpoke.c with no window: it must be refused a pointer into
   the released framebuffer and must page-fault storing into it. The
   check reads this function's lines; a program that "exited" here
   instead of crashing is the 1.7.7 hole reopened. */
static int fbpoke_armed = 0;
static void fbpoke_run(void) {
    unsigned char probe[1];
    if (vfs_read_file("FBPOKE.BIN", probe, 1) < 0 &&
        !vfs_write_file("FBPOKE.BIN", user_fbpoke, USER_FBPOKE_LEN)) {
        serial_puts("ring3app: could not seed FBPOKE.BIN, not started\n");
        return;
    }
    serial_puts("ring3app: launching FBPOKE.BIN at ring 3, no window\n");
    int status = -1;
    const char *argv[] = { "FBPOKE.BIN" };
    if (!exec_user("FBPOKE.BIN", argv, 1, &status)) { serial_puts("ring3app: exec_user failed for FBPOKE.BIN\n"); return; }
    char num[12]; put_dec(num, status);
    if (status < 0 && -status < 32) {
        serial_puts("ring3app: FBPOKE.BIN crashed ("); serial_puts(EXC_SHORT[-status]);
        serial_puts("), released framebuffer stayed supervisor-only\n");
    } else {
        serial_puts("ring3app: BUG FBPOKE.BIN exited "); serial_puts(num); serial_puts(", the released framebuffer was still writable\n");
    }
}

static int ring3app_autoopen_slot = -1; /* APPS[] index to open, -1 when unarmed */
void ring3app_autoopen_arm(const char *cl){
    for (const char *pc = cl; pc && *pc; pc++) {
        if (pc[0]=='o' && pc[1]=='p' && pc[2]=='e' && pc[3]=='n' && pc[4]=='=' && pc[5]=='k' && pc[6]=='e' && pc[7]=='y' && pc[8]=='r') { ring3app_autoopen_slot = 9; serial_puts("autoopen=keyrate\n"); }
        if (pc[0]=='o' && pc[1]=='p' && pc[2]=='e' && pc[3]=='n' && pc[4]=='=' && pc[5]=='t' && pc[6]=='o' && pc[7]=='r' && pc[8]=='o') { ring3app_autoopen_slot = 14; serial_puts("autoopen=toroid\n"); }
        if (pc[0]=='o' && pc[1]=='p' && pc[2]=='e' && pc[3]=='n' && pc[4]=='=' && pc[5]=='c' && pc[6]=='a' && pc[7]=='l' && pc[8]=='c') { ring3app_autoopen_slot = 19; serial_puts("autoopen=calculator\n"); }
        if (pc[0]=='o' && pc[1]=='p' && pc[2]=='e' && pc[3]=='n' && pc[4]=='=' && pc[5]=='h' && pc[6]=='o' && pc[7]=='m' && pc[8]=='e') { ring3app_autoopen_slot = 16; serial_puts("autoopen=homeqi\n"); }
        if (pc[0]=='f' && pc[1]=='b' && pc[2]=='p' && pc[3]=='o' && pc[4]=='k' && pc[5]=='e') { fbpoke_armed = 1; serial_puts("fbpoke armed\n"); }
    }
}
void ring3app_autoopen_run(int mx, int my){
    if (ring3app_autoopen_slot < 0) return;
    int slot = ring3app_autoopen_slot; ring3app_autoopen_slot = -1;
    editor_mouse_x = mx; editor_mouse_y = my;
    gui_launch_from_dock(slot); /* Keyrate's, Toroid's, Calculator's or Homeqi's APPS slot */
    if (fbpoke_armed) { fbpoke_armed = 0; fbpoke_run(); }
    gui_draw_desktop(-1, -1, 0, 0);
    cursor_saved_x = cursor_saved_y = -1;
    gui_cursor_save(mx, my);
    gui_draw_cursor(mx, my);
    serial_puts("autoopen: back on the desktop\n");
}

/* 1.7.7: the size of the app viewport a dock launch opened, for
   kernel/syscall.c's SYS_WINDOW_OPEN. 0 when no app window is open. */
int gui_app_view_size(unsigned int *w, unsigned int *h){
    if (!gui_app_windowed || app_view_w <= 0 || app_view_h <= 0) return 0;
    *w = (unsigned int)app_view_w; *h = (unsigned int)app_view_h;
    return 1;
}

/* 1.7.7: the non-blocking twin of kernel.c's get_key_or_click_until, for
   kernel/syscall.c's SYS_WINDOW_POLL. It runs inside the int 0x80 gate
   with interrupts off, so it must never wait: one look at the keyboard
   queue and the mouse, then back. Returns JT_EV_KEY (1) with the key in
   *a (the same ASCII/KEY_* values the blocking loop hands in-kernel apps),
   JT_EV_CLICK (2) with the pointer in app-window coordinates, JT_EV_WHEEL
   (3) with +1/-1 in *a, or 0 when nothing happened. A 0xE0 prefix whose
   second byte has not arrived yet (it cannot, with IF clear) is remembered
   for the next call rather than dropped, so arrows still work. */
static int gui_poll_pending_e0 = 0;
int gui_poll_event(int *a, int *b){
    *a = 0; *b = 0;
    gui_app_mouse_tick();
    int sc = kbd_pop();
    if (sc >= 0) {
        if (sc == 0xE0 && !gui_poll_pending_e0) { gui_poll_pending_e0 = 1; sc = kbd_pop(); if (sc < 0) return 0; }
        if (gui_poll_pending_e0) {
            gui_poll_pending_e0 = 0;
            if (sc == 0x48) { *a = KEY_UP; return 1; }
            if (sc == 0x50) { *a = KEY_DOWN; return 1; }
            if (sc == 0x4B) { *a = KEY_LEFT; return 1; }
            if (sc == 0x4D) { *a = KEY_RIGHT; return 1; }
            return 0;
        }
        if (sc & 0x80) return 0;
        gui_close_was_click = 0;
        if (kbd_ctrl) {
            int code = sc & 0x7F;
            if (code == 0x2E) { *a = KEY_COPY; return 1; }
            if (code == 0x2D) { *a = KEY_CUT; return 1; }
            if (code == 0x2F) { *a = KEY_PASTE; return 1; }
        }
        char c = kbd_map(sc);
        if (c == '\n') { *a = KEY_ENTER; return 1; }
        if (c == 27)   { *a = KEY_ESC; return 1; }
        if (c) { *a = (int)(unsigned char)c; return 1; }
        return 0;
    }
    if (mouse_click_edge()) { gui_close_was_click = 1; *a = app_cursor_x - app_view_x; *b = app_cursor_y - app_view_y; return 2; }
    int wheel = mouse_get_wheel();
    if (wheel) { *a = wheel > 0 ? 1 : -1; return 3; }
    return 0;
}
