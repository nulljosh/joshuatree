#ifndef IDT_H
#define IDT_H
void idt_install(void);
void idt_set_gate(int n, unsigned int base, unsigned short sel, unsigned char flags);
#endif
