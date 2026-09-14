/* Preemptive round-robin off the PIT tick (irq0), plus the same round-robin
   still reachable voluntarily via yield(). A task's saved esp always points
   at the exact stack shape irq0's asm stub produces mid-interrupt (the 4
   data segments, pusha's 8 registers, then the CPU's own EIP/CS/EFLAGS,
   plus user ESP/SS when the task was in ring 3), whether it got there by
   a real timer tick or by yield()'s `int $32` triggering that same path in
   software, so one restore sequence (pop segments; popa; iret) resumes
   either case.
   v28: real exit()/reap. A task still can't just `return` from its entry
   function (`ret` would land on whatever garbage is above the fabricated
   frame), it must call task_exit() instead, which frees its own stack via
   kfree() then immediately reschedules away via the same `int $32` gate
   yield() uses. Freeing a stack while still executing on it is safe here:
   kfree() only edits the kheap's free-list metadata, never unmaps or
   scribbles the memory itself, and the one transient use afterward (the
   int $32 trap frame, and schedule()'s own C call frame) is nothing but
   more pushes onto memory nobody else can race to reallocate before this
   single-threaded scheduler actually switches ESP over to the next task
   at the very end of the asm stub, after schedule() has already returned.
   v64 (0.61.0): ring-3 tasks are real scheduled tasks. The kmalloc'd
   STACK_SIZE block is a ring-3 task's KERNEL stack (its user stack is
   whatever page the caller hands task_create_user), the one the CPU
   switches to on every int 0x80/tick/fault out of ring 3 via the TSS's
   esp0, which schedule() repoints at the next task's block on every
   switch. That's the whole reason the block exists for a ring-3 task:
   a frame saved there by a tick is what popa/iret resume from later. */
#include "task.h"
#include "kheap.h"
#include "irq.h"
#include "paging.h"
#include "gdt.h"

typedef unsigned int u32;

#define MAX_TASKS  6
#define STACK_SIZE 4096

/* Saved-frame layout, u32 indices from the saved esp upward. Exactly what
   irq0 (irq_stubs.S) pushes, in reverse: pusha, then ds/es/fs/gs. */
#define F_GS 0
#define F_FS 1
#define F_ES 2
#define F_DS 3
#define F_EDI 4
#define F_EAX 11
#define F_EIP 12
#define F_CS 13
#define F_EFLAGS 14
#define F_USERESP 15  /* ring-3 frames only */
#define F_SS 16       /* ring-3 frames only */

#define KCODE 0x08
#define KDATA 0x10
#define UCODE 0x1B /* ring-3 code selector | RPL 3 */
#define UDATA 0x23 /* ring-3 data selector | RPL 3 */

struct task {
    u32 esp;
    void *stack_base;
    u32 page_dir; /* v31 (0.31.0): physical addr of this task's own page directory, loaded into CR3 on switch */
    int used;
};

static struct task tasks[MAX_TASKS];
static int n_tasks = 0;
static int current = 0;
static int last_exit_code = 0;

void tasks_init(void) {
    for (int i = 0; i < MAX_TASKS; i++) tasks[i].used = 0;
    tasks[0].used = 1;         /* the currently-running boot/shell stack */
    tasks[0].stack_base = 0;   /* not ours to free */
    tasks[0].page_dir = paging_kernel_directory(); /* the shell keeps running on the kernel's own shared directory, no isolation of its own */
    n_tasks = 1;
    current = 0;
}

/* Fabricate a kernel stack that looks exactly like a task irq0 already
   interrupted: from the top down, the CPU's own frame (SS/ESP only when
   returning to ring 3), then EAX..EDI in pusha's push order, then
   DS/ES/FS/GS. EFLAGS=0x202 keeps IF set (IOPL 0), so interrupts, and
   with them preemption, stay on once this task actually starts running;
   a ring-3 task additionally gets IOPL 0's guarantee that any in/out it
   tries is a #GP. Returns the slot id or -1. */
static int task_create_frame(u32 eip, u32 cs, u32 data_sel, int user, u32 user_esp) {
    int id = -1;
    for (int i = 0; i < MAX_TASKS; i++) if (!tasks[i].used) { id = i; break; }
    if (id < 0) return -1;
    void *stack = kmalloc(STACK_SIZE);
    if (!stack) return -1;

    u32 *sp = (u32 *)((char *)stack + STACK_SIZE);
    if (user) {
        *(--sp) = UDATA;      /* SS */
        *(--sp) = user_esp;   /* ESP: the user stack, a page the caller already marked user-accessible */
    }
    *(--sp) = 0x202;        /* EFLAGS */
    *(--sp) = cs;           /* CS */
    *(--sp) = eip;          /* EIP */
    for (int i = 0; i < 8; i++) *(--sp) = 0; /* EAX ECX EDX EBX ESP(placeholder) EBP ESI EDI, all zero */
    *(--sp) = data_sel;     /* DS */
    *(--sp) = data_sel;     /* ES */
    *(--sp) = data_sel;     /* FS */
    *(--sp) = data_sel;     /* GS */

    tasks[id].esp = (u32)sp;
    tasks[id].stack_base = stack;
    u32 dir = paging_new_task_directory();
    if (!dir) { kfree(stack); return -1; } /* real OOM, not swallowed: no isolated directory means no task */
    tasks[id].page_dir = dir;
    tasks[id].used = 1;
    if (id >= n_tasks) n_tasks = id + 1; /* n_tasks is a high-water mark for the round-robin scan below, not a live count */
    return id;
}

int task_create(void (*entry)(void)) {
    return task_create_frame((u32)entry, KCODE, KDATA, 0, 0);
}

int task_create_user(u32 entry, u32 user_esp) {
    return task_create_frame(entry, UCODE, UDATA, 1, user_esp);
}

/* Called from irq0's asm stub with the interrupted task's esp (already
   pointing at the segments+pusha+CPU-frame it just built). Returns the
   esp to resume on: the next USED task in round-robin order (an exited
   task's freed slot is skipped, not scheduled into), or the same one
   unchanged if there's nothing else to run. */
u32 schedule(u32 esp) {
    tasks[current].esp = esp;
    int next = current;
    for (int i = 0; i < n_tasks; i++) {
        next = (next + 1) % n_tasks;
        if (tasks[next].used) break;
    }
    if (next != current) {
        paging_load_directory(tasks[next].page_dir); /* v31 (0.31.0): the actual switch of address space, not just stacks */
        /* v64: where the CPU lands the next time THIS task traps out of
           ring 3. Its kernel stack's top is free by then: every entry
           into the kernel from ring 3 starts at the top, and every exit
           back to ring 3 (iret) has unwound it completely. Harmless for
           a ring-0 task (no privilege change, no stack switch). */
        gdt_set_kernel_stack(tasks[next].stack_base ? (u32)tasks[next].stack_base + STACK_SIZE : 0);
    }
    current = next;
    return tasks[current].esp;
}

static int any_other_used(void) {
    for (int i = 0; i < n_tasks; i++) if (i != current && tasks[i].used) return 1;
    return 0;
}

void yield(void) {
    if (!any_other_used()) return;
    __asm__ volatile ("int $32"); /* same IDT gate as the hardware timer: identical frame, identical schedule() */
}

int task_max(void) { return n_tasks; }
int task_used(int id) { return (id >= 0 && id < MAX_TASKS) ? tasks[id].used : 0; }
int task_current(void) { return current; }
int task_last_exit_code(void) { return last_exit_code; }

void task_kill(int id) {
    if (id < 0 || id >= MAX_TASKS || !tasks[id].used || id == current) return;
    /* Patches the saved EIP in that task's own suspended stack frame to
       point at task_exit instead of wherever it actually was, so the next
       time schedule() resumes it, it runs task_exit() on its own real
       stack/context instead of continuing whatever it was doing. Safe
       specifically because id != current was just checked: every OTHER
       used task is, by definition, suspended right now with a valid saved
       frame sitting in memory in exactly the shape task_create_frame()
       documents (F_* above).
       v64: a ring-3 task's frame says CS=0x1B, and task_exit is kernel
       code on a supervisor-only page, so resuming there at ring 3 would
       be a page fault, not an exit (it would still get reaped, via the
       exception path, but by accident). Retarget the frame to ring 0
       instead: kernel CS and data selectors, so popa/iret performs a
       same-privilege return straight into task_exit on the kernel stack
       it's already on. The now-unused user ESP/SS words above the frame
       are just dead stack, and task_exit never returns to care. */
    u32 *frame = (u32 *)tasks[id].esp;
    frame[F_EIP] = (u32)task_exit;
    if ((frame[F_CS] & 3) == 3) {
        frame[F_CS] = KCODE;
        frame[F_DS] = frame[F_ES] = frame[F_FS] = frame[F_GS] = KDATA;
    }
}

/* Frees the calling task's own stack and removes it from the round-robin
   permanently, then reschedules away and never returns. See this file's
   header comment for why freeing the stack it's still standing on is safe. */
void task_exit_with(int code) {
    /* v31 (0.31.0): cli across the free-then-switch window, a real race
       this file didn't close before: a timer tick landing between
       kfree(stack)/paging_free_task_directory() and this task's own
       int $32 would preempt into another task that could kmalloc or
       task_create its way into reusing these just-freed physical frames
       while this task is still nominally suspended "on" them (its own
       saved register state, mid-call-frame locals, and CR3 all still
       point there until the switch actually happens). `int $32` is a
       software trap, not maskable by IF, so it still fires with
       interrupts off; nothing else can run in between. */
    __asm__ volatile ("cli");
    int id = current;
    last_exit_code = code;
    if (tasks[id].stack_base) kfree(tasks[id].stack_base);
    if (tasks[id].page_dir && tasks[id].page_dir != paging_kernel_directory()) paging_free_task_directory(tasks[id].page_dir);
    tasks[id].stack_base = 0;
    tasks[id].page_dir = 0;
    tasks[id].used = 0;
    for (;;) __asm__ volatile ("int $32"); /* loop in case of a spurious extra resume; schedule() will never pick this slot again */
}

void task_exit(void) { task_exit_with(0); }

/* ponytail: no sleep queue, this task keeps taking its round-robin turn and
   just re-checks the clock each time -- fine for a handful of cooperative
   tasks, wasteful for many. Add a real timer-ordered wait queue if that
   ever matters. */
void sleep_ticks(unsigned int n) {
    unsigned int start = ticks();
    while (ticks() - start < n) {
        if (!any_other_used()) { __asm__ volatile ("hlt"); continue; }
        yield();
    }
}
