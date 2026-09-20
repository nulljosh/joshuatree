#ifndef SYSCALL_H
#define SYSCALL_H
/* The int 0x80 syscall ABI. x86 Linux convention, borrowed rather than
   invented: eax = syscall number, ebx/ecx/edx = arguments 1-3, result
   returned in eax, negative errno on failure. The numbers below are Linux
   i386's own (__NR_exit = 1, __NR_write = 4), so anything written against
   the real ABI's calling convention lines up here too.
   docs/SYSCALL-ABI.md is the contract; this header and syscall.c are its
   implementation, and user/jtsys.h is the only thing a ring-3 program
   needs to speak it.
   v64 (0.61.0) shipped exit and write. The rest of the v1 set (read, open,
   close, time, getpid, sched_yield) lands here alongside user/hello.c, the
   first real external caller of any of it. */

#define SYS_EXIT         1   /* ebx = exit code; ends the calling task, never returns */
#define SYS_READ         3   /* ebx = fd, ecx = buf, edx = len; bytes read, 0 at end of file */
#define SYS_WRITE        4   /* ebx = fd (1 or 2 only), ecx = buf, edx = len; returns bytes written */
#define SYS_OPEN         5   /* ebx = path, ecx = flags (0 only); returns a per-task fd >= 3 */
#define SYS_CLOSE        6   /* ebx = fd; 0, or -EBADF */
#define SYS_TIME        13   /* ebx = 0, or a user unsigned* to store into; returns seconds since the epoch */
#define SYS_GETPID      20   /* returns the calling task's slot id */
#define SYS_SCHED_YIELD 158  /* gives up the rest of this quantum; returns 0 */

/* Sized by the highest number in the v1 set (158, sched_yield) rounded up
   to the next multiple of 32, which is the only real reason to pick 160
   over 159: it keeps the table a whole number of cache lines and leaves
   room for the next few Linux numbers worth adopting without another
   resize. It is a table of function pointers, 640 bytes of .bss, so
   size here is not a cost worth optimising. Every slot that is not
   assigned below is a null the dispatcher turns into -ENOSYS rather than
   a jump into nothing, and any number >= NSYSCALLS gets the same answer,
   so the gaps in Linux's numbering cost nothing and hide nothing. */
#define NSYSCALLS 160

/* Exactly the stack shape syscall_entry (isr.S) builds, lowest address
   first: the four data segments pushed last, pusha's eight, then the CPU's
   own privilege-change frame. useresp/ss only exist when the caller was
   ring 3; nothing here reads them. */
struct syscall_frame {
    unsigned int gs, fs, es, ds;
    unsigned int edi, esi, ebp, esp_at_pusha, ebx, edx, ecx, eax;
    unsigned int eip, cs, eflags;
    unsigned int useresp, ss;
};

void syscall_install(void); /* wires vector 0x80 into the IDT with DPL 3 */
void syscall_dispatch(struct syscall_frame *f); /* called from syscall_entry; writes the result into f->eax */

/* Closes every descriptor a task still had open, freeing their buffers.
   Called by task_exit_with() on the way out, so a program that exits
   without closing leaks nothing; a task slot is reused by the next
   task_create, and a stale fd table would hand the new task the previous
   one's open files. */
void syscall_release_task(int id);
#endif
