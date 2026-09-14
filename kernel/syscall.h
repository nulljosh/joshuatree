#ifndef SYSCALL_H
#define SYSCALL_H
/* v64 (0.61.0): the int 0x80 syscall ABI. x86 Linux convention, borrowed
   rather than invented: eax = syscall number, ebx/ecx/edx = arguments 1-3,
   result returned in eax, negative errno on failure. The numbers below are
   Linux i386's own (__NR_exit = 1, __NR_write = 4), so anything written
   against the real ABI's calling convention lines up here too. Deliberately
   tiny: exit and write are the two calls needed to prove a ring-3 task can
   round-trip into the kernel and end cleanly, not the start of a libc. */

#define SYS_EXIT  1  /* ebx = exit code; ends the calling task, never returns */
#define SYS_WRITE 4  /* ebx = fd (1 or 2 only), ecx = buf, edx = len; returns bytes written */
#define NSYSCALLS 8

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
#endif
