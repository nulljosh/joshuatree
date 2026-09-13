/* 8259 PIC: remap IRQ0-15 off the CPU exception vectors (0-31) onto 32-47,
   since the BIOS default overlaps them and would misfire as exceptions. */
#include "pic.h"

typedef unsigned char u8;
typedef unsigned short u16;

static inline void outb(u16 p, u8 v){ __asm__ volatile("outb %0,%1"::"a"(v),"Nd"(p)); }
static inline void io_wait(void){ outb(0x80, 0); } /* write to an unused port to burn a cycle */

#define PIC1 0x20
#define PIC2 0xA0
#define PIC1_DATA 0x21
#define PIC2_DATA 0xA1

void pic_remap(void) {
    outb(PIC1, 0x11); io_wait();       /* ICW1: init, expect ICW4 */
    outb(PIC2, 0x11); io_wait();
    outb(PIC1_DATA, 0x20); io_wait();  /* ICW2: master offset 32 */
    outb(PIC2_DATA, 0x28); io_wait();  /* ICW2: slave offset 40 */
    outb(PIC1_DATA, 0x04); io_wait();  /* ICW3: slave on IRQ2 */
    outb(PIC2_DATA, 0x02); io_wait();
    outb(PIC1_DATA, 0x01); io_wait();  /* ICW4: 8086 mode */
    outb(PIC2_DATA, 0x01); io_wait();

    /* mask everything except IRQ0 (timer), IRQ1 (keyboard), IRQ2 (the
       cascade line itself, must stay unmasked or no slave-PIC IRQ, like
       the mouse's IRQ12, can ever reach the CPU), and IRQ12 (mouse) */
    outb(PIC1_DATA, 0xF8);
    outb(PIC2_DATA, 0xEF);
}

void pic_eof(int irq) {
    if (irq >= 8) outb(PIC2, 0x20);
    outb(PIC1, 0x20);
}
