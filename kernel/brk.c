/* 1.9.27: SYS_BRK, a real heap for ring-3 programs.

   The image window (JT_USER_IMAGE_PAGES) is a fixed, kmalloc'd span per
   running window; growing it for one app (Samantha's face frames) pinned
   megabytes of kernel heap for every app. This file gives each task its
   own heap at JT_BRK_BASE instead: pages come from the PMM one frame at a
   time, are zeroed before they are mapped (a fresh frame may hold another
   task's old bytes), and go into that task's page directory only, user
   and writable. Exit and crash both land in syscall_release_task, which
   calls brk_release: every frame back, the page tables too, and one
   serial line with the live count so a check can prove nothing leaked.

   Two caps: JT_BRK_MAX_PAGES per task, and a global floor of free frames
   the heap will not eat below, so one program cannot starve the kernel.

   The syscall runs on the task's CR3. Frames above the 8MB identity map
   are only dereferenceable after paging_map_region put them in the
   kernel's directory, and a task directory is a snapshot that misses
   later additions, so every walk here switches to the kernel directory,
   does its work, and switches back (which also flushes the TLB after the
   PTE edits). */
#include "brk.h"
#include "memmap.h"
#include "paging.h"
#include "pmm.h"
#include "task.h"
#include "../drivers/serial.h"
typedef unsigned int u32;

#define ENOMEM 12
#define EINVAL 22
#define BRK_FLOOR_FRAMES 1024u /* 4MB of free frames the heap leaves alone */
#define BRK_END (JT_BRK_BASE + JT_BRK_MAX_PAGES * 4096u)
#define BRK_PDE0 (JT_BRK_BASE >> 22)
#define BRK_NPDE ((BRK_END - JT_BRK_BASE + 0x3FFFFFu) >> 22)

static u32 brk_top[TASK_SLOTS];
static u32 brk_pages[TASK_SLOTS];
static u32 live_pages;

static void put_dec(char *out, u32 v) {
    char t[12]; int n = 0;
    do { t[n++] = (char)('0' + v % 10); v /= 10; } while (v);
    int i = 0; while (n) out[i++] = t[--n]; out[i] = 0;
}

/* Kernel-side pointer to a pmm frame: the identity alias, mapped on demand past 8MB. */
static u32 *frame_ptr(u32 pa) {
    if (pa >= 0x800000u && !paging_map_region(pa, 0x1000)) return 0;
    return (u32 *)pa;
}

static u32 *table_for(u32 *dir, u32 vaddr, int create) {
    u32 pde = vaddr >> 22;
    if (dir[pde] & 1) return frame_ptr(dir[pde] & ~0xFFFu);
    if (!create) return 0;
    u32 pa = pmm_alloc_frame();
    if (!pa) return 0;
    u32 *t = frame_ptr(pa);
    if (!t) { pmm_free_frame(pa); return 0; }
    for (int i = 0; i < 1024; i++) t[i] = 0x2;
    dir[pde] = pa | 0x7;
    return t;
}

static int map_one(u32 *dir, u32 vaddr) {
    u32 *t = table_for(dir, vaddr, 1);
    if (!t) return 0;
    u32 pa = pmm_alloc_frame();
    if (!pa) return 0;
    u32 *z = frame_ptr(pa);
    if (!z) { pmm_free_frame(pa); return 0; }
    for (int i = 0; i < 1024; i++) z[i] = 0; /* never hand a task another task's old bytes */
    t[(vaddr >> 12) & 0x3FF] = pa | 0x7;
    return 1;
}

static void unmap_one(u32 *dir, u32 vaddr) {
    u32 *t = table_for(dir, vaddr, 0);
    if (!t) return;
    u32 *pte = &t[(vaddr >> 12) & 0x3FF];
    if (*pte & 1) pmm_free_frame(*pte & ~0xFFFu);
    *pte = 0x2;
}

static void free_tables(u32 *dir) {
    for (u32 i = 0; i < BRK_NPDE; i++) {
        if (!(dir[BRK_PDE0 + i] & 1)) continue;
        pmm_free_frame(dir[BRK_PDE0 + i] & ~0xFFFu);
        dir[BRK_PDE0 + i] = 0x2;
    }
}

static u32 enter_kernel_dir(void) {
    u32 cr3; __asm__ volatile ("mov %%cr3, %0" : "=r"(cr3));
    u32 kd = paging_kernel_directory();
    if (cr3 != kd) __asm__ volatile ("mov %0, %%cr3" :: "r"(kd) : "memory");
    return cr3;
}
static void leave_to(u32 cr3) { __asm__ volatile ("mov %0, %%cr3" :: "r"(cr3) : "memory"); }

int brk_set(int task, unsigned int dir_phys, unsigned int new_top) {
    if (task < 0 || task >= TASK_SLOTS || !dir_phys) return -EINVAL;
    u32 top = brk_top[task] ? brk_top[task] : JT_BRK_BASE;
    if (new_top == 0) return (int)top;
    if (new_top < JT_BRK_BASE || new_top > BRK_END) return -EINVAL; /* anything outside the heap range, the image, stack, fb and private window included */
    u32 want = (new_top + 0xFFFu) & ~0xFFFu, have = (top + 0xFFFu) & ~0xFFFu;
    u32 saved = enter_kernel_dir();
    u32 *dir = (u32 *)dir_phys;
    int err = 0;
    if (want > have) {
        u32 need = (want - have) >> 12;
        if (pmm_free_frames() < need + BRK_FLOOR_FRAMES) err = -ENOMEM;
        else for (u32 v = have; v < want; v += 0x1000) {
            if (!map_one(dir, v)) { for (u32 u = have; u < v; u += 0x1000) { unmap_one(dir, u); } err = -ENOMEM; break; }
            brk_pages[task]++; live_pages++;
        }
        if (err) { brk_pages[task] -= (want - have) >> 12 > brk_pages[task] ? brk_pages[task] : 0; }
    } else if (want < have) {
        for (u32 v = want; v < have; v += 0x1000) { unmap_one(dir, v); brk_pages[task]--; live_pages--; }
    }
    if (err) {
        /* rollback already unmapped the partial run; recount from the tables so the counters stay honest */
        u32 n = 0;
        for (u32 v = JT_BRK_BASE; v < have; v += 0x1000) { u32 *t = table_for(dir, v, 0); if (t && (t[(v >> 12) & 0x3FF] & 1)) n++; }
        live_pages -= brk_pages[task]; brk_pages[task] = n; live_pages += n;
    } else brk_top[task] = new_top;
    leave_to(saved);
    return err ? err : (int)brk_top[task];
}

void brk_release(int task, unsigned int dir_phys) {
    if (task < 0 || task >= TASK_SLOTS) return;
    if (!brk_top[task] && !brk_pages[task]) return;
    u32 freed = 0;
    if (dir_phys) {
        u32 saved = enter_kernel_dir();
        u32 *dir = (u32 *)dir_phys;
        u32 have = (brk_top[task] + 0xFFFu) & ~0xFFFu;
        for (u32 v = JT_BRK_BASE; v < have; v += 0x1000) { unmap_one(dir, v); freed++; }
        free_tables(dir);
        leave_to(saved);
    }
    live_pages -= brk_pages[task] < live_pages ? brk_pages[task] : live_pages;
    brk_top[task] = 0; brk_pages[task] = 0;
    char a[12], b[12]; put_dec(a, freed); put_dec(b, live_pages);
    serial_puts("brk: released "); serial_puts(a); serial_puts(" pages, live="); serial_puts(b); serial_puts("\n");
}

unsigned int brk_live_pages(void) { return live_pages; }
