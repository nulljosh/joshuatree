/* 32 CPU-exception entry points. Each pushes its own vector number and calls
   isr_handler(vector) cdecl-style; the handler never returns (halts), so no
   iret/register-save machinery is needed yet. Vectors 8,10-14,17 have the CPU
   push an error code first -- we ignore it since we only ever report+halt.
   Every path first forces ds/es/fs/gs to the kernel data selector: since
   ring-3 user mode exists now, an exception (a general-protection fault on
   a privileged instruction, most of all) can arrive with those still set to
   the ring-3 data selector from whatever code just faulted. The flat GDT
   means isr_handler's own memory access happens to still work either way,
   but relying on that would be an accident, not a design; this makes it
   correct instead. No save/restore needed since these paths never resume
   the interrupted context anyway. */
.macro ISR_NOERR n
.global isr\n
isr\n:
    push $\n
    mov $0x10, %ax
    mov %ax, %ds
    mov %ax, %es
    mov %ax, %fs
    mov %ax, %gs
    call isr_handler
1:  cli
    hlt
    jmp 1b
.endm

.macro ISR_ERR n
.global isr\n
isr\n:
    push $\n
    mov $0x10, %ax
    mov %ax, %ds
    mov %ax, %es
    mov %ax, %fs
    mov %ax, %gs
    call isr_handler
1:  cli
    hlt
    jmp 1b
.endm

.section .text
ISR_NOERR 0
ISR_NOERR 1
ISR_NOERR 2
ISR_NOERR 3
ISR_NOERR 4
ISR_NOERR 5
ISR_NOERR 6
ISR_NOERR 7
ISR_ERR   8
ISR_NOERR 9
ISR_ERR   10
ISR_ERR   11
ISR_ERR   12
ISR_ERR   13
ISR_ERR   14
ISR_NOERR 15
ISR_NOERR 16
ISR_ERR   17
ISR_NOERR 18
ISR_NOERR 19
ISR_NOERR 20
ISR_ERR   21
ISR_NOERR 22
ISR_NOERR 23
ISR_NOERR 24
ISR_NOERR 25
ISR_NOERR 26
ISR_NOERR 27
ISR_NOERR 28
ISR_ERR   29
ISR_ERR   30
ISR_NOERR 31
