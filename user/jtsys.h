#ifndef JTSYS_H
#define JTSYS_H
/* Joshua Tree user-space syscall wrappers. This is the whole of what a
   ring-3 program gets: no libc, no startup files, no relocations. Every
   function here is one `int $0x80` with the arguments already in the
   registers docs/SYSCALL-ABI.md names, so the generated code is the ABI
   itself rather than a layer over it.

   eax = call number, ebx/ecx/edx = arguments 1-3, eax = result on return,
   negative values are -errno. "memory" is in the clobber list because the
   kernel may write through a pointer argument (read, time), so the
   compiler must not keep a stale copy of that memory in a register across
   the trap. ebx is a general register here (this is built -fno-pic, so it
   is not reserved for a GOT pointer) and can be an input constraint
   directly.

   v2 adds lseek and the open() flags that make write() reach a file, and
   it adds arguments. A program is entered with the i386 cdecl frame a
   plain C function expects, which means this is a valid entry point and
   needs no assembly:

       void _start(int argc, char **argv) { ... jt_exit(0); }

   argv[0] is the program's own name and argv[argc] is NULL. A program
   that wants nothing from its arguments can still declare
   `void _start(void)`, which is what user/hello.c does and why it did not
   have to change. Either way _start must never return: there is no caller
   above it, and the return address the loader leaves at 0(%esp) is 0, so
   returning faults and the kernel reaps the task. Call jt_exit(). */

#define JT_SYS_EXIT        1
#define JT_SYS_READ        3
#define JT_SYS_WRITE       4
#define JT_SYS_OPEN        5
#define JT_SYS_CLOSE       6
#define JT_SYS_TIME        13
#define JT_SYS_LSEEK       19
#define JT_SYS_GETPID      20
#define JT_SYS_SCHED_YIELD 158

/* v2 open() flags and lseek() whence values, Linux i386's own numbers.
   O_RDONLY is 0, which is exactly what v1 required, so a v1 program's
   open(path, 0) means the same thing it always did. O_CREAT, O_TRUNC and
   O_APPEND only work alongside O_WRONLY or O_RDWR; asking for one on a
   read-only descriptor is -EINVAL, because nothing is ever written back
   from a read-only descriptor and a silent no-op would be worse. */
#define JT_O_RDONLY  0x0000
#define JT_O_WRONLY  0x0001
#define JT_O_RDWR    0x0002
#define JT_O_CREAT   0x0040
#define JT_O_TRUNC   0x0200
#define JT_O_APPEND  0x0400

#define JT_SEEK_SET 0
#define JT_SEEK_CUR 1
#define JT_SEEK_END 2

static inline int jt_syscall(int n, unsigned a, unsigned b, unsigned c) {
    int r;
    __asm__ volatile ("int $0x80"
                      : "=a"(r)
                      : "a"(n), "b"(a), "c"(b), "d"(c)
                      : "memory");
    return r;
}

static inline void jt_exit(int code) {
    jt_syscall(JT_SYS_EXIT, (unsigned)code, 0, 0);
    for (;;) { } /* SYS_EXIT never returns; the loop only tells the compiler that too */
}
static inline int jt_read(int fd, void *buf, unsigned len)        { return jt_syscall(JT_SYS_READ,  (unsigned)fd, (unsigned)buf, len); }
static inline int jt_write(int fd, const void *buf, unsigned len) { return jt_syscall(JT_SYS_WRITE, (unsigned)fd, (unsigned)buf, len); }
static inline int jt_open(const char *path, int flags)            { return jt_syscall(JT_SYS_OPEN,  (unsigned)path, (unsigned)flags, 0); }
static inline int jt_close(int fd)                                { return jt_syscall(JT_SYS_CLOSE, (unsigned)fd, 0, 0); }
static inline int jt_time(unsigned *out)                          { return jt_syscall(JT_SYS_TIME,  (unsigned)out, 0, 0); }
static inline int jt_getpid(void)                                 { return jt_syscall(JT_SYS_GETPID, 0, 0, 0); }
static inline int jt_sched_yield(void)                            { return jt_syscall(JT_SYS_SCHED_YIELD, 0, 0, 0); }
static inline int jt_lseek(int fd, int off, int whence)           { return jt_syscall(JT_SYS_LSEEK, (unsigned)fd, (unsigned)off, (unsigned)whence); }

#endif
