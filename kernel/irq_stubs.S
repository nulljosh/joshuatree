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

.section .text
IRQ_STUB 0
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
