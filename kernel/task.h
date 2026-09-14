#ifndef TASK_H
#define TASK_H
void tasks_init(void);
int  task_create(void (*entry)(void));
void yield(void);
void task_exit(void); /* frees the caller's own stack and never returns */
void sleep_ticks(unsigned int n); /* yield repeatedly until n PIT ticks (100/sec) pass */
#endif
