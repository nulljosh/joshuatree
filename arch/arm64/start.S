/* Joshua Tree on ARM64, the first instruction. Two places load this:
   QEMU's virt machine (image at 0x40000000, enters at EL1) and a real Raspberry Pi 4 (the firmware
   puts kernel8.img at 0x80000 and enters at EL2). Either way: park every core but 0, drop to EL1
   with the MMU off, set a stack, call main. The EL the firmware gave us is kept in boot_el. */
.section .text.boot
.global _start
_start:
    mrs x0, mpidr_el1
    and x0, x0, #3
    cbnz x0, park
    mrs x0, CurrentEL
    lsr x0, x0, #2
    adrp x1, boot_el
    str w0, [x1, :lo12:boot_el]
    cmp x0, #2
    b.ne at_el1
    /* EL2 to EL1: EL1 runs aarch64, nothing trapped, EL1 sees the timer, interrupts masked, return into el1_entry */
    mov x0, #(1 << 31)
    msr hcr_el2, x0
    mov x0, #3
    msr cnthctl_el2, x0
    msr cntvoff_el2, xzr
    mov x0, #0x33ff
    msr cptr_el2, x0
    msr hstr_el2, xzr
    mov x0, #0x0800
    movk x0, #0x30d0, lsl #16
    msr sctlr_el1, x0
    mov x0, #0x3c5
    msr spsr_el2, x0
    adr x0, at_el1
    msr elr_el2, x0
    eret
at_el1:
    ldr x0, =_stack_top
    mov sp, x0
    bl main
park:
    wfe
    b park

.section .data
.global boot_el
boot_el: .word 0
