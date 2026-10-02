#ifndef MEMMAP_H
#define MEMMAP_H
/* The one place the fixed ring-3 addresses are written down.
 *
 * Three things pin them: kernel/exec.h (the loader), boot/linker.ld (the
 * NOLOAD reservations that keep pmm.c off these frames), and the user link scripts
 * (user/hello.ld and user/note.ld, where flat binaries are linked, since a headerless image runs at one
 * address only). A linker script cannot include a C header, so the
 * Makefile turns these #defines into boot/memmap.ld (one `NAME = value;`
 * line each) and the scripts INCLUDE that. Plain hex, no `u` suffix, so
 * the same text parses in C and in ld: anything else here breaks the
 * sed in the Makefile and the link fails loudly.
 *
 * All three live in the second 4MB kernel page table (0xC0400000 to
 * 0xC07FFFFF) and inside paging.c's 8MB base map; kernel/paging.c's
 * paging_task_map_private copies that one table for a task's private
 * window and refuses any other, so moving them out of it is a paging
 * change, not just a number change. Within it they can slide freely.
 *
 * 2026-10-01: moved up 512KB from 0xC0507000. Every ring-3 app binary is
 * baked into the kernel as rodata and .bss had closed to under 16KB of
 * the window; this leaves about half a megabyte for more apps before
 * tools/checks/bss-margin-check.py complains again.
 *
 * 2026-10-01 (later): the image window grew from 7 pages to
 * JT_USER_IMAGE_PAGES (32, 128KB): Samantha and Stocks had both closed to
 * within 1KB of 28KB. JT_USER_BASE did not move (docs/SYSCALL-ABI.md names
 * it); .dmabuf and .userfb slid up 128KB to make room, and the window end
 * (0xC0730000) still sits inside the second 4MB table. The page count is
 * hex so the Makefile's sed renders it into boot/memmap.ld too. */

#define JT_USER_BASE        0xC0587000 /* program window: JT_USER_IMAGE_PAGES of image, then 1 page of stack (.userimg) */
#define JT_USER_IMAGE_PAGES 0x20       /* 32 pages = 128KB of image (code+data) per ring-3 program; was 7 (28KB) until 2026-10-01, briefly 0x80 for face frames before SYS_BRK */
#define JT_DMABUF_BASE      0xC0610000 /* 64KB Sound Blaster DMA, 64KB aligned (.dmabuf) */
#define JT_USER_FB          0xC0620000 /* ring-3 window framebuffer (.userfb) */
#define JT_USER_FB_BYTES  0x170000   /* 832x450 at 32bpp fits */

/* SYS_BRK (1.9.27): a per-task heap that grows above the image, backed by
 * pmm frames mapped only into that task's directory (kernel/brk.c). The
 * range sits in the top 16MB of the address space, PDEs 0x3FC and 0x3FD:
 * no kernel alias lives there (paging_map_region identity-maps physical
 * RAM and the 0xFD000000 framebuffer, both below), the private window
 * PDE is 19, and task directories are snapshots of the kernel's PDEs, so
 * nothing the kernel maps later can be written over these two slots. */
#define JT_BRK_BASE         0xFF000000 /* first heap byte; brk starts here and grows up */
#define JT_BRK_MAX_PAGES    0x800      /* 2048 pages = 8MB per task */

#endif
