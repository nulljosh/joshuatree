ENTRY(_start)
SECTIONS {
    . = KBASE;
    .text : { *(.text.boot) *(.text.vectors) *(.text*) }
    .rodata : ALIGN(8) { *(.rodata*) }
    .user : ALIGN(16) { *(.user) }
    .data : ALIGN(8) { *(.data*) }
    .bss : ALIGN(8) { _bss_start = .; *(.bss*) *(COMMON) . = ALIGN(8); _bss_end = .; }
    . = ALIGN(16);
    . += 0x20000;   /* 128 KiB (13.9.3, was 64): a TLS handshake (tls.c) verifies RSA-4096 roots on the stack, under the browser's 1 KiB URLs */
    _stack_top = .;
    . = ALIGN(4096);
    _heap_start = .;
}
