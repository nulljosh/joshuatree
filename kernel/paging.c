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
#include "serial.h"

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

/* 1.9.24: every ring-3 task directory is a by-value copy of page_directory
   taken at task_create, so a kernel PDE that appears later (kheap growing
   through paging_map_region into a 4 MB region that had no table when the
   task was born) was missing from that copy, and the first syscall or IRQ
   running on the task's CR3 that touched the new memory page-faulted
   (Mail's close inside r3win_release was the symptom). Preallocating every
   identity table is not the right trade here: the identity map covers
   physical frames on demand, the framebuffer sits at 0xFD000000, and a
   full table set would be 4 MB of BSS. Instead the shared directory is
   the single source of truth and every change to a kernel PDE is written
   through to every live task directory on the spot, skipping the two
   slots a task owns privately (PAGING_PRIVATE_PDE and the user window
   table at KERNEL_PDE_INDEX + 1). Zero extra memory, one 6-slot loop per
   table change, and table changes are rare (once per 4 MB of growth). */
#include "task.h"
#include "memmap.h"
#define BRK_PDE_FIRST (JT_BRK_BASE >> 22)
#define BRK_PDE_LAST ((JT_BRK_BASE + JT_BRK_MAX_PAGES * 4096u - 1) >> 22)
/* 1.9.28: the brk heap PDEs (kernel/brk.c) are task-private too: the sync must never overwrite them and the check must not count them as drift. */
static int paging_pde_is_private(u32 pde) { return pde == PAGING_PRIVATE_PDE || pde == KERNEL_PDE_INDEX + 1 || (pde >= BRK_PDE_FIRST && pde <= BRK_PDE_LAST); }
/* 1.9.28: flush the TLB on whatever directory the CPU is using. Reloading
   phys(page_directory) here was the ring-3 heap bug: a syscall runs on the
   task's CR3, and paging_set_user (SYS_WINDOW_OPEN's legacy path) or a
   kmalloc that grew the identity map silently moved the task onto the
   kernel directory, so the pages brk_set mapped into task_page_dir() were
   never the ones the CPU looked at and the first heap write faulted at
   JT_BRK_BASE. */
static void paging_flush_current(void) { u32 cr3; __asm__ volatile ("mov %%cr3, %0" : "=r"(cr3)); __asm__ volatile ("mov %0, %%cr3" :: "r"(cr3) : "memory"); }
static unsigned int pde_changes; /* 1.9.24: bumps on every kernel PDE create or remove, read by stress=pde */
unsigned int paging_pde_change_count(void) { return pde_changes; }
static void paging_sync_task_dirs(u32 pde) {
    pde_changes++;
    if (paging_pde_is_private(pde)) return;
    u32 me = phys(page_directory);
    for (int id = 0; id < TASK_SLOTS; id++) {
        u32 d = task_page_dir(id);
        if (!d || d == me) continue;
        ((u32 *)d)[pde] = page_directory[pde];
    }
}
/* 1.9.24: the freeze check. After paging_install the kernel half of every
   live task directory must equal the shared directory entry for entry;
   logs one BUG line per drift and returns how many it found. Runs after
   every sync and from the stress=pde hook. */
int paging_check_task_dirs(void) {
    int bad = 0;
    u32 me = phys(page_directory);
    for (int id = 0; id < TASK_SLOTS; id++) {
        u32 d = task_page_dir(id);
        if (!d || d == me) continue;
        const u32 *dir = (const u32 *)d;
        for (u32 pde = 0; pde < 1024; pde++) {
            if (paging_pde_is_private(pde)) continue;
            if (dir[pde] != page_directory[pde]) { bad++; serial_puts("BUG: paging: task directory drifted from the kernel PDEs\n"); break; }
        }
    }
    return bad;
}

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
        paging_sync_task_dirs(pde); /* 1.9.24: write the new kernel PDE through to every live task directory */

        /* Track which table this PDE is using, for unmapping later */
        if (pde >= BASE_MAP_TABLES) pde_to_extra_table[pde - BASE_MAP_TABLES] = table_idx;

        /* reload CR3 to flush the TLB now that the page directory changed */
        paging_flush_current();
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
                paging_sync_task_dirs(pde); /* 1.9.24: and the removal too, a stale present entry would point at a reused table */
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
    paging_flush_current();
}

void paging_set_user(void *virt_addr) {
    u32 addr = (u32)virt_addr;
    int t = base_table_index(addr);
    if (t < 0) return; /* outside the base map: paging.h documents this as unsupported */
    u32 pte = (addr % 0x400000) / 0x1000;
    base_page_tables[t][pte] |= 0x4;
    page_directory[t] |= 0x4;
    page_directory[KERNEL_PDE_INDEX + t] |= 0x4;
    paging_flush_current();
}

/* 1.7.8: see paging.h. Per page, and per alias for the TLB: the same
   frame is visible at addr and at addr +/- KERNEL_VIRTUAL_BASE, and a
   stale user-permitted TLB entry for either would let the write through
   after the PTE says no. */
void paging_clear_user(void *addr, unsigned int len) {
    u32 start = (u32)addr & ~0xFFFu;
    u32 end = (u32)addr + len;
    if (end < (u32)addr) end = 0xFFFFF000u;
    for (u32 p = start; p < end; p += 0x1000) {
        int t = base_table_index(p);
        if (t < 0) return;
        u32 pte = (p >> 12) & 0x3FF;
        base_page_tables[t][pte] &= ~0x4u;
        u32 alias = p >= KERNEL_VIRTUAL_BASE ? p - KERNEL_VIRTUAL_BASE : p + KERNEL_VIRTUAL_BASE;
        __asm__ volatile ("invlpg (%0)" :: "r"(p) : "memory");
        __asm__ volatile ("invlpg (%0)" :: "r"(alias) : "memory");
        if (p > 0xFFFFF000u - 0x1000) break;
    }
}

/* v64 (0.61.0): access_ok for syscalls. Only the base map (either alias)
   can hold user pages today (paging_set_user's own limit), so anything
   outside it is a kernel-only address by construction. Within it, every
   page in the range needs both present and U/S set in the shared base
   table for its 4MB slot, which every task directory points at by value. */
int paging_user_range_ok_current(unsigned int addr, unsigned int len);
int paging_user_range_ok(unsigned int addr, unsigned int len) {
    return paging_user_range_ok_current(addr, len); /* 1.9.23: walk CR3, so a private window (paging_task_map_private) is judged by its own tables */
}
static int paging_user_range_ok_shared(unsigned int addr, unsigned int len) __attribute__((unused));
static int paging_user_range_ok_shared(unsigned int addr, unsigned int len) {
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

/* 1.9.23: a private user window for one task. Every task directory
   shares base_page_tables[1] by value, which is why two ring-3 programs
   could never run at once: JT_USER_BASE and JT_USER_FB are the same
   physical frames in every directory. This gives one directory its own
   copy of that table (a pmm frame, identity-mapped) with the image, stack
   and framebuffer PTEs pointing at frames only this task maps, user
   accessible there and nowhere else. The low identity alias (PDE 1) keeps
   the shared supervisor-only table, so no task reaches another's pages
   through it. Returns 0 on OOM. */
int paging_task_map_private(unsigned int dir_phys, unsigned int vaddr, unsigned int pa, unsigned int len) {
    u32 *dir = (u32 *)dir_phys;
    u32 pde = vaddr >> 22;
    if (base_table_index(vaddr) != 1 || pde != KERNEL_PDE_INDEX + 1) return 0;
    u32 table_phys;
    if ((dir[pde] & ~0xFFFu) == phys(base_page_tables[1])) {
        table_phys = pmm_alloc_frame();
        if (!table_phys) return 0;
        if (table_phys >= IDENTITY_MAP_LIMIT && !paging_map_region(table_phys, 0x1000)) { pmm_free_frame(table_phys); return 0; }
        u32 *t = (u32 *)table_phys;
        for (int i = 0; i < 1024; i++) t[i] = base_page_tables[1][i] & ~0x4u; /* kernel pages, supervisor only */
        dir[pde] = table_phys | 0x7;
    } else table_phys = dir[pde] & ~0xFFFu;
    u32 *t = (u32 *)table_phys;
    for (u32 off = 0; off < len; off += 0x1000) {
        u32 pte = ((vaddr + off) >> 12) & 0x3FF;
        t[pte] = ((pa + off) & ~0xFFFu) | 0x7;
    }
    return 1;
}

/* 1.10.x: re-map a private window at a new size. Pages of the old buffer
   past the new length go back to the kernel's supervisor-only entries, the
   new frames are mapped user-accessible, and the CPU's TLB is flushed on
   the current directory (the caller is the owning task, inside its own
   syscall). */
int paging_task_remap_private(unsigned int dir_phys, unsigned int vaddr, unsigned int pa, unsigned int len, unsigned int old_len) {
    if (!paging_task_map_private(dir_phys, vaddr, pa, len)) return 0;
    u32 *dir = (u32 *)dir_phys;
    u32 *t = (u32 *)(dir[vaddr >> 22] & ~0xFFFu);
    for (u32 off = len; off < old_len; off += 0x1000) {
        u32 pte = ((vaddr + off) >> 12) & 0x3FF;
        t[pte] = base_page_tables[1][pte] & ~0x4u;
    }
    paging_flush_current();
    return 1;
}

/* 1.9.23: the reverse, for paging_free_task_directory's caller: hand the
   private table frame back. The frames it pointed at are the window's
   own kmalloc'd buffers, freed by their owner. */
void paging_task_unmap_private(unsigned int dir_phys) {
    u32 *dir = (u32 *)dir_phys;
    u32 pde = KERNEL_PDE_INDEX + 1;
    u32 table_phys = dir[pde] & ~0xFFFu;
    if (table_phys == phys(base_page_tables[1])) return;
    dir[pde] = phys(base_page_tables[1]) | 0x3;
    pmm_free_frame(table_phys);
}

/* 1.9.23: access_ok against the directory the CPU is actually using
   (CR3), so a task with a private window (paging_task_map_private) is
   checked against its own tables, and a legacy task against the shared
   ones. Tables are pmm frames or kernel statics, both identity-mapped. */
int paging_user_range_ok_current(unsigned int addr, unsigned int len) {
    if (len == 0) return 1;
    unsigned int end = addr + len;
    if (end < addr) return 0;
    u32 cr3; __asm__ volatile ("mov %%cr3, %0" : "=r"(cr3));
    const u32 *dir = (const u32 *)(cr3 & ~0xFFFu);
    for (u32 p = addr & ~0xFFFu; p < end; p += 0x1000) {
        u32 pde = dir[p >> 22];
        if ((pde & 0x5) != 0x5) return 0;
        const u32 *t = (const u32 *)(pde & ~0xFFFu);
        if ((t[(p >> 12) & 0x3FF] & 0x5) != 0x5) return 0;
        if (p > 0xFFFFF000u - 0x1000) break;
    }
    return 1;
}
