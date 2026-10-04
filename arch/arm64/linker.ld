ENTRY(_start)
SECTIONS {
    . = KBASE;
    .text : { *(.text.boot) *(.text.vectors) *(.text*) }
    .rodata : ALIGN(8) { *(.rodata*) }
    .user : ALIGN(16) { *(.user) }
    .data : ALIGN(8) { *(.data*) }
    .bss : ALIGN(8) { _bss_start = .; *(.bss*) *(COMMON) . = ALIGN(8); _bss_end = .; }
    . = ALIGN(16);
    . += 0x4000;
    _stack_top = .;
    . = ALIGN(4096);
    _heap_start = .;
}
