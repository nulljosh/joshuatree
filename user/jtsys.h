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

/* v3 (1.7.7): windows. Joshua Tree's own numbers, 384 up, past anything
   Linux i386 assigns. A program launched from the dock asks for the app
   window the desktop already opened for it and gets a framebuffer of that
   size to draw into; poll hands back keys, clicks and wheel ticks, and
   with JT_POLL_PRESENT copies the framebuffer to the screen first. There
   is no close call: exiting (or crashing) releases the window. */
#define JT_SYS_WINDOW_OPEN 384
#define JT_SYS_WINDOW_POLL 385
#define JT_SYS_TASKS       386 /* 1.9.6: uptime, memory, live slots; optional kill */
#define JT_SYS_HTTP_GET    387 /* 1.9.11: one GET from joshuatree.heyitsmejosh.com; path only, see kernel/syscall.h */
#define JT_HTTP_PATH_MAX 128   /* longest path SYS_HTTP_GET accepts, bytes before the NUL */
#define JT_HTTP_BODY_MAX 2048  /* longest body it hands back */
#define JT_POLL_PRESENT 1
#define JT_EV_KEY   1
#define JT_EV_CLICK 2
#define JT_EV_WHEEL 3
/* Key codes above ASCII, the same values the desktop's own apps see. */
#define JT_KEY_UP    256
#define JT_KEY_DOWN  257
#define JT_KEY_ENTER 258
#define JT_KEY_ESC   259
#define JT_KEY_LEFT  261
#define JT_KEY_RIGHT 262
struct jt_window_info { unsigned int width, height, pitch; unsigned int *pixels; };
struct jt_event { unsigned int kind; int a, b; };
struct jt_tasks { unsigned int ticks, free_kb, total_kb, current, used; };

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

/* Flat user binaries get no .bss (see user/hello.ld): nothing zeroes it
   and there is no crt0 to do it at startup, so libjt pins its
   zero-initialised globals into .data instead, which the linker script
   does write out. ELF is the only object format the real i386 target
   uses; a host build (tools/checks/libjt-host-check.sh, on macOS/Mach-O
   here) doesn't have a .bss restriction at all and Mach-O's section
   attribute syntax differs, so this is a no-op there. */
#if defined(__ELF__)
#define JT_DATA __attribute__((section(".data")))
#else
#define JT_DATA
#endif

#if defined(__i386__)
static inline int jt_syscall(int n, unsigned a, unsigned b, unsigned c) {
    int r;
    __asm__ volatile ("int $0x80"
                      : "=a"(r)
                      : "a"(n), "b"(a), "c"(b), "d"(c)
                      : "memory");
    return r;
}
#else
/* Host builds (tools/checks/libjt-host-check.sh) compile libjt's
   string.c/stdlib.c natively to diff them against the host libc; `int
   $0x80` is not valid on a non-x86 host, and nothing the host harness
   exercises actually needs a real trap, so this stands in for it. It is
   never linked into a ring-3 program: every real build of user/ passes
   -target i386-unknown-none, which takes the branch above. */
static inline int jt_syscall(int n, unsigned a, unsigned b, unsigned c) {
    (void)n; (void)a; (void)b; (void)c;
    return -1;
}
#endif

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
static inline int jt_window_open(struct jt_window_info *info)     { return jt_syscall(JT_SYS_WINDOW_OPEN, (unsigned)info, 0, 0); }
static inline int jt_tasks(struct jt_tasks *t, int kill)          { return jt_syscall(JT_SYS_TASKS, (unsigned)t, (unsigned)kill, 0); }
/* Body bytes on HTTP 200 (at most len), minus the status on any other reply
   (-100..-599), or -errno: -EINVAL bad path, -EFAULT bad pointer, -ENODEV no
   NIC, -EIO no answer. The host is fixed in the kernel; only the path is ours. */
static inline int jt_http_get(const char *path, void *buf, unsigned len) { return jt_syscall(JT_SYS_HTTP_GET, (unsigned)path, (unsigned)buf, len); }
static inline int jt_window_poll(struct jt_event *ev, unsigned flags) { return jt_syscall(JT_SYS_WINDOW_POLL, (unsigned)ev, flags, 0); }

#endif
