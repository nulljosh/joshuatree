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
   directly. */

#define JT_SYS_EXIT        1
#define JT_SYS_READ        3
#define JT_SYS_WRITE       4
#define JT_SYS_OPEN        5
#define JT_SYS_CLOSE       6
#define JT_SYS_TIME        13
#define JT_SYS_GETPID      20
#define JT_SYS_SCHED_YIELD 158

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

#endif
