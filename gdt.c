/* Flat GDT: null, ring-0 code, ring-0 data, each spanning the full 4GB.
   ponytail: no user-mode segments yet, add those with v3's ring-3/TSS work. */
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

static struct gdt_entry gdt[3];
static struct gdt_ptr gp;

static void set_gate(int n, u32 base, u32 limit, u8 access, u8 gran) {
    gdt[n].base_low  = base & 0xFFFF;
    gdt[n].base_mid  = (base >> 16) & 0xFF;
    gdt[n].base_high = (base >> 24) & 0xFF;
    gdt[n].limit_low = limit & 0xFFFF;
    gdt[n].gran      = ((limit >> 16) & 0x0F) | (gran & 0xF0);
    gdt[n].access    = access;
}

void gdt_install(void) {
    gp.limit = sizeof(gdt) - 1;
    gp.base  = (u32)&gdt;

    set_gate(0, 0, 0, 0, 0);                    /* null */
    set_gate(1, 0, 0xFFFFFFFF, 0x9A, 0xCF);      /* code: present, ring0, exec/read */
    set_gate(2, 0, 0xFFFFFFFF, 0x92, 0xCF);      /* data: present, ring0, read/write */

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
        ::: "ax"
    );
}
