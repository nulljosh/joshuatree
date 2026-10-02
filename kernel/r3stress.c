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
#include "task.h"
#include "paging.h"

extern int syscall_stress_on;
static unsigned int desk_rounds, desk_bad, sys_rounds, sys_bad;

/* 1.9.24: `stress=pde`, the kernel-PDE sync proof (kernel/paging.c,
   paging_sync_task_dirs). Once the ring-3 window is up the desktop grows
   the heap in 4 KB steps until a block lands in a 4 MB region that had no
   page table when the task was created, then hands that block's address
   to the syscall side, which writes and reads it on the task's own CR3.
   Before the sync that read page-faulted at ring 0. */
int pde_stress_on;
static volatile unsigned int *pde_block;
static unsigned int pde_first, pde_done, pde_sys_hits;
#define PDE_GROW_CAP 3072 /* 3072 * 4 KB = 12 MB, enough to cross a 4 MB line from anywhere */
#define DESK_ROUNDS 250
#define DESK_BURST 128

extern int shell_crash_armed;
void r3stress_arm(const char *cl) {
    for (const char *p = cl; p && *p; p++)
        if (p[0]=='p' && p[1]=='a' && p[2]=='n' && p[3]=='i' && p[4]=='c' && p[5]=='d' && p[6]=='e' && p[7]=='s' && p[8]=='k') { shell_crash_armed = 1; serial_puts("panicdesk: armed\n"); }
    for (const char *p = cl; p && *p; p++)
        if (p[0]=='s' && p[1]=='t' && p[2]=='r' && p[3]=='e' && p[4]=='s' && p[5]=='s' && p[6]=='=' && p[7]=='r' && p[8]=='3') { syscall_stress_on = 1; serial_puts("r3stress: armed\n"); }
    for (const char *p = cl; p && *p; p++)
        if (p[0]=='s' && p[1]=='t' && p[2]=='r' && p[3]=='e' && p[4]=='s' && p[5]=='s' && p[6]=='=' && p[7]=='p' && p[8]=='d' && p[9]=='e') { pde_stress_on = 1; serial_puts("pdestress: armed\n"); }
}

/* Under the syscall gate, on the ring-3 task's CR3: touch the block the
   desktop placed in the freshly mapped region. */
static unsigned int pde_task_seen;
void pdestress_syscall_round(void) {
    pde_task_seen = 1; /* a ring-3 window task is live and polling: only grow after this, so the new table is born after its directory */
    if (!pde_block || pde_done) return;
    pde_block[0] = 0x5A5A0000u + pde_sys_hits;
    if (pde_block[0] != 0x5A5A0000u + pde_sys_hits) { serial_puts("pdestress: readback mismatch\n"); }
    pde_sys_hits++;
    if (pde_sys_hits == 1) serial_puts("pdestress: syscall touched the new region\n");
}

/* Once per frame, task 0, interrupts on: grow the heap past a 4 MB line
   after a ring-3 task exists, then wait for the syscall side to touch it. */
void pdestress_desktop_round(void) {
    if (!pde_stress_on || pde_done) return;
    if (!pde_task_seen) return;
    if (!pde_block) {
        unsigned int *probe = kmalloc(16);
        if (!probe) { serial_puts("pdestress: first kmalloc failed\n"); pde_done = 1; return; }
        pde_first = paging_pde_change_count(); (void)probe;
        for (unsigned int i = 0; i < PDE_GROW_CAP; i++) {
            unsigned int *b = kmalloc(4096 - 32);
            if (!b) { serial_puts("pdestress: heap growth stopped early\n"); pde_done = 1; return; }
            if (paging_pde_change_count() != pde_first) { /* grow_heap just created a kernel page table this task never saw at birth */
                pde_block = b;
                serial_puts("pdestress: heap crossed into a new 4 MB region\n");
                if (paging_check_task_dirs()) serial_puts("pdestress: task directories drifted\n");
                else serial_puts("pdestress: task directories in sync\n");
                return;
            }
        }
        serial_puts("pdestress: never crossed a 4 MB line\n"); pde_done = 1; return;
    }
    if (pde_sys_hits >= 8) { pde_done = 1; serial_puts("pdestress: done\n"); }
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
