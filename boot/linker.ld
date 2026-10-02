OUTPUT_FORMAT(elf32-i386)
ENTRY(_start)

/* Higher-half kernel: everything except the very first bit of boot.S is
   linked to run at 0xC0100000+ (VMA) while actually loaded by the
   bootloader at 1MB+ (LMA, via each section's AT()). .text.boot is the one
   piece that must run identically whether paging is on or off yet, so it
   stays unrelocated (VMA == LMA), see boot.S for why. */
/* The ring-3 addresses below come from kernel/memmap.h via the generated
   boot/memmap.ld (see the Makefile), so the kernel, the user link scripts
   and this file cannot disagree. */
INCLUDE boot/memmap.ld

SECTIONS {
    . = 1M;
    .text.boot : { *(.multiboot) *(.text.boot) }

    . += 0xC0000000;

    .text   ALIGN(4096) : AT(ADDR(.text)   - 0xC0000000) { *(.text) }
    .rodata ALIGN(4096) : AT(ADDR(.rodata) - 0xC0000000) { *(.rodata*) }
    .data   ALIGN(4096) : AT(ADDR(.data)   - 0xC0000000) { *(.data*) }
    .bss    ALIGN(4096) : AT(ADDR(.bss)    - 0xC0000000) { *(COMMON) *(.bss*) }

    . = ALIGN(4096);

    /* The ring-3 program window (kernel/exec.h's JT_USER_BASE, and the
       address user/hello.ld links a flat binary at). A flat binary has no
       headers and no relocations, so it runs at one address and only that
       address; something has to pin it, and it has to be an address the
       physical memory manager will never hand to the heap.
       NOLOAD, so it costs nothing in kernel.elf: no bytes are stored for
       it, it is a reserved hole. It sits past .bss on purpose and the
       ASSERT below fails the link if .bss ever grows into it, which is a
       loud failure rather than a kernel that quietly loads user code over
       its own data. Declaring it here also pushes _kernel_end past it, so
       pmm.c reserves these frames at boot the same way it reserves the
       rest of the kernel image.
       2026-09-27: moved up 8KB from 0xC0501000 for 1.7.0's integration
       release. 2026-10-01: moved up 512KB with .dmabuf and .userfb (see
       kernel/memmap.h), because every ring-3 app is rodata in this image
       and .bss had closed to under 16KB of the window again. */
    .userimg JT_USER_BASE (NOLOAD) : AT(JT_USER_BASE - 0xC0000000) {
        . += 8 * 4096; /* 7 pages of image + 1 page of user stack */
    }

    /* Sound Blaster 16 DMA buffer (drivers/sb16.c). ISA DMA can only
       address the low 16MB and a transfer must not cross a 64KB boundary,
       so this is 64KB at a 64KB-aligned physical address. NOLOAD, like
       .userimg above: it costs nothing in kernel.elf, and sitting before
       _kernel_end keeps pmm.c from handing these frames out. */
    .dmabuf JT_DMABUF_BASE (NOLOAD) : AT(JT_DMABUF_BASE - 0xC0000000) {
        sb16_dma_buf = .;
        . += 0x10000;
    }

    /* 1.7.7: the ring-3 window framebuffer (kernel/exec.h JT_USER_FB).
       A program that asked for a window through SYS_WINDOW_OPEN draws
       into these pages directly; the kernel copies them into the app
       viewport on SYS_WINDOW_POLL. Big enough for the dock's app window
       (804x345 pixels, 32bpp) and for the Apps folder viewport an app
       launched from the grid runs in (832x450, 0x16DA00 bytes). NOLOAD like the two
       above, and still inside the 8MB base map paging_set_user covers. */
    .userfb JT_USER_FB (NOLOAD) : AT(JT_USER_FB - 0xC0000000) {
        . += JT_USER_FB_BYTES;
    }

    /* 1.9.26: 64KB of user-accessible scratch for ring-3 apps whose arena
       (a flat image has no .bss) is bigger than the 8-page window, Notes
       and Terminal. Private per window task, shared in exec_user. */
    .userheap JT_USER_HEAP (NOLOAD) : AT(JT_USER_HEAP - 0xC0000000) {
        . += JT_USER_HEAP_BYTES;
    }

    . = ALIGN(4096);
    _kernel_end = . - 0xC0000000; /* physical, matching pmm.c's other physical addresses */
}

ASSERT(ADDR(.bss) + SIZEOF(.bss) <= JT_USER_BASE,
       "kernel .bss has grown into the ring-3 program window (JT_USER_BASE); move it up in kernel/memmap.h")
ASSERT(JT_USER_BASE + 8 * 4096 <= JT_DMABUF_BASE, "kernel/memmap.h: .userimg overlaps .dmabuf")
ASSERT((JT_DMABUF_BASE & 0xFFFF) == 0, "kernel/memmap.h: .dmabuf must be 64KB aligned for ISA DMA")
ASSERT(JT_DMABUF_BASE + 0x10000 <= JT_USER_FB, "kernel/memmap.h: .dmabuf overlaps .userfb")
ASSERT(JT_USER_FB + JT_USER_FB_BYTES <= JT_USER_HEAP, "kernel/memmap.h: .userfb overlaps .userheap")
ASSERT(JT_USER_HEAP + JT_USER_HEAP_BYTES <= 0xC0800000, "kernel/memmap.h: .userheap leaves the 4MB table")
ASSERT(JT_USER_FB + JT_USER_FB_BYTES <= 0xC0800000, "kernel/memmap.h: .userfb leaves the 4MB table paging_task_map_private copies")
