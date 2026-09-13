#ifndef PAGING_H
#define PAGING_H
void paging_install(void);

/* Identity-maps every 4MB page that overlaps [phys_addr, phys_addr+length),
   in addition to the base 4MB from paging_install. Needed for anything that
   lives at a physical address paging_install didn't already cover, like a
   PCI device's memory-mapped framebuffer. Returns 1 on success, 0 if there
   are no more spare page tables (see paging.c's MAX_EXTRA_TABLES). */
int paging_map_region(unsigned int phys_addr, unsigned int length);
#endif
