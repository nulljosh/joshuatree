/* IRQ0-15 entry points (vectors 32-47 after the PIC remap). Unlike the
   CPU-exception ISRs these must resume the interrupted code, so they save
   and restore every register and iret instead of halting.

   v64: the four data segment registers are part of the saved frame now,
   not just the general registers. Ring-3 tasks are real scheduled tasks
   since v64, so the code irq0 interrupts can be ring 3 (segments 0x23)
   and the code it resumes can be a different task at a different ring.
   Two real reasons, both from the Intel SDM rather than tidiness: the
   handler itself should run on the kernel selector, not whatever the
   interrupted code left loaded; and iret to ring 3 nulls any of
   DS/ES/FS/GS still holding a DPL-0 selector, so a ring-3 task resumed
   with the kernel's 0x10 still in DS would #GP on its next memory access.
   Saving each task's own values in its own frame and popping them back is
   the standard fix (OSDev "Interrupt Service Routines", xv6's trapframe). */
.macro IRQ_STUB n
.global irq\n
irq\n:
    pusha
    push %ds
    push %es
    push %fs
    push %gs
    mov $0x10, %ax
    mov %ax, %ds
    mov %ax, %es
    mov %ax, %fs
    mov %ax, %gs
    push $\n
    call irq_handler
    add $4, %esp
    pop %gs
    pop %fs
    pop %es
    pop %ds
    popa
    iret
.endm

/* IRQ0 (the PIT timer) additionally drives preemption: after the normal
   tick bookkeeping, esp already points at exactly the pusha+segments+
   CPU-frame shape task.c's schedule() expects and hands back (task.c's
   own comment explains why every task's stack is fabricated to match).
   Swapping esp to schedule()'s return value before the pops is the entire
   preemptive switch, whether schedule() picked a different task or handed
   the same esp straight back. yield() reaches this exact path too, via
   `int $32` (same IDT gate, same frame shape), so voluntary and
   involuntary switches are one tested mechanism, not two. When the frame
   being resumed belongs to a ring-3 task, the final iret sees CS with
   RPL 3 and pops the extra user ESP/SS pair the CPU pushed on entry,
   exactly the frame task_create_user() fabricates. */
.section .text
.global irq0
irq0:
    pusha
    push %ds
    push %es
    push %fs
    push %gs
    mov $0x10, %ax
    mov %ax, %ds
    mov %ax, %es
    mov %ax, %fs
    mov %ax, %gs
    push $0
    call irq_handler
    add $4, %esp
    push %esp
    call schedule
    add $4, %esp
    mov %eax, %esp
    pop %gs
    pop %fs
    pop %es
    pop %ds
    popa
    iret

IRQ_STUB 1
IRQ_STUB 2
IRQ_STUB 3
IRQ_STUB 4
IRQ_STUB 5
IRQ_STUB 6
IRQ_STUB 7
IRQ_STUB 8
IRQ_STUB 9
IRQ_STUB 10
IRQ_STUB 11
IRQ_STUB 12
IRQ_STUB 13
IRQ_STUB 14
IRQ_STUB 15
