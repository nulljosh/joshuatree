#ifndef TASK_H
#define TASK_H

/* How many task slots exist, total. Exported (task.c's MAX_TASKS is
   defined from it) because syscall.c keeps a per-task file-descriptor
   table indexed by slot id and has to be exactly as wide as this. */
#define TASK_SLOTS 6

void tasks_init(void);
int  task_create(void (*entry)(void));
void yield(void);
void task_exit(void); /* frees the caller's own stack and never returns */

/* v64 (0.61.0): ring-3 tasks in the same scheduler. `entry` and `user_esp`
   are addresses on pages the caller already made user-accessible
   (paging_set_user); the task starts at CPL 3 with IOPL 0 and its own
   kmalloc'd kernel stack for traps. It ends by SYS_EXIT, by faulting
   (reaped by idt.c's ring-3 path), or by task_kill(), never by falling off
   the end of its code. Returns the slot id or -1. */
int  task_create_user(unsigned int entry, unsigned int user_esp);
void task_exit_with(int code); /* task_exit with a recorded status, what SYS_EXIT and the fault path call */
int  task_last_exit_code(void); /* status recorded by the most recent task_exit_with, for whoever waited on it */
int  task_current(void);        /* slot id of the running task; 0 is the shell */

/* v32 (0.32.0): a real minimal signal, SIGKILL-equivalent only. Can't
   target the caller's own currently-running task (that's what
   task_exit() is for) or an unused/out-of-range slot; both are silent
   no-ops. Takes effect the next time the scheduler would resume that
   task, not immediately, that IS "the scheduler checks between quanta",
   the real mechanism, not a simplification of one. Works on ring-3 tasks
   too as of v64 (the frame is retargeted to ring 0 first, see task.c). */
void task_kill(int id);
int  task_max(void);   /* one past the highest slot index ever handed out */
int  task_used(int id); /* 1 if that slot is a live task right now */
void sleep_ticks(unsigned int n); /* yield repeatedly until n PIT ticks (100/sec) pass */
#endif
