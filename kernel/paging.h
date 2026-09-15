#ifndef PAGING_H
#define PAGING_H
void paging_install(void);

/* Identity-maps every 4MB page that overlaps [phys_addr, phys_addr+length),
   in addition to the base 4MB from paging_install. Needed for anything that
   lives at a physical address paging_install didn't already cover, like a
   PCI device's memory-mapped framebuffer. Returns 1 on success, 0 if there
   are no more spare page tables (see paging.c's MAX_EXTRA_TABLES). */
int paging_map_region(unsigned int phys_addr, unsigned int length);

/* v77 (0.66.x): Reverse paging_map_region -- unmap regions and reclaim their
   page tables. Used by window_close() to free the framebuffer mapping budget
   when the GUI exits, allowing the heap and other subsystems to grow again.
   Unmaps the 4MB pages overlapping [phys_addr, phys_addr+length) and marks
   their table slots available for reuse. */
void paging_unmap_region(unsigned int phys_addr, unsigned int length);

/* Marks one 4KB page (must fall in the base identity-mapped region, 8MB
   as of v74, either alias) as user-accessible (sets the U/S bit at both
   the page-table and page-directory level), so ring-3 code can actually
   touch it. Everything else in that region stays supervisor-only.
   ponytail: only handles the base map (paging_install's
   base_page_tables) -- that covers every ring-3 page this kernel needs
   today; extend if one ever needs to live in a region paging_map_region
   mapped instead. */
void paging_set_user(void *virt_addr);

/* v64 (0.61.0): 1 if every page of [addr, addr+len) is mapped user-
   accessible, the check a syscall makes on a ring-3 pointer before
   dereferencing it (Linux's access_ok shape). Anything outside the base
   map is kernel-only by construction and returns 0. */
int paging_user_range_ok(unsigned int addr, unsigned int len);

/* v31 (0.31.0): real per-task memory isolation. Every task gets its own
   page directory, cloned from the kernel's (so kernel code/data/stack
   stay shared and trusted, exactly as today) plus one private page table
   at a fixed slot (PAGING_PRIVATE_PDE) mapping exactly one private
   physical frame. Two different tasks both writing to the same virtual
   address, PAGING_PRIVATE_VADDR, land on two different physical frames,
   real isolation, not a shared scratch buffer with a reused name. */
/* v75 (0.66.x): was PDE 2 (0x00800000), which is the exact same PDE
   paging_map_region()'s general-purpose extra-mapping pool hands out
   FIRST once anything (kheap growth included) needs memory past the 8MB
   base map, a routine occurrence, not an edge case, under normal desktop
   use. Root-caused via reaptest: paging_new_task_directory() clones the
   kernel's page_directory by value, so a new task starts out sharing
   whatever kheap already mapped at PDE 2 -- including, if the task's own
   just-kmalloc'd kernel stack happened to land in that same 4MB region,
   the mapping for its own stack -- and then unconditionally overwrites
   dir[PAGING_PRIVATE_PDE] with its private table, destroying that shared
   mapping in its own copy only. The very first context switch into such
   a task then faults popping its own ESP: real, reproduced, and fixed by
   giving the private slot a PDE the general pool can never reach instead
   of one it reaches immediately. BASE_MAP_TABLES (2) + MAX_EXTRA_TABLES
   (16) = PDEs 0..17 are the pool's whole reachable range (paging.c); PDE
   19 leaves a one-PDE margin. Bump this again if either of those two
   constants ever grows enough to reach it. */
#define PAGING_PRIVATE_PDE   19
#define PAGING_PRIVATE_VADDR (PAGING_PRIVATE_PDE * 0x400000) /* 0x04C00000 */

/* Allocates a new page directory (cloned from the kernel's) plus its own
   private page table and one private physical frame mapped at
   PAGING_PRIVATE_VADDR. Returns the new directory's physical address (also
   the value to load into CR3), or 0 on OOM. */
unsigned int paging_new_task_directory(void);

/* Frees a directory task_exit() is done with: its private frame, its
   private page table, and the directory frame itself. Never call this on
   the kernel's own page_directory (task 0 never gets one of these). */
void paging_free_task_directory(unsigned int dir_phys);

void paging_load_directory(unsigned int dir_phys); /* loads CR3 */
unsigned int paging_kernel_directory(void); /* the shared kernel directory's own physical address, what task 0 runs on */
#endif
