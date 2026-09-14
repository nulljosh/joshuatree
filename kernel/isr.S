/* 32 CPU-exception entry points plus the int 0x80 syscall gate (v64).

   Exceptions: each stub pushes a vector number and jumps to one shared
   tail that calls isr_handler(vector, err, eip, cs, eflags) cdecl-style,
   straight off the CPU's own frame. Vectors 8,10-14,17 have the CPU push
   an error code first; the others push a dummy 0 so the frame has one
   fixed shape and the C side can read CS at a fixed slot (the standard
   OSDev/James Molloy "push byte 0" trick). CS is what matters now: its
   RPL says whether the fault came from ring 3 (a task the kernel can reap
   and keep going) or ring 0 (a real kernel bug, still report and halt).
   isr_handler never returns in either case, so no register save/iret.

   Every path first forces ds/es/fs/gs to the kernel data selector: a
   fault or syscall arriving from ring 3 lands here with those still set
   to the ring-3 data selector, since they're not part of the CPU's own
   privilege-transition frame (only SS/ESP/EFLAGS/CS/EIP are). The flat
   GDT means kernel memory access happens to still work either way, but
   that's an accident of both data segments covering the same 4GB, not a
   design; this makes it correct instead.

   The syscall gate does the same thing, but saves the ring-3 values first
   and restores them on the way out, because unlike an exception it does
   resume the caller. That restore is load-bearing, not tidiness: iret to
   a lower privilege level silently nulls any of DS/ES/FS/GS still holding
   a selector more privileged than the new CPL (Intel SDM vol. 3, IRET,
   "return to outer privilege level"), so returning to ring 3 with the
   kernel's 0x10 still loaded would leave the user code with a null DS and
   a #GP on its very next memory access. */
.macro ISR_NOERR n
.global isr\n
isr\n:
    push $0
    push $\n
    jmp isr_common
.endm

.macro ISR_ERR n
.global isr\n
isr\n:
    push $\n
    jmp isr_common
.endm

.section .text
isr_common:
    mov $0x10, %ax
    mov %ax, %ds
    mov %ax, %es
    mov %ax, %fs
    mov %ax, %gs
    call isr_handler    /* args: [vector][err][eip][cs][eflags] already on the stack */
1:  cli
    hlt
    jmp 1b

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

/* int 0x80: x86 Linux convention, eax = syscall number, ebx/ecx/edx = the
   first three arguments, result back in eax. The frame this builds is
   exactly struct syscall_frame in syscall.h (pusha's 8, then the 4 data
   segments pushed after them so they sit lowest), and syscall_dispatch
   writes the return value into the frame's eax slot, which popa then
   hands back to the caller. Installed as an interrupt gate with DPL 3
   (0xEE in idt.c): DPL 3 so ring 3 may raise it at all (with the 0x8E
   the exception gates use, a user-mode `int $0x80` is itself a #GP), an
   interrupt gate rather than a trap gate so IF is clear for the whole
   handler, which is what makes sys_exit's free-then-switch window safe
   by construction rather than by a cli it has to remember. */
.global syscall_entry
syscall_entry:
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
    push %esp           /* struct syscall_frame * */
    call syscall_dispatch
    add $4, %esp
    pop %gs
    pop %fs
    pop %es
    pop %ds
    popa
    iret
