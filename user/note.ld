/* Link script for user/note.c, the v2 reference program.
 *
 * Deliberately a separate file from user/hello.ld rather than a shared
 * one. The two say the same thing today, and the duplication is the
 * point: hello.ld is part of the frozen v1 surface that
 * tools/checks/usertest-check.sh reads directly, and merging them would
 * mean a v2 change could move the v1 program's link address. Both
 * addresses are asserted against kernel/exec.h and boot/linker.ld by the
 * checks, so a drift between them fails loudly rather than loading code
 * at an address it was not linked for.
 *
 * Everything else is hello.ld's reasoning and applies unchanged: a flat
 * binary has no headers and no relocations, so it runs at JT_USER_BASE
 * and only there; .text.start comes first so _start sits at offset 0,
 * because exec_user() jumps straight to the load address; and there is no
 * .bss, because nothing zeroes one in a headerless image.
 */
ENTRY(_start)

SECTIONS {
    . = 0xC0500000;

    .text : {
        *(.text.start)
        *(.text*)
    }
    .rodata ALIGN(16) : { *(.rodata*) }
    .data   ALIGN(16) : { *(.data*) }
    _user_end = .;

    .bss ALIGN(16) : { *(COMMON) *(.bss*) }
    _user_bss_size = SIZEOF(.bss);

    /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) }
}

ASSERT(_user_end - 0xC0500000 <= 0x7000,
       "user flat binary is larger than the 7-page image window exec_user() loads into")
ASSERT(_user_bss_size == 0,
       "a flat user binary cannot have .bss: nothing zeroes it, see user/hello.ld")
