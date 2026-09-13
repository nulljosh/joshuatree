/* IRQ0-15 entry points (vectors 32-47 after the PIC remap). Unlike the
   CPU-exception ISRs these must resume the interrupted code, so they save
   and restore every register and iret instead of halting. */
.macro IRQ_STUB n
.global irq\n
irq\n:
    pusha
    push $\n
    call irq_handler
    add $4, %esp
    popa
    iret
.endm

/* IRQ0 (the PIT timer) additionally drives preemption: after the normal
   tick bookkeeping, esp already points at exactly the pusha+CPU-frame
   shape task.c's schedule() expects and hands back (task.c's own comment
   explains why every task's stack is fabricated to match). Swapping esp
   to schedule()'s return value before popa/iret is the entire preemptive
   switch, whether schedule() picked a different task or handed the same
   esp straight back. yield() reaches this exact path too, via `int $32`
   (same IDT gate, same frame shape), so voluntary and involuntary
   switches are one tested mechanism, not two. */
.section .text
.global irq0
irq0:
    pusha
    push $0
    call irq_handler
    add $4, %esp
    push %esp
    call schedule
    add $4, %esp
    mov %eax, %esp
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
