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
   first real external caller of any of it.

   v2 adds file writing (open flags, write to a descriptor from open,
   write-back on close), lseek, and argv. It adds numbers and flag bits
   and never changes what a v1 number means: v1 is frozen, and
   tools/checks/usertest-check.sh still runs user/hello.c unmodified. */

#define SYS_EXIT         1   /* ebx = exit code; ends the calling task, never returns */
#define SYS_READ         3   /* ebx = fd, ecx = buf, edx = len; bytes read, 0 at end of file */
#define SYS_WRITE        4   /* ebx = fd (1 or 2 only), ecx = buf, edx = len; returns bytes written */
#define SYS_OPEN         5   /* ebx = path, ecx = flags (0 only); returns a per-task fd >= 3 */
#define SYS_CLOSE        6   /* ebx = fd; 0, or -EBADF */
#define SYS_TIME        13   /* ebx = 0, or a user unsigned* to store into; returns seconds since the epoch */
#define SYS_LSEEK       19   /* v2: ebx = fd, ecx = offset, edx = whence; returns the new position */
#define SYS_GETPID      20   /* returns the calling task's slot id */
#define SYS_SCHED_YIELD 158  /* gives up the rest of this quantum; returns 0 */

/* v3 (1.7.7): windows. Joshua Tree's own numbers start at 384, past
   anything Linux i386 assigns that this kernel could ever want to borrow,
   so the two sets can never collide. Same register shape as everything
   above: ebx/ecx/edx, result in eax, negative errno on failure. */
#define SYS_WINDOW_OPEN 384  /* ebx = struct jt_window_info* (user); fills it, maps the framebuffer user-accessible; 0 or -errno */
#define SYS_WINDOW_POLL 385  /* ebx = struct jt_event* (user), ecx = flags (JT_POLL_PRESENT); 1 event written, -EAGAIN none */

#define SYS_TASKS       386  /* 1.9.6: ebx = struct jt_tasks* (user), ecx = slot to kill or -1; fills the snapshot, 0 or -errno */

/* 1.9.11: SYS_HTTP_GET, the one call the Curbfind port needed. One HTTP
   GET to the fixed host joshuatree.heyitsmejosh.com, port 80: userland
   names only the path, never the host, so a ring-3 program cannot point
   the kernel's network stack anywhere else.
     ebx = path (user, NUL-terminated): at most JT_HTTP_PATH_MAX bytes
           before the NUL, must start with '/', printable ASCII 0x21..0x7E
           only (no spaces, no CR or LF, nothing that could end the request
           line or start a header); anything else is -EINVAL, and -EFAULT
           if the string walks off user memory.
     ecx = out buffer (user, writable), edx = its length, clamped to
           JT_HTTP_BODY_MAX; -EFAULT unless the whole range is user memory.
   Returns the body byte count (0..edx) when the reply was HTTP 200.
   A non-200 reply returns minus its status, -(100..599), which can never
   collide with an errno (all below 100). -ENODEV when there is no NIC,
   -EIO when the host did not answer within the 1500ms reply budget.
   Nothing is written to the buffer on any failure. */
#define SYS_HTTP_GET    387
#define JT_HTTP_PATH_MAX 128
#define JT_HTTP_BODY_MAX 2048

#define JT_POLL_PRESENT 1    /* copy the framebuffer to the screen before looking for an event */

/* Event kinds SYS_WINDOW_POLL writes. a/b depend on the kind: KEY carries
   the key in a (ASCII, or app.h's KEY_* codes at 256 and up), CLICK the
   pointer in window coordinates (a = x, b = y), WHEEL the direction in a
   (+1 up, -1 down). */
#define JT_EV_KEY   1
#define JT_EV_CLICK 2
#define JT_EV_WHEEL 3

struct jt_window_info { unsigned int width, height, pitch; unsigned int *pixels; };
struct jt_event { unsigned int kind; int a, b; };
/* SYS_TASKS: uptime ticks (100Hz), free and total memory in KB, the calling
   task's own slot, and a bitmask of the used scheduler slots. */
struct jt_tasks { unsigned int ticks, free_kb, total_kb, current, used; };

/* v2 open() flags. Linux i386's own values, the same borrow the call
   numbers are: O_RDONLY/O_WRONLY/O_RDWR are the low two bits, the rest
   are independent bits. v1 froze "flags must be 0", which is exactly
   O_RDONLY with no modifiers, so every v1 open still means what it meant.
   Only these bits are understood; any other bit set is -EINVAL rather
   than silently ignored, so a program cannot think it asked for
   something this kernel did not do. */
#define JT_O_RDONLY  0x0000
#define JT_O_WRONLY  0x0001
#define JT_O_RDWR    0x0002
#define JT_O_ACCMODE 0x0003
#define JT_O_CREAT   0x0040
#define JT_O_TRUNC   0x0200
#define JT_O_APPEND  0x0400

/* lseek whence values, Linux's own. */
#define JT_SEEK_SET 0
#define JT_SEEK_CUR 1
#define JT_SEEK_END 2

/* Sized by the highest number in the v1 set (158, sched_yield) rounded up
   to the next multiple of 32, which is the only real reason to pick 160
   over 159: it keeps the table a whole number of cache lines and leaves
   room for the next few Linux numbers worth adopting without another
   resize. It is a table of function pointers, 640 bytes of .bss, so
   size here is not a cost worth optimising. Every slot that is not
   assigned below is a null the dispatcher turns into -ENOSYS rather than
   a jump into nothing, and any number >= NSYSCALLS gets the same answer,
   so the gaps in Linux's numbering cost nothing and hide nothing. */
#define NSYSCALLS 416 /* 385 (SYS_WINDOW_POLL) rounded up to a multiple of 32; was 160 before v3. 386 (tasks) and 387 (http_get) fit under it */

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

/* 1.7.7: slot id of the task that owns the ring-3 window, or -1. The
   launcher (kernel/ring3app.c) reads it after exec_user returns to say
   whether the window was released cleanly. */
int syscall_window_owner(void);
#endif
