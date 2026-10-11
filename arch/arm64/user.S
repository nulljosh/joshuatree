/* ARM64 M3a: the first EL0 program. It runs unprivileged and talks to the kernel only through svc. The kernel copies
   everything between user_start and user_end into the user code page, so it is position independent (adr, no absolute
   addresses). Syscall ABI: number in x8 (write 64, exit 93), arguments in x0 to x2, result in x0.
   Entry A (x0 = the address of a kernel-only page): write, check the return value, try to make the kernel write out
   kernel memory (must be refused with -14), then read the kernel page directly (must fault, so the lines after it never
   print). Entry C (x1 = a kernel text address): branch there, which must be an instruction abort. Entry B: write a line and exit with status 7.
   Entry D (M3c): the probe of a loaded C program. x0 points at four words the kernel put in this stack page: a word in the
   program's zeroed pool, the first word of its data page (a relocated pointer), the first byte of its code, and the size of
   its window. It checks the relocation landed inside the image, the pool reads zero, the pool and the data are writable, JT_SYS_TIME
   stores through a pointer into the pool but refuses one into the code page, JT_SYS_GETPID answers, then stores to the code page, which must fault. */
.macro say m
    mov x0, #1
    adr x1, \m
    mov x2, #(\m\()_end - \m)
    mov x8, #64
    svc #0
.endm
.section .user, "ax"
.global user_start, user_end, user_entry_a, user_entry_b, user_entry_c, user_entry_d
user_start:
user_entry_a:
    sub sp, sp, #16
    str x0, [sp]                /* the EL0 stack works */
    ldr x19, [sp]
    mov x0, #1
    adr x1, msg_hello
    mov x2, #(msg_hello_end - msg_hello)
    mov x8, #64
    svc #0
    cmp x0, #(msg_hello_end - msg_hello)
    b.ne fail
    mov x0, #1
    adr x1, msg_ret
    mov x2, #(msg_ret_end - msg_ret)
    mov x8, #64
    svc #0
    mov x0, #1                  /* write(1, kernel page, 16): the kernel must refuse */
    mov x1, x19
    mov x2, #16
    mov x8, #64
    svc #0
    cmn x0, #14
    b.ne fail
    mov x0, #1
    adr x1, msg_refused
    mov x2, #(msg_refused_end - msg_refused)
    mov x8, #64
    svc #0
    mov x0, #1                  /* write(1, 0xFFFFFFFFFFFFFF80, 256): the range check must not wrap */
    mov x1, #-128
    mov x2, #256
    mov x8, #64
    svc #0
    cmn x0, #14
    b.ne fail
    mov x0, #1
    adr x1, msg_wrap
    mov x2, #(msg_wrap_end - msg_wrap)
    mov x8, #64
    svc #0
    ldr x2, [x19]               /* a load from a kernel-only page: the fault ends this program */
fail:
    mov x0, #1
    adr x1, msg_fail
    mov x2, #(msg_fail_end - msg_fail)
    mov x8, #64
    svc #0
    mov x0, #1
    mov x8, #93
    svc #0
1:  b 1b
user_entry_c:                   /* x1 = a kernel text address: branching there must fault */
    br x1
    b fail
user_entry_b:
    mov x0, #1
    adr x1, msg_second
    mov x2, #(msg_second_end - msg_second)
    mov x8, #64
    svc #0
    mov x0, #7
    mov x8, #93
    svc #0
2:  b 2b
user_entry_d:
    ldp x19, x20, [x0]          /* x19 = a word in the zeroed pool, x20 = the first data word */
    ldp x21, x22, [x0, #16]     /* x21 = the first code byte, x22 = the size of the window */
    ldr x2, [x20]
    sub x2, x2, x21
    cmp x2, x22
    b.hs fail                   /* the relocated pointer is not inside the image */
    say msg_reloc
    ldr x2, [x19]
    cbnz x2, fail               /* the pool must read zero */
    say msg_bss
    movz x3, #0x5a5a
    movk x3, #0x5a5a, lsl #16
    str x3, [x19]
    ldr x4, [x19]
    cmp x3, x4
    b.ne fail
    str x3, [x20]
    ldr x4, [x20]
    cmp x3, x4
    b.ne fail
    say msg_rw
    mov x0, x19                 /* time(&pool word): the kernel stores the seconds through it and returns them */
    mov x8, #13
    svc #0
    ldr w4, [x19]
    cmp w0, w4
    b.ne fail
    say msg_time
    mov x0, x21                 /* time(&code byte): refused, the kernel must not store into a read-only page */
    mov x8, #13
    svc #0
    cmn x0, #14
    b.ne fail
    say msg_timefault
    mov x0, #0                  /* getpid */
    mov x8, #20
    svc #0
    cmp x0, #0
    b.le fail
    say msg_pid
    str x3, [x21]               /* a store to the code page: the fault ends the program */
    b fail
msg_hello: .ascii "EL0: hello from EL0\n"
msg_hello_end:
msg_ret: .ascii "EL0: write returned 20\n"
msg_ret_end:
msg_refused: .ascii "EL0: kernel memory refused, write returned -14\n"
msg_refused_end:
msg_wrap: .ascii "EL0: wrapped pointer refused, write returned -14\n"
msg_wrap_end:
msg_reloc: .ascii "EL0: relocated pointer in the data page points into the image\n"
msg_reloc_end:
msg_bss: .ascii "EL0: pool reads zero\n"
msg_bss_end:
msg_rw: .ascii "EL0: data page and pool are writable\n"
msg_rw_end:
msg_time: .ascii "EL0: time stored through a pool pointer\n"
msg_time_end:
msg_timefault: .ascii "EL0: time into a code page refused, returned -14\n"
msg_timefault_end:
msg_pid: .ascii "EL0: getpid answered\n"
msg_pid_end:
msg_fail: .ascii "EL0: reached code after the blocked access\n"
msg_fail_end:
msg_second: .ascii "EL0: second task\n"
msg_second_end:
.balign 4
user_end:
