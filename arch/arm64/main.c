/* M0: serial hello. The PL011 UART on QEMU's virt machine sits at 0x09000000.
   A real Pi 4 has the same PL011 at 0xFE201000 (M4 changes only this base). */
#define UART0 ((volatile unsigned int *)0x09000000UL)
#define UARTFR 0x18 / 4
#define TXFF (1u << 5)

static void uart_putc(char c) {
    while (UART0[UARTFR] & TXFF) {}
    UART0[0] = (unsigned char)c;
}
static void uart_puts(const char *s) { while (*s) uart_putc(*s++); }

void main(void) {
    unsigned long el;
    __asm__ volatile ("mrs %0, CurrentEL" : "=r"(el));
    uart_puts("Joshua Tree on ARM64\n");
    uart_puts(((el >> 2) & 3) == 1 ? "EL1\n" : "not EL1\n");
    uart_puts("M0 ok\n");
    for (;;) __asm__ volatile ("wfe");
}
