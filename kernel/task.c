/* Preemptive round-robin off the PIT tick (irq0), plus the same round-robin
   still reachable voluntarily via yield(). A task's saved esp always points
   at the exact stack shape irq0's asm stub produces mid-interrupt (pusha's
   8 registers, then the CPU's own EIP/CS/EFLAGS), whether it got there by a
   real timer tick or by yield()'s `int $32` triggering that same path in
   software, so one restore sequence (popa; iret) resumes either case.
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
   at the very end of the asm stub, after schedule() has already returned. */
#include "task.h"
#include "kheap.h"
#include "irq.h"

typedef unsigned int u32;

#define MAX_TASKS  6
#define STACK_SIZE 4096

struct task {
    u32 esp;
    void *stack_base;
    int used;
};

static struct task tasks[MAX_TASKS];
static int n_tasks = 0;
static int current = 0;

void tasks_init(void) {
    for (int i = 0; i < MAX_TASKS; i++) tasks[i].used = 0;
    tasks[0].used = 1;         /* the currently-running boot/shell stack */
    tasks[0].stack_base = 0;   /* not ours to free */
    n_tasks = 1;
    current = 0;
}

int task_create(void (*entry)(void)) {
    int id = -1;
    for (int i = 0; i < MAX_TASKS; i++) if (!tasks[i].used) { id = i; break; }
    if (id < 0) return -1;
    void *stack = kmalloc(STACK_SIZE);
    if (!stack) return -1;

    /* Fabricate a stack that looks exactly like a task irq0 already
       interrupted mid-pusha: EDI..EAX (popa's pop order, low to high),
       then EIP/CS/EFLAGS (iret's pop order). EFLAGS=0x202 keeps IF set,
       so interrupts, and with them preemption, stay on once this task
       actually starts running. */
    u32 *sp = (u32 *)((char *)stack + STACK_SIZE);
    *(--sp) = 0x202;        /* EFLAGS */
    *(--sp) = 0x08;         /* CS: kernel code selector */
    *(--sp) = (u32)entry;   /* EIP */
    *(--sp) = 0;            /* EAX */
    *(--sp) = 0;            /* ECX */
    *(--sp) = 0;            /* EDX */
    *(--sp) = 0;            /* EBX */
    *(--sp) = 0;            /* ESP placeholder, popa discards it */
    *(--sp) = 0;            /* EBP */
    *(--sp) = 0;            /* ESI */
    *(--sp) = 0;            /* EDI */

    tasks[id].esp = (u32)sp;
    tasks[id].stack_base = stack;
    tasks[id].used = 1;
    if (id >= n_tasks) n_tasks = id + 1; /* n_tasks is a high-water mark for the round-robin scan below, not a live count */
    return id;
}

/* Called from irq0's asm stub with the interrupted task's esp (already
   pointing at the pusha+CPU-frame it just built). Returns the esp to
   resume on: the next USED task in round-robin order (an exited task's
   freed slot is skipped, not scheduled into), or the same one unchanged
   if there's nothing else to run. */
u32 schedule(u32 esp) {
    tasks[current].esp = esp;
    int next = current;
    for (int i = 0; i < n_tasks; i++) {
        next = (next + 1) % n_tasks;
        if (tasks[next].used) break;
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

/* Frees the calling task's own stack and removes it from the round-robin
   permanently, then reschedules away and never returns. See this file's
   header comment for why freeing the stack it's still standing on is safe. */
void task_exit(void) {
    int id = current;
    if (tasks[id].stack_base) kfree(tasks[id].stack_base);
    tasks[id].stack_base = 0;
    tasks[id].used = 0;
    for (;;) __asm__ volatile ("int $32"); /* loop in case of a spurious extra resume; schedule() will never pick this slot again */
}

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
