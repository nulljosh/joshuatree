/* Flat GDT: null, ring-0 code/data, ring-3 code/data, and a TSS whose only
   real job is telling the CPU which kernel stack (ss0:esp0) to switch to
   when a ring-3->ring-0 privilege change happens: an int 0x80 syscall, a
   timer tick, or a fault (see ring3.c, syscall.c). Still one static TSS,
   never task-switched through (no hardware task switching, same as Linux
   and xv6), but v64 made esp0 a moving target: task.c's schedule() points
   it at the next task's own kernel stack on every switch, because two
   ring-3 tasks sharing one fixed kernel stack would have the second's
   trap frame land on top of the first's saved one. */
#include "gdt.h"

typedef unsigned int  u32;
typedef unsigned short u16;
typedef unsigned char u8;

struct gdt_entry {
    u16 limit_low;
    u16 base_low;
    u8  base_mid;
    u8  access;
    u8  gran;
    u8  base_high;
} __attribute__((packed));

struct gdt_ptr {
    u16 limit;
    u32 base;
} __attribute__((packed));

/* Only esp0/ss0 matter here: the fields the CPU actually reads on a
   privilege-raising interrupt. Everything else stays zeroed. iomap_base
   equal to the TSS limit means "no I/O permission bitmap present", so a
   ring-3 in/out always faults regardless of IOPL, which is exactly the
   privileged-instruction fault this whole feature exists to prove. */
struct tss_entry {
    u32 prev_tss;
    u32 esp0, ss0;
    u32 esp1, ss1;
    u32 esp2, ss2;
    u32 cr3;
    u32 eip, eflags;
    u32 eax, ecx, edx, ebx, esp, ebp, esi, edi;
    u32 es, cs, ss, ds, fs, gs;
    u32 ldt;
    u16 trap, iomap_base;
} __attribute__((packed));

static struct gdt_entry gdt[6];
static struct gdt_ptr gp;
static struct tss_entry tss;
static u8 tss_kernel_stack[4096] __attribute__((aligned(16)));

static void set_gate(int n, u32 base, u32 limit, u8 access, u8 gran) {
    gdt[n].base_low  = base & 0xFFFF;
    gdt[n].base_mid  = (base >> 16) & 0xFF;
    gdt[n].base_high = (base >> 24) & 0xFF;
    gdt[n].limit_low = limit & 0xFFFF;
    gdt[n].gran      = ((limit >> 16) & 0x0F) | (gran & 0xF0);
    gdt[n].access    = access;
}

/* 0 means "back to the boot-time static stack", what task 0 (the shell,
   which never runs at ring 3 and so never actually triggers the switch)
   gets. Read by the CPU only at the moment of a ring-3->ring-0 transition,
   so changing it while already in ring 0 is always safe. */
void gdt_set_kernel_stack(u32 esp0) {
    tss.esp0 = esp0 ? esp0 : (u32)(tss_kernel_stack + sizeof(tss_kernel_stack));
}

void gdt_install(void) {
    gp.limit = sizeof(gdt) - 1;
    gp.base  = (u32)&gdt;

    set_gate(0, 0, 0, 0, 0);                    /* null */
    set_gate(1, 0, 0xFFFFFFFF, 0x9A, 0xCF);      /* 0x08 code: present, ring0, exec/read */
    set_gate(2, 0, 0xFFFFFFFF, 0x92, 0xCF);      /* 0x10 data: present, ring0, read/write */
    set_gate(3, 0, 0xFFFFFFFF, 0xFA, 0xCF);      /* 0x18 code: present, ring3, exec/read */
    set_gate(4, 0, 0xFFFFFFFF, 0xF2, 0xCF);      /* 0x20 data: present, ring3, read/write */

    for (u32 i = 0; i < sizeof(tss); i++) ((u8 *)&tss)[i] = 0;
    tss.ss0 = 0x10;
    tss.esp0 = (u32)(tss_kernel_stack + sizeof(tss_kernel_stack));
    tss.iomap_base = sizeof(tss); /* no I/O bitmap: any ring-3 in/out faults */
    set_gate(5, (u32)&tss, sizeof(tss) - 1, 0x89, 0x00); /* 0x28 TSS: present, ring0, 32-bit TSS available */

    __asm__ volatile ("lgdt %0" :: "m"(gp));
    __asm__ volatile (
        "ljmp $0x08, $1f\n"
        "1:\n"
        "mov $0x10, %%ax\n"
        "mov %%ax, %%ds\n"
        "mov %%ax, %%es\n"
        "mov %%ax, %%fs\n"
        "mov %%ax, %%gs\n"
        "mov %%ax, %%ss\n"
        "mov $0x28, %%ax\n"
        "ltr %%ax\n"
        ::: "ax"
    );
}
