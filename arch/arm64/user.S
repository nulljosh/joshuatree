/* ARM64 M3a: the first EL0 program. It runs unprivileged and talks to the kernel only through svc. The kernel copies
   everything between user_start and user_end into the user code page, so it is position independent (adr, no absolute
   addresses). Syscall ABI: number in x8 (write 64, exit 93), arguments in x0 to x2, result in x0.
   Entry A (x0 = the address of a kernel-only page): write, check the return value, try to make the kernel write out
   kernel memory (must be refused with -14), then read the kernel page directly (must fault, so the lines after it never
   print). Entry B: write a line and exit with status 7. */
.section .user, "ax"
.global user_start, user_end, user_entry_a, user_entry_b
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
msg_hello: .ascii "EL0: hello from EL0\n"
msg_hello_end:
msg_ret: .ascii "EL0: write returned 20\n"
msg_ret_end:
msg_refused: .ascii "EL0: kernel memory refused, write returned -14\n"
msg_refused_end:
msg_fail: .ascii "EL0: reached code after the blocked access\n"
msg_fail_end:
msg_second: .ascii "EL0: second task\n"
msg_second_end:
.balign 4
user_end:
