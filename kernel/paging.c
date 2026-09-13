/* Identity-maps the first 4MB (virtual == physical) and turns paging on.
   ponytail: one page table, 4MB is everything the kernel, its stack, and
   the pmm bitmap need right now. Add more page tables (or 4MB pages) when
   something is allocated above 0x400000 -- the higher-half move later in
   v2 is the natural point to redo this properly. */
#include "paging.h"

typedef unsigned int u32;

static u32 page_directory[1024]   __attribute__((aligned(4096)));
static u32 first_page_table[1024] __attribute__((aligned(4096)));

/* Extra page tables for regions outside the base 4MB, statically reserved
   (not kmalloc'd) because they need page alignment kheap's bump allocator
   doesn't guarantee. 4 tables covers 16MB of extra mapping, comfortably
   more than one graphics-mode framebuffer needs. */
#define MAX_EXTRA_TABLES 4
static u32 extra_page_tables[MAX_EXTRA_TABLES][1024] __attribute__((aligned(4096)));
static int extra_tables_used = 0;

void paging_install(void) {
    for (int i = 0; i < 1024; i++) {
        first_page_table[i] = (i * 0x1000) | 0x3; /* present, read/write */
    }
    for (int i = 0; i < 1024; i++) {
        page_directory[i] = 0x00000002; /* not present, read/write, supervisor */
    }
    page_directory[0] = ((u32)first_page_table) | 0x3;

    __asm__ volatile ("mov %0, %%cr3" :: "r"(page_directory));

    u32 cr0;
    __asm__ volatile ("mov %%cr0, %0" : "=r"(cr0));
    cr0 |= 0x80000000; /* PG bit */
    __asm__ volatile ("mov %0, %%cr0" :: "r"(cr0));
}

int paging_map_region(u32 phys_addr, u32 length) {
    u32 start_pde = phys_addr / 0x400000;
    u32 end_pde   = (phys_addr + length - 1) / 0x400000;

    for (u32 pde = start_pde; pde <= end_pde; pde++) {
        if (page_directory[pde] & 0x1) continue; /* already mapped */
        if (extra_tables_used >= MAX_EXTRA_TABLES) return 0;

        u32 *table = extra_page_tables[extra_tables_used++];
        u32 base = pde * 0x400000;
        for (int i = 0; i < 1024; i++) {
            table[i] = (base + i * 0x1000) | 0x3;
        }
        page_directory[pde] = ((u32)table) | 0x3;

        /* reload CR3 to flush the TLB now that the page directory changed */
        __asm__ volatile ("mov %0, %%cr3" :: "r"(page_directory));
    }
    return 1;
}

void paging_set_user(void *virt_addr) {
    u32 addr = (u32)virt_addr;
    u32 pte = (addr % 0x400000) / 0x1000;
    first_page_table[pte] |= 0x4;
    page_directory[0] |= 0x4;
    __asm__ volatile ("mov %0, %%cr3" :: "r"(page_directory));
}
