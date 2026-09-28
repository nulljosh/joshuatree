/* 1.7.7: Keyrate as a real ring-3 process, and the supervisor around it.

   Roadmap 2.0 says apps leave the kernel, so a crash in one cannot take
   the machine down. This is step one of that: exactly one app, Keyrate,
   the smallest real one, moved out of ring 0. The dock entry lands here,
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

   The binary rides along in kernel.elf (drivers/user_keyrate.h, generated
   by the Makefile from the real built user/keyrate.bin) and is seeded onto
   the VFS on first launch, the same way `usertest` seeds HELLO.BIN: the
   headless checks and the browser demo have no disk. */
#include "ring3app.h"
#include "exec.h"
#include "syscall.h"
#include "task.h"
#include "vfs.h"
#include "serial.h"
#include "user_keyrate.h"

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

void keyrate_ring3_open(void) {
    unsigned char probe[1];
    if (vfs_read_file("KEYRATE.BIN", probe, 1) < 0 &&
        !vfs_write_file("KEYRATE.BIN", user_keyrate, USER_KEYRATE_LEN)) {
        serial_puts("ring3app: could not seed KEYRATE.BIN, not started\n");
        return;
    }
    serial_puts("ring3app: launching KEYRATE.BIN at ring 3\n");
    int status = -1;
    const char *argv[] = { "KEYRATE.BIN" };
    if (!exec_user("KEYRATE.BIN", argv, 1, &status)) {
        serial_puts("ring3app: exec_user failed (not found, too big, or no free task slot)\n");
        return;
    }
    /* exec_user returned, so the task slot is free and syscall_release_task
       has already run for it. A negative status is idt.c's -(vector): the
       program crashed and was reaped. Zero or positive is its own exit
       code. Either way the desktop is what comes next. */
    char num[12]; put_dec(num, status);
    if (status < 0 && -status < 32) {
        serial_puts("ring3app: KEYRATE.BIN crashed ("); serial_puts(EXC_SHORT[-status]);
        serial_puts("), window torn down, desktop alive\n");
    } else {
        serial_puts("ring3app: KEYRATE.BIN exited "); serial_puts(num); serial_puts(", window torn down, desktop alive\n");
    }
    if (syscall_window_owner() >= 0) serial_puts("ring3app: BUG window still owned after the task ended\n");
}
