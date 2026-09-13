#ifndef PIC_H
#define PIC_H
void pic_remap(void);
void pic_eof(int irq); /* send end-of-interrupt for the given IRQ line */
#endif
