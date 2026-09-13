#ifndef PAGING_H
#define PAGING_H
void paging_install(void);

/* Identity-maps every 4MB page that overlaps [phys_addr, phys_addr+length),
   in addition to the base 4MB from paging_install. Needed for anything that
   lives at a physical address paging_install didn't already cover, like a
   PCI device's memory-mapped framebuffer. Returns 1 on success, 0 if there
   are no more spare page tables (see paging.c's MAX_EXTRA_TABLES). */
int paging_map_region(unsigned int phys_addr, unsigned int length);

/* Marks one 4KB page (must fall in the base identity-mapped 4MB) as
   user-accessible (sets the U/S bit at both the page-table and page-
   directory level), so ring-3 code can actually touch it. Everything else
   in that region stays supervisor-only. ponytail: only handles the base
   4MB (paging_install's first_page_table) -- that covers every ring-3 page
   this kernel needs today; extend if one ever needs to live in a region
   paging_map_region mapped instead. */
void paging_set_user(void *virt_addr);
#endif
