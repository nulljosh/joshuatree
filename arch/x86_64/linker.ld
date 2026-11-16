/* x86-64 slice 1: a flat binary at 1 MiB, the multiboot header first so the loader finds it in the first 8 KiB. */
ENTRY(start32)
SECTIONS {
    . = 0x100000;
    _load_start = .;
    .multiboot : { KEEP(*(.multiboot)) }
    .text : { *(.text*) }
    .rodata : { *(.rodata*) }
    .data : { *(.data*) }
    _load_end = .;
    .bss : ALIGN(4096) { *(.bss*) *(COMMON) }
    _bss_end = .;
    /DISCARD/ : { *(.comment) *(.note*) *(.eh_frame*) }
}
