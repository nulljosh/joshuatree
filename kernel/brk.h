#ifndef BRK_H
#define BRK_H
/* 1.9.27: per-task heap growth, SYS_BRK. See brk.c. */
int  brk_set(int task, unsigned int dir_phys, unsigned int new_top); /* returns the top after the call, or -errno */
void brk_release(int task, unsigned int dir_phys);                   /* unmaps and frees every heap page the task holds */
unsigned int brk_live_pages(void);                                   /* heap pages mapped across all tasks, for the leak line */
#endif
