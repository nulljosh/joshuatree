/* Preemptive round-robin off the PIT tick (irq0), plus the same round-robin
   still reachable voluntarily via yield(). A task's saved esp always points
   at the exact stack shape irq0's asm stub produces mid-interrupt (pusha's
   8 registers, then the CPU's own EIP/CS/EFLAGS), whether it got there by a
   real timer tick or by yield()'s `int $32` triggering that same path in
   software, so one restore sequence (popa; iret) resumes either case.
   ponytail: no exit()/reap -- a task that returns from its entry function
   still corrupts whatever real stack frame `ret` lands on next, so a task
   that's done still has to loop forever (hlt is fine, it's silent and
   nearly free) rather than actually return. */
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
    if (n_tasks >= MAX_TASKS) return -1;
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

    int id = n_tasks++;
    tasks[id].esp = (u32)sp;
    tasks[id].stack_base = stack;
    tasks[id].used = 1;
    return id;
}

/* Called from irq0's asm stub with the interrupted task's esp (already
   pointing at the pusha+CPU-frame it just built). Returns the esp to
   resume on: the next task's in round-robin order, or the same one
   unchanged if there's nothing else to run. */
u32 schedule(u32 esp) {
    if (n_tasks < 2) return esp;
    tasks[current].esp = esp;
    current = (current + 1) % n_tasks;
    return tasks[current].esp;
}

void yield(void) {
    if (n_tasks < 2) return;
    __asm__ volatile ("int $32"); /* same IDT gate as the hardware timer: identical frame, identical schedule() */
}

/* ponytail: no sleep queue, this task keeps taking its round-robin turn and
   just re-checks the clock each time -- fine for a handful of cooperative
   tasks, wasteful for many. Add a real timer-ordered wait queue if that
   ever matters. */
void sleep_ticks(unsigned int n) {
    unsigned int start = ticks();
    while (ticks() - start < n) {
        if (n_tasks < 2) { __asm__ volatile ("hlt"); continue; }
        yield();
    }
}
