/* Identity-maps the first 4MB (virtual == physical) AND maps that same
   4MB at the kernel's own higher-half base (0xC0000000, page directory
   entry 768), then turns paging on. boot.S already did an equivalent,
   temporary version of this to get from "paging off, running low" to
   "paging on, running at 0xC0100000+"; this replaces it with the kernel's
   real, permanent tables. Keeping the low identity map isn't a leftover,
   it's deliberate: pmm.c hands out physical frame addresses that kheap.c
   and others still dereference directly as pointers, exactly as they did
   before the higher-half move, so nothing downstream needed to change.
   ponytail: one page table, 4MB (reachable both ways) is everything the
   kernel, its stack, and the pmm bitmap need right now. Add more page
   tables (or 4MB pages) when something is allocated above 0x400000. */
#include "paging.h"

typedef unsigned int u32;

#define KERNEL_VIRTUAL_BASE 0xC0000000
#define KERNEL_PDE_INDEX (KERNEL_VIRTUAL_BASE >> 22)

/* Page directory entries and CR3 are read by the MMU itself, which has no
   notion of "virtual", it only ever walks physical memory: every table
   address stored INTO another table (or into CR3) has to be physical, even
   though every one of these tables is now a normal higher-half-linked C
   static whose OWN address, as far as C code seeing &table is concerned,
   is a high virtual one. Missing this exact conversion was the first real
   bug this move produced: a page-directory entry holding a virtual address
   pointed the MMU at physical memory that happened to be unmapped garbage,
   producing a page fault immediately followed by a double fault. */
static inline u32 phys(void *virt) { return (u32)virt - KERNEL_VIRTUAL_BASE; }

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
    page_directory[0] = phys(first_page_table) | 0x3;
    /* also reachable at the high alias: this MUST be set before CR3 is
       reloaded below, otherwise the instant the new table takes effect,
       the CPU's own currently-executing EIP (a high address, the kernel is
       already running up there via boot.S's temporary tables by the time
       this function runs) would have nothing mapping it, and the very
       next instruction fetch after the CR3 write would page-fault */
    page_directory[KERNEL_PDE_INDEX] = phys(first_page_table) | 0x3;

    __asm__ volatile ("mov %0, %%cr3" :: "r"(phys(page_directory)));

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
        page_directory[pde] = phys(table) | 0x3;

        /* reload CR3 to flush the TLB now that the page directory changed */
        __asm__ volatile ("mov %0, %%cr3" :: "r"(phys(page_directory)));
    }
    return 1;
}

void paging_set_user(void *virt_addr) {
    u32 addr = (u32)virt_addr;
    u32 pte = (addr % 0x400000) / 0x1000;
    first_page_table[pte] |= 0x4;
    page_directory[0] |= 0x4;
    page_directory[KERNEL_PDE_INDEX] |= 0x4;
    __asm__ volatile ("mov %0, %%cr3" :: "r"(phys(page_directory)));
}
