/* Identity-maps the first 4MB (virtual == physical) AND maps that same
   4MB at the kernel's own higher-half base (0xC0000000, page directory
   entry 768), then turns paging on. boot.S already did an equivalent,
   temporary version of this to get from "paging off, running low" to
   "paging on, running at 0xC0100000+"; this replaces it with the kernel's
   real, permanent tables. Keeping the low identity map isn't a leftover,
   it's deliberate: pmm.c hands out physical frame addresses that kheap.c
   and others still dereference directly as pointers, exactly as they did
   before the higher-half move, so nothing downstream needed to change.
   ponytail: one page table, 4MB (reachable both ways) was everything the
   kernel, its stack, and the pmm bitmap needed, with the note "add more
   page tables when something is allocated above 0x400000." v74 (0.66.0)
   is that day: the kernel image itself (1MB load + text + the 1.5MB baked
   wallpaper + .bss) crossed 4MB once the PNG decoder's test fixtures
   landed, _kernel_end 0x40f000, and the first .bss write after
   irq_install faulted on unmapped memory. The base map is now
   BASE_MAP_TABLES back-to-back tables (8MB), mirrored at both aliases,
   and paging_set_user/paging_user_range_ok index whichever base table an
   address falls in (ring3.c's user code/stack pages are static .bss
   arrays, which is exactly what moved past 4MB; the old first-table-only
   indexing would have flipped the U/S bit on the wrong page). boot.S's
   temporary map carries the same count. */
#include "paging.h"
#include "pmm.h"

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
/* The base map: BASE_MAP_TABLES * 4MB identity-mapped, and the same
   physical range mirrored at KERNEL_VIRTUAL_BASE. Must match boot.S. */
#define BASE_MAP_TABLES 2
#define BASE_MAP_LIMIT  (BASE_MAP_TABLES * 0x400000u)
static u32 base_page_tables[BASE_MAP_TABLES][1024] __attribute__((aligned(4096)));

/* Which base table (0..BASE_MAP_TABLES-1) a virtual address lives in via
   either alias, or -1 if it's outside the base map entirely. */
static int base_table_index(u32 addr) {
    u32 pde = addr >> 22;
    if (pde >= KERNEL_PDE_INDEX) pde -= KERNEL_PDE_INDEX;
    return pde < BASE_MAP_TABLES ? (int)pde : -1;
}

/* Extra page tables for regions outside the base 4MB, statically reserved
   (not kmalloc'd) because they need page alignment kheap's bump allocator
   doesn't guarantee. Was 4 (16MB), sized when the only extra region was
   one graphics-mode framebuffer; v34 then made kheap grow through the same
   pool, so it also caps the heap. v63: at 1920x1080 the LFB alone takes
   two of the four (8.3MB spans two 4MB PDEs), leaving the heap 4MB base +
   8MB, and the GUI's real working set at that size (wind_base 5.6MB, the
   icon caches, the two 1.9MB dock-band buffers) no longer fit: the dock
   band kmalloc failed on the first hover, every hover step silently fell
   back to the direct on-screen repaint, and the tray read empty mid-frame
   in a real framebuffer dump. 16 tables = 64MB of extra mapping, 64KB of
   BSS. Physical memory is still the real ceiling (pmm), not this. */
#define MAX_EXTRA_TABLES 16
static u32 extra_page_tables[MAX_EXTRA_TABLES][1024] __attribute__((aligned(4096)));
static int extra_tables_used = 0;

/* Track which extra table (if any) is used by each PDE above the base map.
   -1 means the PDE is not using an extra table. Allows paging_unmap_region
   to free unused table slots. v77 (0.66.x): added to support window_close()
   v78+ (0.67.3): uses signed char instead of int since values range only from
   -1 to 15 (MAX_EXTRA_TABLES), saving ~3KB of kernel BSS per (1022 entries). */
static signed char pde_to_extra_table[1024 - BASE_MAP_TABLES];

/* Track which extra table slots are in use (1) or free (0), allowing reuse
   when paging_unmap_region frees a table. This fixes the bug where unmapping
   a framebuffer couldn't reclaim its table slot unless it was the last one.
   v78+ (0.67.3): uses unsigned char instead of int, saving 48 bytes of BSS. */
static unsigned char extra_table_free[MAX_EXTRA_TABLES];

void paging_install(void) {
    for (int t = 0; t < BASE_MAP_TABLES; t++)
        for (int i = 0; i < 1024; i++)
            base_page_tables[t][i] = (t * 0x400000 + i * 0x1000) | 0x3; /* present, read/write */
    for (int i = 0; i < 1024; i++) {
        page_directory[i] = 0x00000002; /* not present, read/write, supervisor */
    }
    for (int t = 0; t < BASE_MAP_TABLES; t++) {
        page_directory[t] = phys(base_page_tables[t]) | 0x3;
        /* also reachable at the high alias: this MUST be set before CR3 is
           reloaded below, otherwise the instant the new table takes effect,
           the CPU's own currently-executing EIP (a high address, the kernel is
           already running up there via boot.S's temporary tables by the time
           this function runs) would have nothing mapping it, and the very
           next instruction fetch after the CR3 write would page-fault */
        page_directory[KERNEL_PDE_INDEX + t] = phys(base_page_tables[t]) | 0x3;
    }

    /* Initialize the PDE-to-extra-table mapping (v77): allows paging_unmap_region
       to track which extra tables are in use for which PDEs. */
    for (int i = 0; i < (int)(1024 - BASE_MAP_TABLES); i++) {
        pde_to_extra_table[i] = -1;
    }

    /* Initialize the free list for extra tables: all slots start free */
    for (int i = 0; i < MAX_EXTRA_TABLES; i++) {
        extra_table_free[i] = 1;
    }

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

        /* Find the first free table slot, allowing reuse of tables freed by
           paging_unmap_region, instead of always appending to the end. */
        int table_idx = -1;
        for (int i = 0; i < MAX_EXTRA_TABLES; i++) {
            if (extra_table_free[i]) { table_idx = i; break; }
        }
        if (table_idx < 0) return 0; /* all tables in use */

        u32 *table = extra_page_tables[table_idx];
        extra_table_free[table_idx] = 0; /* mark slot as used */

        /* Recalculate extra_tables_used: the highest index + 1 of used tables */
        int new_max = 0;
        for (int i = 0; i < MAX_EXTRA_TABLES; i++) {
            if (!extra_table_free[i]) new_max = i + 1;
        }
        extra_tables_used = new_max;

        u32 base = pde * 0x400000;
        for (int i = 0; i < 1024; i++) {
            table[i] = (base + i * 0x1000) | 0x3;
        }
        page_directory[pde] = phys(table) | 0x3;

        /* Track which table this PDE is using, for unmapping later */
        if (pde >= BASE_MAP_TABLES) pde_to_extra_table[pde - BASE_MAP_TABLES] = table_idx;

        /* reload CR3 to flush the TLB now that the page directory changed */
        __asm__ volatile ("mov %0, %%cr3" :: "r"(phys(page_directory)));
    }
    return 1;
}

/* Reverse paging_map_region: unmap regions and free their page tables.
   v77 (0.66.x): Real fix for GUI consuming all extra page tables. When
   window_close() or other subsystems finish with large mappings
   (framebuffers, etc.), they can now call this to reclaim the mapping
   budget. Unmapped PDEs have their table slots freed for reuse. */
void paging_unmap_region(u32 phys_addr, u32 length) {
    u32 start_pde = phys_addr / 0x400000;
    u32 end_pde   = (phys_addr + length - 1) / 0x400000;

    for (u32 pde = start_pde; pde <= end_pde; pde++) {
        if (!(page_directory[pde] & 0x1)) continue; /* not mapped, nothing to unmap */

        /* Only unmap if this PDE is using an extra table (not the base map).
           Base map PDEs stay permanently mapped. */
        if (pde >= BASE_MAP_TABLES) {
            int table_idx = pde_to_extra_table[pde - BASE_MAP_TABLES];
            if (table_idx >= 0) {
                page_directory[pde] = 0x00000002; /* not present, read/write, supervisor */
                pde_to_extra_table[pde - BASE_MAP_TABLES] = -1;

                /* Mark the table slot as free for reuse by paging_map_region.
                   Recalculate extra_tables_used as the highest index + 1 of
                   used tables. This allows any freed table to be reclaimed
                   immediately, not just the last one. */
                extra_table_free[table_idx] = 1;
                int new_max = 0;
                for (int i = 0; i < MAX_EXTRA_TABLES; i++) {
                    if (!extra_table_free[i]) new_max = i + 1;
                }
                extra_tables_used = new_max;
            }
        }
    }

    /* reload CR3 to flush the TLB now that the page directory changed */
    __asm__ volatile ("mov %0, %%cr3" :: "r"(phys(page_directory)));
}

void paging_set_user(void *virt_addr) {
    u32 addr = (u32)virt_addr;
    int t = base_table_index(addr);
    if (t < 0) return; /* outside the base map: paging.h documents this as unsupported */
    u32 pte = (addr % 0x400000) / 0x1000;
    base_page_tables[t][pte] |= 0x4;
    page_directory[t] |= 0x4;
    page_directory[KERNEL_PDE_INDEX + t] |= 0x4;
    __asm__ volatile ("mov %0, %%cr3" :: "r"(phys(page_directory)));
}

/* v64 (0.61.0): access_ok for syscalls. Only the base map (either alias)
   can hold user pages today (paging_set_user's own limit), so anything
   outside it is a kernel-only address by construction. Within it, every
   page in the range needs both present and U/S set in the shared base
   table for its 4MB slot, which every task directory points at by value. */
int paging_user_range_ok(unsigned int addr, unsigned int len) {
    if (len == 0) return 1;
    unsigned int end = addr + len;
    if (end < addr) return 0; /* wrapped */
    for (u32 p = addr & ~0xFFFu; p < end; p += 0x1000) {
        int t = base_table_index(p);
        if (t < 0) return 0;
        u32 pte = (p >> 12) & 0x3FF;
        if ((base_page_tables[t][pte] & 0x5) != 0x5) return 0; /* present + user */
        if (p > 0xFFFFF000u - 0x1000) break; /* next += would wrap; end < addr already ruled the range in */
    }
    return 1;
}

/* v31 (0.31.0): real per-task memory isolation, see paging.h. A directory
   and its private table/frame are all plain physical frames from pmm,
   below IDENTITY_MAP_LIMIT (the base map's end, the same value kheap.c
   keys its on-demand mapping off) so this file can dereference them
   directly as pointers, the same established convention kheap.c already
   relies on. */
#define IDENTITY_MAP_LIMIT BASE_MAP_LIMIT

unsigned int paging_kernel_directory(void) {
    return phys(page_directory);
}

/* v75 (0.66.x): task creation used to require dir_phys/table_phys to land
   below the fixed 8MB IDENTITY_MAP_LIMIT, the only region guaranteed
   directly dereferenceable as a plain pointer. Real bug, root-caused via
   reaptest: pmm_alloc_frame() hands out the lowest free frame first, and
   normal desktop use (wallpaper/weather/font/window buffers through
   kheap, all long-lived, never freed) fills that entire 8MB region during
   ordinary GUI use, not a pathological case. Once it's full,
   pmm_alloc_frame() can only return frames above the limit, and every
   later task_create() -- not just a 6th, ANY of them -- failed
   permanently for the rest of the boot, exactly reaptest's real, scoped
   symptom (n=0, "paging_new_task_directory failed", confirmed via a
   temporary serial trace before this fix). The actual constraint was
   narrower than the code enforced: dir_phys/table_phys only need to be
   dereferenceable *while paging.c itself writes their initial entries*,
   which paging_map_region() (already proven, kheap growth and window
   framebuffers both escape the same original 8MB cap through it) can
   guarantee for any physical frame by identity-mapping its whole 4MB PDE
   on demand. page_phys never gets dereferenced by this code at all, it's
   only ever stored as a physical address inside a page-table entry, so
   it needs no mapping and no limit check either, that restriction was
   never load-bearing to begin with. */
unsigned int paging_new_task_directory(void) {
    u32 dir_phys = pmm_alloc_frame();
    if (!dir_phys) return 0;
    if (dir_phys >= IDENTITY_MAP_LIMIT && !paging_map_region(dir_phys, 0x1000)) { pmm_free_frame(dir_phys); return 0; }

    u32 table_phys = pmm_alloc_frame();
    if (!table_phys) { pmm_free_frame(dir_phys); return 0; }
    /* Resolved BEFORE the dir[]=page_directory[] copy below, on purpose:
       paging_map_region() can add a fresh entry to the shared kernel
       page_directory (a new extra_page_tables slot) when table_phys falls
       outside every region already mapped. If that happened after the
       copy instead, this new task's own directory would silently miss
       that entry, and the kernel could page-fault dereferencing it the
       next time this specific task is current. Doing it first means
       whatever the kernel's page_directory looks like by the time the
       copy runs is exactly what this task inherits, same guarantee
       paging_install's own ordering already relies on. */
    if (table_phys >= IDENTITY_MAP_LIMIT && !paging_map_region(table_phys, 0x1000)) { pmm_free_frame(table_phys); pmm_free_frame(dir_phys); return 0; }

    u32 *dir = (u32 *)dir_phys;
    for (int i = 0; i < 1024; i++) dir[i] = page_directory[i]; /* share every existing mapping (kernel code/data/stack) by value */

    u32 *table = (u32 *)table_phys;
    for (int i = 0; i < 1024; i++) table[i] = 0x00000002; /* not present, read/write, supervisor */

    u32 page_phys = pmm_alloc_frame(); /* never dereferenced here, only stored as a PTE value, no mapping/limit needed */
    if (!page_phys) { pmm_free_frame(table_phys); pmm_free_frame(dir_phys); return 0; }
    table[0] = page_phys | 0x3; /* the one private page, PAGING_PRIVATE_VADDR's page-table index is 0 since it starts a fresh 4MB region */

    dir[PAGING_PRIVATE_PDE] = table_phys | 0x3;
    return dir_phys;
}

void paging_free_task_directory(unsigned int dir_phys) {
    u32 *dir = (u32 *)dir_phys;
    u32 table_phys = dir[PAGING_PRIVATE_PDE] & ~0xFFF;
    u32 *table = (u32 *)table_phys;
    u32 page_phys = table[0] & ~0xFFF;
    pmm_free_frame(page_phys);
    pmm_free_frame(table_phys);
    pmm_free_frame(dir_phys);
}

void paging_load_directory(unsigned int dir_phys) {
    __asm__ volatile ("mov %0, %%cr3" :: "r"(dir_phys));
}
