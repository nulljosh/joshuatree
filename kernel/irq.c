/* IRQ0 (PIT timer) and IRQ1 (keyboard), plus a catch-all for anything else
   the PIC might raise. ponytail: only IRQ0/1 are unmasked in pic.c, so the
   other 14 stubs exist only so an unexpected line doesn't jump into garbage. */
#include "irq.h"
#include "pic.h"
#include "idt.h"
#include "mouse.h"

typedef unsigned int  u32;
typedef unsigned short u16;
typedef unsigned char u8;

static inline u8 inb(u16 p){ u8 v; __asm__ volatile("inb %1,%0":"=a"(v):"Nd"(p)); return v; }
static inline void outb(u16 p, u8 v){ __asm__ volatile("outb %0,%1"::"a"(v),"Nd"(p)); }

#define IRQ(n) extern void irq##n(void);
IRQ(0) IRQ(1) IRQ(2) IRQ(3) IRQ(4) IRQ(5) IRQ(6) IRQ(7)
IRQ(8) IRQ(9) IRQ(10) IRQ(11) IRQ(12) IRQ(13) IRQ(14) IRQ(15)
#undef IRQ

static volatile unsigned int tick_count = 0;

/* small ring buffer: IRQ1 writes, kernel.c's getch() reads via kbd_pop() */
#define KBD_BUF_SIZE 32
static volatile u8 kbd_buf[KBD_BUF_SIZE];
static volatile int kbd_head = 0, kbd_tail = 0;

void irq_handler(u32 irq_no) {
    if (irq_no == 0) {
        tick_count++;
    } else if (irq_no == 1) {
        u8 sc = inb(0x60);
        int next = (kbd_head + 1) % KBD_BUF_SIZE;
        if (next != kbd_tail) { kbd_buf[kbd_head] = sc; kbd_head = next; }
    } else if (irq_no == 12) {
        mouse_handle_byte(inb(0x60));
    }
    pic_eof((int)irq_no);
}

int kbd_pop(void) {
    if (kbd_head == kbd_tail) return -1;
    u8 sc = kbd_buf[kbd_tail];
    kbd_tail = (kbd_tail + 1) % KBD_BUF_SIZE;
    return sc;
}

unsigned int ticks(void) { return tick_count; }

/* PIT channel 0, mode 3 (square wave), reload for ~100Hz from the 1.193182MHz base. */
static void pit_init(unsigned int hz) {
    unsigned int divisor = 1193182 / hz;
    outb(0x43, 0x36);
    outb(0x40, divisor & 0xFF);
    outb(0x40, (divisor >> 8) & 0xFF);
}

void irq_install(void) {
    pic_remap();
    pit_init(100);
#define SET(n) idt_set_gate(32 + n, (u32)irq##n, 0x08, 0x8E)
    SET(0);  SET(1);  SET(2);  SET(3);  SET(4);  SET(5);  SET(6);  SET(7);
    SET(8);  SET(9);  SET(10); SET(11); SET(12); SET(13); SET(14); SET(15);
#undef SET
    __asm__ volatile ("sti");
}
