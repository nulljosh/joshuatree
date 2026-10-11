/* M3b: user/hello.c built for aarch64 (hello_c.bin, a flat position independent image, see the Makefile), baked into
   the kernel. main.c copies it into the user code page and runs it at EL0, the way it runs user.S. */
    .section .rodata.hello_c, "a"
    .balign 16
    .global hello_c_start, hello_c_end
hello_c_start:
    .incbin "hello_c.bin"
hello_c_end:
