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

/* ring buffer: IRQ1 writes, kernel.c's getch() reads via kbd_pop().
   512 scancodes, about 250 keystrokes. It was 32 (16 keystrokes), and a full
   ring drops the scancode silently: Notes repaints its whole growing line on
   every key, which under v86 is slower than a typist (or the landing tour's
   55 ms per character), so halfway through a sentence letters went missing
   ("and a dozen real apps" arrived as "ad a oen ral ap"). Half a kilobyte
   buys enough slack that input lags behind a slow repaint instead of being
   lost. Must stay a power of two only by preference, the modulo is general. */
#define KBD_BUF_SIZE 512
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

/* Modifier state, tracked here at the one place every scancode passes
   through, so Shift and Caps Lock work in every app, not only in Notes
   (which decoded them itself). Left/right Shift make 0x2A/0x36, break
   0xAA/0xB6; Caps Lock make 0x3A toggles. kbd_map() applies them. */
int kbd_shift = 0, kbd_caps = 0;

int kbd_pop(void) {
    if (kbd_head == kbd_tail) return -1;
    u8 sc = kbd_buf[kbd_tail];
    kbd_tail = (kbd_tail + 1) % KBD_BUF_SIZE;
    if (sc == 0x2A || sc == 0x36) kbd_shift = 1;
    else if (sc == 0xAA || sc == 0xB6) kbd_shift = 0;
    else if (sc == 0x3A) kbd_caps = !kbd_caps;
    return sc;
}

void kbd_drain(void) {
    while (kbd_pop() >= 0) {}
}

unsigned int ticks(void) { return tick_count; }

/* PIT channel 0, mode 3 (square wave), reload for ~100Hz from the 1.193182MHz base. */
static void pit_init(unsigned int hz) {
    unsigned int divisor = 1193182 / hz;
    outb(0x43, 0x36);
    outb(0x40, divisor & 0xFF);
    outb(0x40, (divisor >> 8) & 0xFF);
}

/* Real hardware and QEMU both inherit a keyboard that's already scanning
   from their own BIOS's POST sequence, so this kernel never had to enable
   it itself, exactly the same class of gap the VGA text-mode init fix
   closed. A BIOS-less multiboot path (v86) leaves the keyboard's internal
   scan-enable flag off by default: confirmed by direct inspection of a
   live v86 instance (`ps2.enable_keyboard_stream === false`), the reason
   real keydown events never reached this kernel's IRQ1 handler even
   though v86 itself was receiving them fine. 0xF4 written straight to the
   data port (0x60), no 0xD4 mouse-redirect prefix, targets the keyboard
   channel specifically. */
static void keyboard_enable_scanning(void) {
    while (inb(0x64) & 0x02) {} /* wait for the controller's input buffer to be clear */
    outb(0x60, 0xF4);
    unsigned int timeout = 100000;
    while (timeout-- && !(inb(0x64) & 0x01)) {} /* wait for the ACK to land in the output buffer */
    /* Drain everything sitting in the output buffer, not just one byte: a
       real, found-by-testing bug here was a single stray byte still queued
       after just consuming the ACK, which the IRQ1 handler then picked up
       once interrupts turned on and got misread as a real keypress. */
    while (inb(0x64) & 0x01) (void)inb(0x60);
}

void irq_install(void) {
    pic_remap();
    pit_init(100);
#define SET(n) idt_set_gate(32 + n, (u32)irq##n, 0x08, 0x8E)
    SET(0);  SET(1);  SET(2);  SET(3);  SET(4);  SET(5);  SET(6);  SET(7);
    SET(8);  SET(9);  SET(10); SET(11); SET(12); SET(13); SET(14); SET(15);
#undef SET
    keyboard_enable_scanning();
    __asm__ volatile ("sti");
}
