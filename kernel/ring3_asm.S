/* The actual ring-3 payload, plus the transition into it. Written in
   straight-line assembly (no jumps or calls inside the payload) on purpose:
   ring3.c copies these bytes onto a different page than the one they're
   linked at, and a PC-relative jump would break across that move, an
   absolute reference to a C global like ring3_marker doesn't, since only
   the code moves, the global's real address never does. */
.section .text

/* %edi holds a marker address the caller already mapped user-accessible
   (enter_ring3 loads it before iret): writing straight to a C global by
   name here would target wherever the linker put it, almost certainly a
   page that's still supervisor-only, and a real early test build proved
   exactly that (a page fault on the write, before ever reaching outb). */
.global ring3_demo_code
.global ring3_demo_code_end
ring3_demo_code:
    movl $0xABCDEF01, (%edi)   /* proof this really ran as ring-3 code before faulting */
    outb %al, $0x80             /* privileged: no I/O bitmap, any ring-3 in/out faults (#GP) */
    hlt                          /* unreachable in practice; a safety net if it somehow didn't */
ring3_demo_code_end:

/* enter_ring3(eip, esp, marker_addr): cdecl, never returns. Builds the
   iret frame a privilege-raising interrupt return needs (SS, ESP, EFLAGS,
   CS, EIP) and sets DS/ES/FS/GS to the ring-3 data selector first, matching
   what the payload will actually run under. */
.global enter_ring3
enter_ring3:
    mov 4(%esp), %eax   /* eip (arg1) */
    mov 8(%esp), %ecx   /* esp (arg2) */
    mov 12(%esp), %edi  /* marker_addr (arg3) */

    mov $0x23, %dx       /* ring-3 data selector | RPL 3 */
    mov %dx, %ds
    mov %dx, %es
    mov %dx, %fs
    mov %dx, %gs

    push $0x23           /* SS */
    push %ecx            /* ESP */
    pushf
    pop %edx
    or $0x200, %edx       /* keep IF set */
    push %edx             /* EFLAGS */
    push $0x1B            /* CS: ring-3 code selector | RPL 3 */
    push %eax             /* EIP */
    iret
