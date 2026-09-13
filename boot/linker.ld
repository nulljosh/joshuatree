OUTPUT_FORMAT(elf32-i386)
ENTRY(_start)

/* Higher-half kernel: everything except the very first bit of boot.S is
   linked to run at 0xC0100000+ (VMA) while actually loaded by the
   bootloader at 1MB+ (LMA, via each section's AT()). .text.boot is the one
   piece that must run identically whether paging is on or off yet, so it
   stays unrelocated (VMA == LMA), see boot.S for why. */
SECTIONS {
    . = 1M;
    .text.boot : { *(.multiboot) *(.text.boot) }

    . += 0xC0000000;

    .text   ALIGN(4096) : AT(ADDR(.text)   - 0xC0000000) { *(.text) }
    .rodata ALIGN(4096) : AT(ADDR(.rodata) - 0xC0000000) { *(.rodata*) }
    .data   ALIGN(4096) : AT(ADDR(.data)   - 0xC0000000) { *(.data*) }
    .bss    ALIGN(4096) : AT(ADDR(.bss)    - 0xC0000000) { *(COMMON) *(.bss*) }

    . = ALIGN(4096);
    _kernel_end = . - 0xC0000000; /* physical, matching pmm.c's other physical addresses */
}
