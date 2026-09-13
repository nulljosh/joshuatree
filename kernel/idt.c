/* IDT + the 32 CPU-exception ISRs. ponytail: no recovery path, an exception
   here means a real bug, so the handler prints and halts instead of trying
   to resume. Add iret + register save/restore when IRQs (v1's next step)
   need to return control to interrupted code. */
#include "idt.h"
#include "console.h"

typedef unsigned int  u32;
typedef unsigned short u16;
typedef unsigned char u8;

struct idt_entry {
    u16 base_low;
    u16 sel;
    u8  zero;
    u8  flags;
    u16 base_high;
} __attribute__((packed));

struct idt_ptr {
    u16 limit;
    u32 base;
} __attribute__((packed));

static struct idt_entry idt[256];
static struct idt_ptr ip;

void idt_set_gate(int n, u32 base, u16 sel, u8 flags) {
    idt[n].base_low  = base & 0xFFFF;
    idt[n].base_high = (base >> 16) & 0xFFFF;
    idt[n].sel       = sel;
    idt[n].zero      = 0;
    idt[n].flags     = flags;
}

#define ISR(n) extern void isr##n(void);
ISR(0) ISR(1) ISR(2) ISR(3) ISR(4) ISR(5) ISR(6) ISR(7)
ISR(8) ISR(9) ISR(10) ISR(11) ISR(12) ISR(13) ISR(14) ISR(15)
ISR(16) ISR(17) ISR(18) ISR(19) ISR(20) ISR(21) ISR(22) ISR(23)
ISR(24) ISR(25) ISR(26) ISR(27) ISR(28) ISR(29) ISR(30) ISR(31)
#undef ISR

static const char *EXC_NAME[32] = {
    "divide-by-zero", "debug", "NMI", "breakpoint", "overflow",
    "bound-range", "invalid-opcode", "device-not-available",
    "double-fault", "coprocessor-overrun", "invalid-TSS",
    "segment-not-present", "stack-fault", "general-protection",
    "page-fault", "reserved", "x87-fp", "alignment-check",
    "machine-check", "SIMD-fp", "virtualization", "control-protection",
    "reserved", "reserved", "reserved", "reserved", "reserved",
    "reserved", "hypervisor-injection", "VMM-communication",
    "security", "reserved"
};

void isr_handler(u32 vector) {
    puts("\n!! CPU exception: ");
    puts(EXC_NAME[vector < 32 ? vector : 31]);
    if (vector == 14) {
        u32 fault_addr;
        __asm__ volatile ("mov %%cr2, %0" : "=r"(fault_addr));
        puts(" at ");
        puthex(fault_addr);
    }
    puts(" -- halted\n");
    for (;;) __asm__ volatile ("cli; hlt");
}

void idt_install(void) {
    ip.limit = sizeof(idt) - 1;
    ip.base  = (u32)&idt;
    for (int i = 0; i < 256; i++) idt_set_gate(i, 0, 0, 0);

#define SET(n) idt_set_gate(n, (u32)isr##n, 0x08, 0x8E)
    SET(0);  SET(1);  SET(2);  SET(3);  SET(4);  SET(5);  SET(6);  SET(7);
    SET(8);  SET(9);  SET(10); SET(11); SET(12); SET(13); SET(14); SET(15);
    SET(16); SET(17); SET(18); SET(19); SET(20); SET(21); SET(22); SET(23);
    SET(24); SET(25); SET(26); SET(27); SET(28); SET(29); SET(30); SET(31);
#undef SET

    __asm__ volatile ("lidt %0" :: "m"(ip));
}
