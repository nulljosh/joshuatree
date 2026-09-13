/* Cooperative round-robin: a task only ever switches away by calling
   yield() itself. ponytail: no preemption from the PIT tick yet (that's
   the hard, easy-to-get-subtly-wrong half of "real" multitasking) and no
   exit()/reap -- a task that returns from its entry function just corrupts
   the next yield's return address, so don't let one return. Add both once
   this cooperative half is proven solid. */
#include "task.h"
#include "kheap.h"

typedef unsigned int u32;

#define MAX_TASKS  4
#define STACK_SIZE 4096

struct task {
    u32 esp;
    void *stack_base;
    int used;
};

static struct task tasks[MAX_TASKS];
static int n_tasks = 0;
static int current = 0;

extern void switch_context(u32 *old_esp, u32 new_esp);

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

    u32 *sp = (u32 *)((char *)stack + STACK_SIZE);
    *(--sp) = (u32)entry; /* "return address" switch_context's ret lands on */
    *(--sp) = 0; /* ebp */
    *(--sp) = 0; /* ebx */
    *(--sp) = 0; /* esi */
    *(--sp) = 0; /* edi */

    int id = n_tasks++;
    tasks[id].esp = (u32)sp;
    tasks[id].stack_base = stack;
    tasks[id].used = 1;
    return id;
}

void yield(void) {
    if (n_tasks < 2) return;
    int prev = current;
    current = (current + 1) % n_tasks;
    switch_context(&tasks[prev].esp, tasks[current].esp);
}
