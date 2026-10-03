ENTRY(_start)
SECTIONS {
    . = KBASE;
    .text : { *(.text.boot) *(.text*) }
    .rodata : { *(.rodata*) }
    .data : { *(.data*) }
    .bss : { *(.bss*) *(COMMON) }
    . = ALIGN(16);
    . += 0x4000;
    _stack_top = .;
}
