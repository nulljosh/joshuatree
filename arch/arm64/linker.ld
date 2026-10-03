ENTRY(_start)
SECTIONS {
    . = KBASE;
    .text : { *(.text.boot) *(.text.vectors) *(.text*) }
    .rodata : ALIGN(8) { *(.rodata*) }
    .data : ALIGN(8) { *(.data*) }
    .bss : ALIGN(8) { *(.bss*) *(COMMON) }
    . = ALIGN(16);
    . += 0x4000;
    _stack_top = .;
}
