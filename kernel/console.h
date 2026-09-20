#ifndef CONSOLE_H
#define CONSOLE_H
void putc(char c);
void puts(const char *s);
void puthex(unsigned int v);

/* The next already-queued keystroke as an ASCII character, or -1 if the
   keyboard ring is empty. Non-blocking on purpose: its one caller is
   sys_read(fd 0), which runs inside an interrupt gate with IF clear, so
   the keyboard IRQ that would deliver a byte cannot fire while it waits.
   Key releases and keys with no ASCII meaning are skipped, not returned
   as zeroes, so a caller never has to know about scancodes. */
int console_read_key(void);
#endif
