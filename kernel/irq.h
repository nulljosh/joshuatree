#ifndef IRQ_H
#define IRQ_H
void irq_install(void);          /* remap PIC, register IRQ0/IRQ1 in the IDT */
int  kbd_pop(void);              /* next raw scancode, or -1 if none waiting */
extern int kbd_shift, kbd_caps;  /* modifier state kbd_pop keeps up to date */
void kbd_drain(void);            /* discards anything already queued; call once, right before the shell starts reading real input */
unsigned int ticks(void);        /* PIT tick count since boot */
#endif
