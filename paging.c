/* Identity-maps the first 4MB (virtual == physical) and turns paging on.
   ponytail: one page table, 4MB is everything the kernel, its stack, and
   the pmm bitmap need right now. Add more page tables (or 4MB pages) when
   something is allocated above 0x400000 -- the higher-half move later in
   v2 is the natural point to redo this properly. */
#include "paging.h"

typedef unsigned int u32;

static u32 page_directory[1024]   __attribute__((aligned(4096)));
static u32 first_page_table[1024] __attribute__((aligned(4096)));

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
