/* v64 (0.61.0): int 0x80 dispatch, see syscall.h for the ABI. One table
   keyed by eax, the same shape as Linux's sys_call_table, with every
   unassigned slot a null that dispatch turns into -ENOSYS rather than a
   jump into nothing. Runs with interrupts off (interrupt gate, see isr.S),
   on the calling task's own kernel stack (gdt.c's TSS esp0, repointed by
   task.c's schedule() on every switch). */
#include "syscall.h"
#include "idt.h"
#include "task.h"
#include "paging.h"
#include "console.h"
#include "serial.h"

typedef unsigned int u32;
typedef int (*syscall_fn)(u32 a, u32 b, u32 c);

#define EBADF   9
#define EFAULT  14
#define ENOSYS  38

extern void syscall_entry(void);

static int sys_exit(u32 code, u32 b, u32 c) {
    (void)b; (void)c;
    serial_puts("syscall: exit from ring 3\n");
    task_exit_with((int)code); /* never returns: frees this task's own stack + directory, reschedules away */
    return 0;
}

/* write(fd, buf, len): fd 1/2 both go to the console (there's one). The
   pointer is a ring-3 address the kernel is about to read, so it's checked
   against the page tables first, the same job Linux's access_ok() does
   before copy_from_user: every page of [buf, buf+len) must actually be
   user-accessible, otherwise a user program could hand the kernel any
   kernel address and have it echoed back. Bounded to one line per call
   so the copy has a fixed on-stack buffer; longer output is more calls. */
#define WRITE_MAX 255
static int sys_write(u32 fd, u32 buf, u32 len) {
    if (fd != 1 && fd != 2) return -EBADF;
    if (len > WRITE_MAX) len = WRITE_MAX;
    if (!paging_user_range_ok(buf, len)) return -EFAULT;
    char tmp[WRITE_MAX + 1];
    const char *src = (const char *)buf;
    u32 n = 0;
    for (; n < len; n++) { char ch = src[n]; if (!ch) break; tmp[n] = ch; }
    tmp[n] = 0;
    puts(tmp);
    serial_puts("syscall: write(");
    serial_puts(fd == 1 ? "1" : "2");
    serial_puts(") from ring 3: ");
    serial_puts(tmp);
    if (n == 0 || tmp[n - 1] != '\n') serial_puts("\n");
    return (int)n;
}

static const syscall_fn table[NSYSCALLS] = {
    [SYS_EXIT]  = sys_exit,
    [SYS_WRITE] = sys_write,
};

void syscall_dispatch(struct syscall_frame *f) {
    u32 n = f->eax;
    if (n < NSYSCALLS && table[n]) {
        f->eax = (u32)table[n](f->ebx, f->ecx, f->edx);
    } else {
        serial_puts("syscall: unknown number, -ENOSYS\n");
        f->eax = (u32)-ENOSYS;
    }
}

void syscall_install(void) {
    /* 0xEE: present, DPL 3, 32-bit interrupt gate. The DPL is the part
       that's easy to get wrong and hard to notice: with the exception
       gates' 0x8E a user-mode `int $0x80` is itself a general-protection
       fault, which now reaps the task, so it would look like a crashing
       program, not a misconfigured gate (OSDev wiki, "System Calls"). */
    idt_set_gate(0x80, (u32)syscall_entry, 0x08, 0xEE);
}
