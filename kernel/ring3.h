#ifndef RING3_H
#define RING3_H
/* One-shot ring-3 user-mode demo: copies a tiny hand-written routine onto a
   user-accessible page, drops the CPU to ring 3 to run it, and never
   returns. The routine writes a known marker (proof it really executed as
   ring-3 code), then attempts a privileged instruction, which faults
   straight into idt.c's existing general-protection handler and halts.
   There's no process kill/reap yet (see task.c's own note on the same
   gap), so this ends the interactive session; reboot to get the shell
   back. */
void ring3_test(void);
#endif
