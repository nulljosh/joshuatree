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
/* An interrupt can land in the middle of floating point work (the TrueType rasterizer in text.c, or anything later), so
   irq_entry also keeps q0-q31, FPCR and FPSR: 528 bytes, restored before eret. Without it a handler that touched the FP
   registers would corrupt the interrupted code with no fault to show for it. The sync paths do not need it: only the
   interrupt can arrive at a point the code did not choose. (The file builds with -mgeneral-regs-only, so the FP
   extension is switched on for these instructions alone; FP_NOSAVE is the arm64-fp-check.py control build.) */
.arch_extension fp
.macro save_fp
    sub sp, sp, #528
    stp q0, q1, [sp, #0]
    stp q2, q3, [sp, #32]
    stp q4, q5, [sp, #64]
    stp q6, q7, [sp, #96]
    stp q8, q9, [sp, #128]
    stp q10, q11, [sp, #160]
    stp q12, q13, [sp, #192]
    stp q14, q15, [sp, #224]
    stp q16, q17, [sp, #256]
    stp q18, q19, [sp, #288]
    stp q20, q21, [sp, #320]
    stp q22, q23, [sp, #352]
    stp q24, q25, [sp, #384]
    stp q26, q27, [sp, #416]
    stp q28, q29, [sp, #448]
    stp q30, q31, [sp, #480]
    mrs x1, fpcr
    mrs x2, fpsr
    str x1, [sp, #512]
    str x2, [sp, #520]
.endm
.macro restore_fp
    ldr x1, [sp, #512]
    ldr x2, [sp, #520]
    msr fpcr, x1
    msr fpsr, x2
    ldp q0, q1, [sp, #0]
    ldp q2, q3, [sp, #32]
    ldp q4, q5, [sp, #64]
    ldp q6, q7, [sp, #96]
    ldp q8, q9, [sp, #128]
    ldp q10, q11, [sp, #160]
    ldp q12, q13, [sp, #192]
    ldp q14, q15, [sp, #224]
    ldp q16, q17, [sp, #256]
    ldp q18, q19, [sp, #288]
    ldp q20, q21, [sp, #320]
    ldp q22, q23, [sp, #352]
    ldp q24, q25, [sp, #384]
    ldp q26, q27, [sp, #416]
    ldp q28, q29, [sp, #448]
    ldp q30, q31, [sp, #480]
    add sp, sp, #528
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
    ventry el0_sync_entry   /* lower EL, AArch64: svc and faults from EL0 */
    ventry irq_entry
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
el0_sync_entry:
    save_all
    mov x0, sp
    bl exc_el0_sync
    restore_all
    eret
irq_entry:
    save_all
    mov x0, sp
#ifndef FP_NOSAVE
    save_fp
#endif
    bl exc_irq
#ifndef FP_NOSAVE
    restore_fp
#endif
    restore_all
    eret
bad_entry:
    save_all
    mov x0, sp
    bl exc_bad
1:  wfe
    b 1b

/* M3a: enter_user(entry, sp, arg, arg2) saves the kernel's callee-saved registers, stack and interrupt mask, then drops to EL0
   at entry with sp_el0 = sp, x0 = arg, x1 = arg2 and every other register cleared. leave_user(status) is how a program ends (exit or a
   fault): it restores that state and returns from enter_user with status in x0. */
.section .text
.global enter_user
enter_user:
    mov x6, x3                  /* arg2 survives the context save below */
    adrp x3, k_ctx
    add x3, x3, :lo12:k_ctx
    stp x19, x20, [x3]
    stp x21, x22, [x3, #16]
    stp x23, x24, [x3, #32]
    stp x25, x26, [x3, #48]
    stp x27, x28, [x3, #64]
    stp x29, x30, [x3, #80]
    mov x4, sp
    mrs x5, daif
    stp x4, x5, [x3, #96]
    msr elr_el1, x0
    msr sp_el0, x1
    mov x4, #0x3c0              /* EL0t, interrupts masked */
    msr spsr_el1, x4
    mov x0, x2
    mov x1, x6
    mov x2, xzr
    mov x3, xzr
    mov x4, xzr
    mov x5, xzr
    mov x6, xzr
    mov x7, xzr
    mov x8, xzr
    mov x9, xzr
    mov x10, xzr
    mov x11, xzr
    mov x12, xzr
    mov x13, xzr
    mov x14, xzr
    mov x15, xzr
    mov x16, xzr
    mov x17, xzr
    mov x18, xzr
    mov x19, xzr
    mov x20, xzr
    mov x21, xzr
    mov x22, xzr
    mov x23, xzr
    mov x24, xzr
    mov x25, xzr
    mov x26, xzr
    mov x27, xzr
    mov x28, xzr
    mov x29, xzr
    mov x30, xzr
    eret
.global leave_user
leave_user:
    adrp x3, k_ctx
    add x3, x3, :lo12:k_ctx
    ldp x19, x20, [x3]
    ldp x21, x22, [x3, #16]
    ldp x23, x24, [x3, #32]
    ldp x25, x26, [x3, #48]
    ldp x27, x28, [x3, #64]
    ldp x29, x30, [x3, #80]
    ldp x4, x5, [x3, #96]
    mov sp, x4
    msr daif, x5
    ret
.section .bss
.balign 16
k_ctx: .skip 112
