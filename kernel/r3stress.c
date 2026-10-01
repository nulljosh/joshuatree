/* 1.9.23: the two halves of the ring-3 concurrency stress, armed by
   `stress=r3` on the kernel command line and otherwise dead code.

   The hazard it exercises: the desktop loop (task 0) runs with interrupts
   on and can be preempted anywhere, including inside kmalloc's free-list
   walk or a FAT write; the ring-3 window task the tick switches to then
   enters the same heap and the same filesystem through int 0x80. On the
   pre-lock kernel that is two walkers on one list and two writers on one
   cwd. So: the desktop half churns the heap and rewrites a file on every
   frame, the syscall half does the same inside SYS_WINDOW_POLL (which a
   windowed program calls in a tight loop), and after a fixed number of
   desktop rounds the heap is walked (kheap_check) and the verdict goes to
   serial for tools/checks/ring3stress-check.py to read. The desktop also
   stands inside NOTES/ for part of every round, so a syscall that forgot
   to resolve from the root would create its file in the wrong place. */
#include "kheap.h"
#include "vfs.h"
#include "serial.h"

extern int syscall_stress_on;
static unsigned int desk_rounds, desk_bad, sys_rounds, sys_bad;
#define DESK_ROUNDS 400
#define DESK_BURST 256

void r3stress_arm(const char *cl) {
    for (const char *p = cl; p && *p; p++)
        if (p[0]=='s' && p[1]=='t' && p[2]=='r' && p[3]=='e' && p[4]=='s' && p[5]=='s' && p[6]=='=' && p[7]=='r' && p[8]=='3') { syscall_stress_on = 1; serial_puts("r3stress: armed\n"); }
}

/* Called under the syscall gate by a ring-3 window task. */
void r3stress_syscall_round(void) {
    static char buf[64];
    unsigned int cwd = vfs_cwd_get(); vfs_cwd_set(0); /* the same rule syscall.c's path_enter applies: a syscall path starts at the root */
    sys_rounds++;
    if (!kheap_stress_round(sys_rounds * 7u + 3u, 1)) sys_bad++;
    for (int i = 0; i < 64; i++) buf[i] = (char)('a' + (sys_rounds + (unsigned)i) % 26);
    vfs_replace_file("R3SYS.TXT", buf, sizeof buf);
    char back[64];
    if (vfs_read_file("R3SYS.TXT", back, sizeof back) != 64) sys_bad++;
    vfs_cwd_set(cwd);
}

/* Called once per compositor frame by task 0, interrupts on. */
void r3stress_desktop_round(void) {
    if (!syscall_stress_on || desk_rounds > DESK_ROUNDS) return;
    desk_rounds++;
    static char buf[256];
    for (int i = 0; i < DESK_BURST; i++) if (!kheap_stress_round(desk_rounds * 131u + (unsigned)i, 0)) desk_bad++;
    vfs_mkdir("NOTES"); vfs_chdir("NOTES");
    for (int i = 0; i < 256; i++) buf[i] = (char)('A' + (desk_rounds + (unsigned)i) % 26);
    vfs_replace_file("R3DESK.TXT", buf, sizeof buf);
    for (int i = 0; i < DESK_BURST; i++) if (!kheap_stress_round(desk_rounds * 17u + (unsigned)i, 0)) desk_bad++;
    vfs_chdir("..");
    if (desk_rounds == DESK_ROUNDS) {
        serial_puts(sys_rounds ? "r3stress: syscall side ran\n" : "r3stress: syscall side never ran\n");
        serial_puts(desk_bad || sys_bad ? "r3stress: stamp mismatches seen\n" : "r3stress: no stamp mismatch\n");
        char probe[4];
        serial_puts(vfs_read_file("R3SYS.TXT", probe, sizeof probe) >= 0 ? "r3stress: R3SYS.TXT at root\n" : "r3stress: R3SYS.TXT missing from root\n");
        vfs_chdir("NOTES");
        serial_puts(vfs_read_file("R3SYS.TXT", probe, sizeof probe) >= 0 ? "r3stress: R3SYS.TXT leaked into NOTES\n" : "r3stress: NOTES clean\n");
        vfs_chdir("..");
        kheap_check();
        serial_puts("r3stress: done\n");
    }
}
