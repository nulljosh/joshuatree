/* Minimal polling COM1 driver, debug output only: a real, live boot trace
   readable with plain `-serial stdio`, no timing guesswork like a
   screendump needs and immune to the VGA framebuffer's own state, so it
   still shows the last thing logged even if a crash resets the display
   back to text mode. Not wired into any user-facing command, klog() below
   pushes every entry here too. */
#include "serial.h"

typedef unsigned char  u8;
typedef unsigned short u16;

#define COM1 0x3F8

static inline u8   inb(u16 port)        { u8 v; __asm__ volatile ("inb %1,%0" : "=a"(v) : "Nd"(port)); return v; }
static inline void outb(u16 port, u8 v) { __asm__ volatile ("outb %0,%1" :: "a"(v), "Nd"(port)); }

void serial_init(void) {
    outb(COM1 + 1, 0x00); /* disable interrupts */
    outb(COM1 + 3, 0x80); /* enable DLAB (set baud rate divisor) */
    outb(COM1 + 0, 0x03); /* divisor low byte: 38400 baud */
    outb(COM1 + 1, 0x00); /* divisor high byte */
    outb(COM1 + 3, 0x03); /* 8 bits, no parity, one stop bit */
    outb(COM1 + 2, 0xC7); /* enable FIFO, clear, 14-byte threshold */
}

static void serial_putc(char c) {
    while (!(inb(COM1 + 5) & 0x20)); /* wait for transmit holding register empty */
    outb(COM1, (u8)c);
}

void serial_puts(const char *s) {
    while (*s) { if (*s == '\n') serial_putc('\r'); serial_putc(*s++); }
}
