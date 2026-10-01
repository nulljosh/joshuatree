/* Link script for a Joshua Tree flat user binary.
 *
 * A flat binary has no headers and no relocations, so it only works at one
 * address: the one the kernel loads it at. That address is JT_USER_BASE
 * (kernel/memmap.h), inside the kernel's base identity map, on pages
 * exec_user() marks user-accessible before the task starts. It comes in
 * through boot/memmap.ld, which the Makefile generates from that header,
 * so there is one source of truth; tools/checks/usertest-check.sh still
 * asserts the scripts include it.
 *
 * .text.start comes first so _start sits at offset 0: exec_user() jumps
 * straight to the load address, there is no ELF header for it to read an
 * entry point out of.
 */
INCLUDE boot/memmap.ld
ENTRY(_start)

SECTIONS {
    . = JT_USER_BASE;

    .text : {
        *(.text.start)
        *(.text*)
    }
    .rodata ALIGN(16) : { *(.rodata*) }
    .data   ALIGN(16) : { *(.data*) }
    /* No .bss on purpose. With --oformat binary a SHT_NOBITS section is
       not written into the image at all, and there is no crt0 here to
       zero it at startup, so a program with zero-initialised globals
       would silently read whatever the previous program left on that
       page. The ASSERT below catches it if one ever appears. */
    _user_end = .;

    .bss ALIGN(16) : { *(COMMON) *(.bss*) }
    _user_bss_size = SIZEOF(.bss);

    /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) }
}

ASSERT(_user_end - JT_USER_BASE <= 0x7000,
       "user flat binary is larger than the 7-page image window exec_user() loads into")
ASSERT(_user_bss_size == 0,
       "a flat user binary cannot have .bss: nothing zeroes it, see the comment above")

