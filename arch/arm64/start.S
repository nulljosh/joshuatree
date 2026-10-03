/* Joshua Tree on ARM64, M0: the first instruction. QEMU's virt machine loads the image at
   0x40000000 and enters at EL1 with the MMU off. Park every core but 0, set a stack, call main. */
.section .text.boot
.global _start
_start:
    mrs x0, mpidr_el1
    and x0, x0, #3
    cbnz x0, park
    ldr x0, =_stack_top
    mov sp, x0
    bl main
park:
    wfe
    b park
