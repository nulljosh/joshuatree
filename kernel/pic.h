#ifndef PIC_H
#define PIC_H
void pic_remap(void);
void pic_eof(int irq); /* send end-of-interrupt for the given IRQ line */
/* v0.76.8: mask/unmask a single IRQ line at the PIC itself, so the line
   never reaches the CPU at all -- unlike cli/sti, this holds across a task
   switch regardless of the new task's own saved EFLAGS.IF, since each task
   restores its own IF via iret on resume. Needed to suspend IRQ0 (the timer)
   specifically, see its one real caller in kernel.c's tasktest. */
void pic_set_mask(int irq, int masked);
#endif
