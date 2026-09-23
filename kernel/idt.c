/* IDT + the 32 CPU-exception ISRs. A ring-0 exception means a real kernel
   bug, so the handler prints and halts, no recovery attempted. v64: an
   exception from ring 3 is a user task's bug, not the kernel's, so that
   path reports it and reaps the task through the same task_exit machinery
   every other task ends through, and the kernel keeps running. The CS the
   CPU pushed says which case this is (RPL 3 = came from ring 3), which is
   why isr.S now hands the whole frame over instead of just the vector. */
#include "idt.h"
#include "console.h"
#include "serial.h"
#include "task.h"

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

void isr_handler(u32 vector, u32 err, u32 eip, u32 cs, u32 eflags) {
    (void)err; (void)eflags;
    const char *name = EXC_NAME[vector < 32 ? vector : 31];
    u32 fault_addr = 0;
    if (vector == 14) __asm__ volatile ("mov %%cr2, %0" : "=r"(fault_addr));

    if ((cs & 3) == 3 && task_current() != 0) {
        /* From ring 3, and not the shell's own slot (task 0 is the one
           task that can't be reaped, it has no stack of its own to free).
           Report exactly what happened, then end this task the way
           task_exit ends any other: its kernel stack and page directory
           freed, its slot skipped by the scheduler from here on. We're
           already on this task's own kernel stack (the TSS esp0 switch
           put us there), so task_exit's "free the stack you're standing
           on, then int $32 away" reasoning applies unchanged. */
        puts("\n!! ring-3 task hit "); puts(name);
        if (vector == 14) { puts(" at "); puthex(fault_addr); }
        puts(" (eip "); puthex(eip); puts(") -- task killed, kernel continues\n");
        serial_puts("exception: ring-3 task hit "); serial_puts(name); serial_puts(", reaped\n");
        task_exit_with(-(int)vector);
    }

    puts("\n!! CPU exception: ");
    puts(name);
    if (vector == 14) { puts(" at "); puthex(fault_addr); }
    puts(" -- halted\n");
    serial_puts("exception: ring-0 "); serial_puts(name); serial_puts(", halted\n");

    /* Display panic screen on GUI if active */
    extern void gui_panic_screen(const char *name, unsigned int fault_addr, unsigned int eip);
    gui_panic_screen(name, fault_addr, eip);

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
