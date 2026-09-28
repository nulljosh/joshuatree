#ifndef IRQ_H
#define IRQ_H
void irq_install(void);          /* remap PIC, register IRQ0/IRQ1 in the IDT */
int  kbd_pop(void);              /* next raw scancode, or -1 if none waiting */
int  kbd_peek(void);             /* same as kbd_pop but leaves it queued; a caller can decide whether a scancode is "for it" without stealing one some other handler needed this same frame */
extern int kbd_shift, kbd_caps, kbd_ctrl, kbd_alt;  /* modifier state kbd_pop keeps up to date */
void kbd_drain(void);            /* discards anything already queued; call once, right before the shell starts reading real input */
void kbd_inject(unsigned char sc); /* v1.8.0: push a synthetic scancode as if a real key came in -- phone_home.h's back-chevron uses this to send the ESC make code (0x01) so a tap closes an open app through the exact same kbd_pop()==27 path a keyboard's Esc already does, not a second close mechanism */
unsigned int ticks(void);        /* PIT tick count since boot */
#endif
