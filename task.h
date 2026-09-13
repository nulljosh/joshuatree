#ifndef TASK_H
#define TASK_H
void tasks_init(void);
int  task_create(void (*entry)(void));
void yield(void);
#endif
