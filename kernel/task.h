#ifndef TASK_H
#define TASK_H
void tasks_init(void);
int  task_create(void (*entry)(void));
void yield(void);
void task_exit(void); /* frees the caller's own stack and never returns */

/* v32 (0.32.0): a real minimal signal, SIGKILL-equivalent only. Can't
   target the caller's own currently-running task (that's what
   task_exit() is for) or an unused/out-of-range slot; both are silent
   no-ops. Takes effect the next time the scheduler would resume that
   task, not immediately, that IS "the scheduler checks between quanta",
   the real mechanism, not a simplification of one. */
void task_kill(int id);
int  task_max(void);   /* one past the highest slot index ever handed out */
int  task_used(int id); /* 1 if that slot is a live task right now */
void sleep_ticks(unsigned int n); /* yield repeatedly until n PIT ticks (100/sec) pass */
#endif
