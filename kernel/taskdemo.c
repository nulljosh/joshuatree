/* The scheduler demos the shell runs (tasktest, fputest, preempttest, killtest, spawntest, isotest, reaptest) and the
 small tasks they start. Each proves one thing about task.c for a headless check reading the serial log. Moved out
 of kernel.c in 4.18.6; run() hands every line here after selftest_run. */
#include "libc.h"
#include "console.h"
#include "irq.h"
#include "pic.h"
#include "serial.h"
#include "paging.h"
#include "task.h"
#include "selftest.h"

/* ---- task demo: two tasks that each print a letter and yield, round-robin,
   to prove context switching actually swaps stacks correctly. Bounded, then
   task_exit() (v28): a task can't safely `return` (see task.c), but now
   that it can really exit instead of parking in `hlt` forever, running
   tasktest repeatedly no longer permanently burns 2 of the 6 task slots. ---- */
static void task_a(void){ for (int i = 0; i < 10; i++) { puts("A"); yield(); } task_exit(); }
static void task_b(void){ for (int i = 0; i < 10; i++) { puts("B"); yield(); } task_exit(); }

/* 2.2.0: fputest. Two tasks each push a value on the x87 stack, switch away inside the same asm, pop it back; crossed values mean schedule() lost per-task float state. */
static volatile int fpu_bad, fpu_done;
static void fpu_task(double mark) { for (int i = 0; i < 300; i++) { double in = mark + i, out = 0; __asm__ volatile ("fldl %1; int $32; fstpl %0" : "=m"(out) : "m"(in) : "memory"); if (out != in) fpu_bad++; } fpu_done++; task_exit(); }
static void fpu_a(void){ fpu_task(1000.5); } static void fpu_b(void){ fpu_task(7000.25); }

/* ---- preemption demo: two tasks that never call yield() or hlt, proving
   the timer itself forces a switch. The shell's own wait loop below also
   never yields/hlts on purpose, so if preemption weren't real this whole
   command would just spin, and neither counter would ever move.
   preempt_stop (v28) lets the shell actually reap these once it's done
   measuring, same reasoning as task_a/task_b above: without it these two
   would spin forever and permanently hold 2 of the 6 task slots. ---- */
static volatile int preempt_a_count = 0;
static volatile int preempt_b_count = 0;
static volatile int preempt_stop = 0;
static void preempt_task_a(void){ while (!preempt_stop) preempt_a_count++; task_exit(); }
static void preempt_task_b(void){ while (!preempt_stop) preempt_b_count++; task_exit(); }

/* ---- v0.88.0: spawntest. The Activity app (user/activity.c) shows and
   kills real scheduler tasks through the exact same task_used/task_kill
   primitives `ps`/`kill` already use, but nothing in the shell so far
   leaves a task running indefinitely for a test to observe from the GUI
   side -- every existing demo (task_a/b, preempt_task_a/b, iso_task_a/b)
   cleans itself up within a handful of yields. spawntest_task is the one
   deliberately long-lived task: it just spins until killed, real and
   idle, the same shape preempt_task_a already has minus the self-stop
   flag, so tools/checks/activity-check.py has a real, persistent task to
   select and kill through the app instead of a synthetic fixture. ---- */
static void spawntest_task(void){ for (;;) yield(); }

/* ---- v31 (0.31.0) isolation demo: two tasks write different markers to
   the SAME virtual address, PAGING_PRIVATE_VADDR. If page directories were
   still shared (the pre-v31 world), the second write would clobber the
   first and both readbacks would show 0xBBBBBBBB. Each task's own
   directory maps that address to its own private physical frame, so both
   readbacks should show what that task itself wrote, unaffected by the
   other task's write to the "same" address in between. ---- */
static volatile unsigned int iso_readback_a = 0, iso_readback_b = 0;
static void iso_task_a(void){
    *(volatile unsigned int *)PAGING_PRIVATE_VADDR = 0xAAAAAAAA;
    yield(); /* let task B run and write its own marker to the "same" address before we read ours back */
    iso_readback_a = *(volatile unsigned int *)PAGING_PRIVATE_VADDR;
    task_exit();
}
static void iso_task_b(void){
    *(volatile unsigned int *)PAGING_PRIVATE_VADDR = 0xBBBBBBBB;
    yield();
    iso_readback_b = *(volatile unsigned int *)PAGING_PRIVATE_VADDR;
    task_exit();
}

int taskdemo_run(const char *line) {
    if (0) {}
    else if (!strcmp(line, "fputest")) { fpu_bad = fpu_done = 0; task_create(fpu_a); task_create(fpu_b); for (int i = 0; i < 2000 && fpu_done < 2; i++) yield();
        puts(fpu_done == 2 && !fpu_bad ? "fputest: x87 state kept per task: ok\n" : fpu_done == 2 ? "fputest: FAIL x87 values crossed between tasks\n" : "fputest: FAIL tasks did not finish\n"); }
    else if (!strcmp(line, "tasktest")) {
        /* v0.76.8: real, reproduced-on-demand CI flake fixed at the root.
           yield()'s software `int $32` and the hardware PIT's own IRQ0 both
           land on the identical IDT gate and both call schedule() (see
           task.c's own comment on yield()), so this kernel has been truly,
           continuously preemptive in the background since whatever version
           first wired IRQ0 to it -- not just during preempttest's deliberate
           no-yield demo. This test's strict "ABABAB..." expectation is only
           true if NO hardware tick lands during its own tiny window, which
           held by luck on a fast/idle machine but not on a slower or
           shared CI runner: reproduced reliably here by temporarily
           reconfiguring the PIT to 5000Hz (irq_install's pit_init call),
           which corrupted the output on every single run (e.g.
           "ABABAABABABABABABABB"), then confirmed clean again after adding
           the mask below, even at that same artificially high rate; the
           real PIT rate (100Hz) was restored unchanged after the repro.
           Fix: mask IRQ0 at the PIC for the exact width of this test, so
           only the tasks' own explicit yield() calls drive scheduling here
           -- pic_set_mask holds across a task switch regardless of which
           task's own EFLAGS.IF is active (unlike cli/sti, which only
           affects the currently running task's own restored flags), unlike
           preempttest/killtest elsewhere, which still rely on real IRQ0
           ticks and are intentionally left untouched. */
        pic_set_mask(0, 1);
        puts("\n");
        task_create(task_a);
        task_create(task_b);
        for (int i = 0; i < 10; i++) yield(); /* shell is task 0; let A/B interleave */
        pic_set_mask(0, 0);
        puts("\ndone (expect ABABAB...)\n");
    }
    else if (!strcmp(line, "preempttest")) {
        preempt_a_count = 0; preempt_b_count = 0; preempt_stop = 0;
        int ida = task_create(preempt_task_a);
        int idb = task_create(preempt_task_b);
        if (ida < 0 || idb < 0) { puts("no free task slots (run fewer other task tests first)\n"); }
        else {
            unsigned int deadline = ticks() + 20; /* ~200ms real wall clock */
            while (ticks() < deadline) { } /* deliberately no yield()/hlt here */
            puts((preempt_a_count > 0 && preempt_b_count > 0) ? "preempted without yield: ok\n" : "no preemption (still cooperative-only)\n");
            preempt_stop = 1;
            for (int i = 0; i < 5; i++) yield(); /* let both tasks actually reach task_exit() and free their slots before returning */
        }
    }
    else if (!strcmp(line, "killtest")) {
        /* Real proof a killed task actually stops, not just that `kill`
           didn't crash anything: preempt_task_a runs forever incrementing
           a plain counter (same demo task preempttest already uses,
           reused rather than writing a third almost-identical one). Kill
           it mid-flight, let a few ticks pass, and confirm the counter
           genuinely stopped moving instead of merely slowing down. */
        preempt_a_count = 0; preempt_stop = 0;
        int id = task_create(preempt_task_a);
        if (id < 0) { puts("no free task slots\n"); }
        else {
            unsigned int warmup = ticks() + 5;
            while (ticks() < warmup) { }
            task_kill(id);
            for (int i = 0; i < 3; i++) yield(); /* let the scheduler actually resume it into task_exit() */
            int stopped_at = preempt_a_count;
            unsigned int settle = ticks() + 10;
            while (ticks() < settle) { }
            puts(preempt_a_count == stopped_at ? "kill: task really stopped: ok\n" : "kill: FAILED (counter kept moving)\n");
        }
    }
    else if (!strcmp(line, "spawntest")) {
        /* Real, persistent task for tools/checks/activity-check.py: prints
           its own slot id over serial so the script can compute which
           Activity row to select, the same "serial marker for a headless
           check" idiom the ring-3 apps' own marker lines use. */
        int id = task_create(spawntest_task);
        if (id < 0) puts("no free task slots\n");
        else {
            char buf[16]; int n = 0; unsigned int v = (unsigned int)id;
            char tmp[6]; int ti = 0; if (v==0) tmp[ti++]='0'; while (v) { tmp[ti++]='0'+v%10; v/=10; }
            while (ti) buf[n++] = tmp[--ti];
            buf[n] = 0;
            serial_puts("spawntest id="); serial_puts(buf); serial_puts("\n");
            puts("spawned task "); puts(buf); puts(" (runs until killed)\n");
        }
    }
    else if (!strcmp(line, "isotest")) {
        iso_readback_a = 0; iso_readback_b = 0;
        int ida = task_create(iso_task_a);
        int idb = task_create(iso_task_b);
        if (ida < 0 || idb < 0) { puts("no free task slots\n"); }
        else {
            for (int i = 0; i < 6; i++) yield(); /* let both tasks actually finish and reap themselves */
            int ok = (iso_readback_a == 0xAAAAAAAA) && (iso_readback_b == 0xBBBBBBBB);
            puts(ok ? "isolation: separate address spaces confirmed: ok\n" : "isolation: FAILED (one task saw the other's write)\n");
        }
    }
    else if (!strcmp(line, "reaptest")) {
        /* Real proof task_exit() actually frees its slot, not just that the
           kernel doesn't hang: exhaust every slot, confirm the next create
           fails, exit one, confirm a create then succeeds and reuses that
           exact slot id, not a coincidence, the same id every time since
           task_create always takes the lowest free slot. */
        int ids[16]; /* well above task.c's real MAX_TASKS, just a safe bound for this loop */
        int n = 0;
        while (n < 16) { int id = task_create(task_exit); if (id < 0) break; ids[n++] = id; }
        int full = (task_create(task_exit) < 0); /* every slot taken, including task 0 (the shell), so this must fail */
        for (int i = 0; i < 5; i++) yield(); /* let the throwaway tasks actually reach task_exit() */
        int reused = task_create(task_a);
        int ok = full && reused == ids[0] && reused >= 0; /* task_create always takes the lowest free slot, so the first slot handed out above is the first one reused */
        puts(ok ? "reap: freed slot really reused: ok\n" : "reap: FAILED\n");
        for (int i = 0; i < 12; i++) yield(); /* drain task_a's own A-printing + exit so the prompt doesn't land mid-output */
    }
    else return 0;
    return 1;
}
