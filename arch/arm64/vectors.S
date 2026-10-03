/* ARM64 M1: the exception vector table. A synchronous exception from EL1 goes to exc_sync (a deliberate svc is
   answered, anything else is printed and stops), an interrupt goes to exc_irq, and every other slot goes to exc_bad,
   which prints and stops, so a fault is a line on the UART instead of a silent hang. */
.macro save_all
    sub sp, sp, #272
    stp x0, x1, [sp, #0]
    stp x2, x3, [sp, #16]
    stp x4, x5, [sp, #32]
    stp x6, x7, [sp, #48]
    stp x8, x9, [sp, #64]
    stp x10, x11, [sp, #80]
    stp x12, x13, [sp, #96]
    stp x14, x15, [sp, #112]
    stp x16, x17, [sp, #128]
    stp x18, x19, [sp, #144]
    stp x20, x21, [sp, #160]
    stp x22, x23, [sp, #176]
    stp x24, x25, [sp, #192]
    stp x26, x27, [sp, #208]
    stp x28, x29, [sp, #224]
    str x30, [sp, #240]
    mrs x0, elr_el1
    mrs x1, esr_el1
    stp x0, x1, [sp, #248]
.endm
.macro restore_all
    ldp x0, x1, [sp, #248]
    msr elr_el1, x0
    ldp x0, x1, [sp, #0]
    ldp x2, x3, [sp, #16]
    ldp x4, x5, [sp, #32]
    ldp x6, x7, [sp, #48]
    ldp x8, x9, [sp, #64]
    ldp x10, x11, [sp, #80]
    ldp x12, x13, [sp, #96]
    ldp x14, x15, [sp, #112]
    ldp x16, x17, [sp, #128]
    ldp x18, x19, [sp, #144]
    ldp x20, x21, [sp, #160]
    ldp x22, x23, [sp, #176]
    ldp x24, x25, [sp, #192]
    ldp x26, x27, [sp, #208]
    ldp x28, x29, [sp, #224]
    ldr x30, [sp, #240]
    add sp, sp, #272
.endm
.macro ventry target
    .balign 0x80
    b \target
.endm

.section .text.vectors, "ax"
.balign 0x800
.global vectors
vectors:
    ventry bad_entry        /* current EL, SP0: sync, irq, fiq, serror */
    ventry bad_entry
    ventry bad_entry
    ventry bad_entry
    ventry sync_entry       /* current EL, SPx */
    ventry irq_entry
    ventry bad_entry
    ventry bad_entry
    ventry bad_entry        /* lower EL, AArch64 */
    ventry bad_entry
    ventry bad_entry
    ventry bad_entry
    ventry bad_entry        /* lower EL, AArch32 */
    ventry bad_entry
    ventry bad_entry
    ventry bad_entry

sync_entry:
    save_all
    mov x0, sp
    bl exc_sync
    restore_all
    eret
irq_entry:
    save_all
    mov x0, sp
    bl exc_irq
    restore_all
    eret
bad_entry:
    save_all
    mov x0, sp
    bl exc_bad
1:  wfe
    b 1b
