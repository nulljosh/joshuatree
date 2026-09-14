/* The ring-3 payloads. Written in straight-line assembly on purpose:
   ring3.c copies these bytes onto a different page than the one they're
   linked at, so nothing here may reference its own address via the
   linker. Data inside a payload (ring3_msg) is reached through %esi,
   which ring3.c passes in as "this page's real base" on the user stack,
   plus a link-time constant offset (label minus label, fixed by the
   assembler, unaffected by the move). The only jump is the spinner's own
   `jmp 1b`, a relative jump inside the copied bytes, which survives a
   move by definition.

   Calling convention into each payload: ring3.c pushes two words on the
   user stack before the task starts, so at entry 0(%esp) = the marker
   address (base of the user stack page, already user-accessible) and
   4(%esp) = the code page's base. Nothing else is assumed. */
.section .text

/* Normal life: write a line through SYS_WRITE, then SYS_EXIT(42).
   The marker records the kernel's return value (the byte count) tagged
   with 0xABCD0000, written from ring 3 AFTER int 0x80 returned: a value
   only the kernel knew, present in user memory, is the round-trip proof,
   not just "ring 3 ran". */
.global ring3_demo_code
.global ring3_demo_code_end
ring3_demo_code:
    mov (%esp), %edi
    mov 4(%esp), %esi
    mov $4, %eax                                /* SYS_WRITE */
    mov $1, %ebx                                /* fd 1 */
    lea ring3_msg-ring3_demo_code(%esi), %ecx   /* buf: inside this same user page */
    mov $(ring3_msg_end-ring3_msg), %edx        /* len */
    int $0x80
    or $0xABCD0000, %eax
    mov %eax, (%edi)
    mov $1, %eax                                /* SYS_EXIT */
    mov $42, %ebx
    int $0x80
    hlt                                         /* unreachable; a ring-3 hlt is #GP, which idt.c now reaps instead of halting */
ring3_msg:
    .ascii "hello from ring 3 via int 0x80\n"
ring3_msg_end:
ring3_demo_code_end:

/* Bad citizen: the v3 payload, kept exactly, a privileged instruction with
   no I/O bitmap and IOPL 0 is a #GP. v3 halted the whole kernel here; v64
   reaps just this task. The marker still proves it ran before faulting. */
.global ring3_fault_code
.global ring3_fault_code_end
ring3_fault_code:
    mov (%esp), %edi
    movl $0xABCDEF01, (%edi)
    outb %al, $0x80
    hlt
ring3_fault_code_end:

/* Bad pointer: calls SYS_WRITE with buf=NULL, a real user-mode address
   this task has no mapping for, exercising sys_write's paging_user_range_ok
   check (v64's access_ok equivalent) instead of just its happy path. The
   marker records the raw kernel return value (expect -EFAULT, i.e.
   0xFFFFFFF2) BEFORE the exit, same "kernel-known value written back from
   ring 3" proof ring3_demo_code uses, so a kernel that skipped the check
   and let the write proceed (or worse, faulted on an unmapped kernel
   read) is distinguishable from one that correctly rejected it. */
.global ring3_badptr_code
.global ring3_badptr_code_end
ring3_badptr_code:
    mov (%esp), %edi
    mov $4, %eax                                /* SYS_WRITE */
    mov $1, %ebx                                 /* fd 1 */
    xor %ecx, %ecx                               /* buf = NULL: not mapped, not user-accessible */
    mov $10, %edx                                /* len */
    int $0x80
    mov %eax, (%edi)
    mov $1, %eax                                 /* SYS_EXIT */
    mov $7, %ebx                                 /* distinct exit code, checked alongside the marker */
    int $0x80
    hlt
ring3_badptr_code_end:

/* Spinner: never syscalls, never faults, just counts in the marker forever
   at ring 3. Exists to prove a ring-3 task gets preempted like any other
   (the counter moves while the shell has the CPU too) and that task_kill
   can end one that never asked to end. */
.global ring3_spin_code
.global ring3_spin_code_end
ring3_spin_code:
    mov (%esp), %edi
1:  incl (%edi)
    jmp 1b
ring3_spin_code_end:
