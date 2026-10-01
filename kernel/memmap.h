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
 * tools/checks/bss-margin-check.py complains again. */

#define JT_USER_BASE      0xC0587000 /* 8 pages: 7 of image, 1 of stack (.userimg) */
#define JT_DMABUF_BASE    0xC0590000 /* 64KB Sound Blaster DMA, 64KB aligned (.dmabuf) */
#define JT_USER_FB        0xC05A0000 /* ring-3 window framebuffer (.userfb) */
#define JT_USER_FB_BYTES  0x170000   /* 832x450 at 32bpp fits */

#endif
