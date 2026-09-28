#ifndef RING3APP_H
#define RING3APP_H
/* 1.7.7: the first app that leaves the kernel. Keyrate's dock entry runs
   user/keyrate.c as a real ring-3 process instead of calling
   drivers/app_keyrate.c's in-kernel function. The launcher is also the
   supervisor: it waits for the process, and whether the program exited
   on its own or was reaped by idt.c's ring-3 fault path, it tears the
   window down and hands the desktop back. See ring3app.c. */
void keyrate_ring3_open(void);
#endif
