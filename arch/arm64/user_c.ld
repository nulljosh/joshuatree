/* Link script for a flat ARM64 EL0 program (M3b): no headers, no relocations, no .bss. The kernel copies the whole
   image to the start of the user code page and enters at its first byte, so _start comes first. Position independent
   code (adrp/add, bl) links at 0 and runs wherever that page is. */
ENTRY(_start)
SECTIONS {
    . = 0;
    .text : { *(.text.start) *(.text*) }
    .rodata : ALIGN(8) { *(.rodata*) }
    .data : ALIGN(8) { *(.data*) }
    _user_end = .;
    .bss : { *(COMMON) *(.bss*) }
    .got : { *(.got*) *(.data.rel*) }
    /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) }
}
ASSERT(SIZEOF(.bss) == 0, "a flat user binary cannot have .bss: nothing zeroes it")
ASSERT(SIZEOF(.got) == 0, "a GOT or absolute pointer table means the code is not position independent")
ASSERT(_user_end <= 4096, "the user binary must fit the one user code page")
