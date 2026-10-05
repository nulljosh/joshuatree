/* v64 (0.61.0): int 0x80 dispatch, see syscall.h for the ABI. One table
   keyed by eax, the same shape as Linux's sys_call_table, with every
   unassigned slot a null that dispatch turns into -ENOSYS rather than a
   jump into nothing. Runs with interrupts off (interrupt gate, see isr.S),
   on the calling task's own kernel stack (gdt.c's TSS esp0, repointed by
   task.c's schedule() on every switch).

   The rest of the v1 set lands here. Every one of them follows the shape
   sys_write set, and it is the shape that matters more than any of the
   individual calls: an argument that is a user pointer is checked with
   paging_user_range_ok() before the kernel touches a byte of it, the copy
   is bounded by a fixed kernel-side limit, and a failure is a negative
   errno rather than a partial side effect. A ring-3 program can pass any
   32-bit value it likes in ebx/ecx/edx; nothing below dereferences one
   without that check, so there is no argument it can pass that makes the
   kernel read a kernel address on its behalf. */
#include "syscall.h"
#include "brk.h"
#include "idt.h"
#include "task.h"
#include "paging.h"
#include "console.h"
#include "serial.h"
#include "kheap.h"
#include "irq.h"
#include "vfs.h"
#include "exec.h"
#include "window.h"
#include "app.h"
#include "pmm.h"
#include "net.h"  /* 1.9.11: SYS_HTTP_GET */
#include "http.h"
#include "irqlock.h"
#include "shellsys.h"
#include "sb16.h"
#include "font.h"  /* 1.9.21: SYS_TEXT draws through the same AA face as every kernel string */

typedef unsigned int u32;
typedef unsigned char u8;
typedef int (*syscall_fn)(u32 a, u32 b, u32 c);

#define EIO      5
#define EBADF   9
#define EAGAIN  11
#define JT_NET_STACK_MIN 12288u /* 2.0.0: the net path (request + 1514-byte receive + tcp/ip frames + one nested IRQ) stays under 8 KB; 12 KB keeps a margin */
#define ENOMEM  12
#define EFAULT  14
#define EINVAL  22
#define EMFILE  24
#define ENOSPC  28
#define EFBIG   27
#define ESPIPE  29
#define ENOSYS  38
#define ENOENT   2
#define EBUSY   16
#define ENODEV  19
#define EPERM    1
#define EISDIR  21

extern void syscall_entry(void);
int syscall_stress_on; /* 1.9.23: set by r3stress_arm when the command line says stress=r3 */
void r3stress_syscall_round(void);
extern int pde_stress_on; void pdestress_syscall_round(void); /* 1.9.24: stress=pde */
static void window_release(int id); /* v3 windows, below */
static int window_owned(void);

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
   so the copy has a fixed on-stack buffer; longer output is more calls.

   This function is v1 frozen behaviour, character for character, including
   the stop-at-first-NUL that makes it a text sink rather than a byte sink.
   v2's file writes do NOT stop at NUL (see write_file_fd below); the
   inconsistency is deliberate, because changing what a v1 write to fd 1
   does would be a MAJOR break for no gain, and a file descriptor has no v1
   behaviour to preserve. */
#define WRITE_MAX 255
static int console_write(u32 fd, u32 buf, u32 len) {
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

/* ---- open/read/close ----------------------------------------------------
   Descriptors are per task and per task slot: fds[id] belongs to whatever
   task currently owns slot id, and syscall_release_task() clears the row
   when that task exits, so a reused slot never inherits the previous
   task's open files.

   An open file is read eagerly and whole into a kmalloc'd buffer, then
   read() serves out of that snapshot. That is a real design choice with a
   real cost, not an oversight: vfs_read_file() is the only read primitive
   the VFS layer offers (no seek, no partial read, no per-backend cursor),
   so the alternative would be re-reading the entire file on every read()
   call. The cost is that a file bigger than OPEN_MAX_FILE cannot be
   opened at all, and that a file changing on disk after open is not seen
   by an already-open fd. Both are in the contract, not hidden. */
#define MAX_FDS       8    /* per task; 0/1/2 are the standard streams, so 3..7 are openable, five files at once */
#define FIRST_FD      3
#define OPEN_MAX_FILE 8192 /* bytes; open() on anything larger fails rather than silently truncating */
#define PATH_MAX      63   /* bytes of path copied from user space, NUL not included */

struct open_file {
    char *data;
    u32   size;
    u32   pos;
    u32   flags;                /* the open() flags this descriptor was created with */
    int   dirty;                /* 1 once a write has landed in data that the backend has not seen */
    char  name[PATH_MAX + 1];   /* kept so close() knows which file to write the buffer back to */
};
static struct open_file fds[TASK_SLOTS][MAX_FDS];

/* v2: writing.

   The VFS layer has exactly two write primitives, vfs_write_file() and
   vfs_replace_file(), and both take a whole file at once. There is no
   partial write, no truncate-to-length, no per-backend cursor. So a
   writable descriptor here is the read snapshot run backwards: the file
   is loaded whole into the same kmalloc'd buffer, write() edits that
   buffer at the descriptor's own offset, and close() hands the whole
   buffer back through vfs_replace_file(). Nothing else the VFS offers
   could implement write() honestly, and pretending otherwise would mean
   re-writing the entire file on every 16-byte write() call.

   The real, contractual consequence is that the file on the backend does
   not change until close() (or task exit, which closes for you). Two
   descriptors open on the same file will not see each other, and the last
   one closed wins the whole file. That is in docs/SYSCALL-ABI.md, not
   hidden here. */

/* Copies a NUL-terminated string out of user space into a fixed kernel
   buffer, one byte at a time with the mapping checked ahead of each read.
   Per byte rather than per string because the length isn't known until the
   NUL is found: checking "the whole string" up front would mean reading it
   to find its end, which is the very read being guarded. Returns 0 on
   success, a negative errno if the pointer walks off a mapped user page
   (-EFAULT) or if no NUL turns up within PATH_MAX (-EINVAL). */
static int copy_path_from_user(u32 addr, char *out) {
    for (u32 i = 0; i <= PATH_MAX; i++) {
        if (!paging_user_range_ok(addr + i, 1)) return -EFAULT;
        char ch = ((const char *)addr)[i];
        out[i] = ch;
        if (!ch) return 0;
    }
    return -EINVAL;
}

/* 1.9.13: relative paths. The VFS has one current directory (the shell's)
   and every vfs_* call is relative to it; the only way to reach a file in
   a subdirectory is vfs_chdir, which moves that one global. A ring-3
   program must not be able to move it, so a path with slashes is walked
   here, one vfs_chdir per component, and walked back with ".." before the
   syscall returns. int 0x80 is an interrupt gate, so nothing else runs in
   between and the move is invisible to every other task.

   path_enter splits `path` in place, enters every component but the last
   `keep` of them (keep is 1 for open, which wants the file's own name left
   over; 0 for readdir, which wants to stand inside the directory itself),
   and hands back the number of directories entered and a pointer to the
   leaf. A component that is empty, "." or "..", a leading slash, or more
   than JT_PATH_DEPTH components is -EINVAL; a component that vfs_chdir
   refuses is -ENOENT (ramfs refuses them all: it has no directories). On
   any failure it has already walked back, so the cwd is what it was.
   path_leave walks back; a ".." that fails is logged, since the cwd is
   then genuinely wrong and the shell's next `ls` will show it. */
/* 1.9.23: the cwd cursor is the desktop's. Task 0 may have been preempted
   while standing inside NOTES/, so a syscall path starts at the root and
   the cursor is put back exactly where it was before the gate returns. */
static unsigned int path_saved_cwd;
int path_leave(int depth) {
    int ok = 1;
    while (depth-- > 0) if (!vfs_chdir("..")) ok = 0;
    if (!ok) serial_puts("syscall: BUG chdir(..) failed walking back a relative path\n");
    vfs_cwd_set(path_saved_cwd);
    return ok;
}
int path_enter(char *path, int keep, char **leaf, int *depth) {
    *depth = 0; *leaf = path;
    if (path[0] == '/') return -EINVAL;
    char *comp[JT_PATH_DEPTH + 1];
    int n = 0;
    if (path[0]) {
        char *p = path;
        for (;;) {
            if (n > JT_PATH_DEPTH) return -EINVAL;
            comp[n++] = p;
            char *q = p;
            while (*q && *q != '/') q++;
            int len = (int)(q - p);
            if (len == 0 || (len == 1 && p[0] == '.') || (len == 2 && p[0] == '.' && p[1] == '.')) return -EINVAL;
            if (!*q) break;
            *q = 0;
            p = q + 1;
        }
    }
    if (n < keep) return -EINVAL; /* open("") or open("DOCS/") */
    if (n - keep > JT_PATH_DEPTH) return -EINVAL;
    path_saved_cwd = vfs_cwd_get(); vfs_cwd_set(0);
    for (int i = 0; i < n - keep; i++) {
        if (!vfs_chdir(comp[i])) { path_leave(*depth); *depth = 0; return -ENOENT; }
        (*depth)++;
    }
    *leaf = n ? comp[n - 1] : path;
    return 0;
}

static int sys_open(u32 path, u32 flags, u32 c) {
    (void)c;
    /* Only bits this kernel actually implements are accepted. An
       unrecognised bit is -EINVAL rather than ignored, so a program can
       never believe it asked for something that did not happen. v1's rule
       was "flags must be 0", which is exactly O_RDONLY with no modifiers,
       so every v1 open still means what it meant. */
    if (flags & ~(u32)(JT_O_ACCMODE | JT_O_CREAT | JT_O_TRUNC | JT_O_APPEND)) return -EINVAL;
    u32 acc = flags & JT_O_ACCMODE;
    if (acc == JT_O_ACCMODE) return -EINVAL; /* 3 is not an access mode */
    /* O_CREAT/O_TRUNC/O_APPEND only mean something on a descriptor that
       can write, because nothing is ever written back from a read-only
       one. Asking for them read-only is a program bug, so it is an error
       rather than a silent no-op. */
    if (acc == JT_O_RDONLY && (flags & (JT_O_CREAT | JT_O_TRUNC | JT_O_APPEND))) return -EINVAL;

    int id = task_current();
    if (id < 0 || id >= TASK_SLOTS) return -EBADF;

    char name[PATH_MAX + 1];
    int err = copy_path_from_user(path, name);
    if (err) return err;

    int fd = -1;
    for (int i = FIRST_FD; i < MAX_FDS; i++) if (!fds[id][i].data) { fd = i; break; }
    if (fd < 0) return -EMFILE;

    /* 1.9.13: "DOCS/NOTE.TXT" opens NOTE.TXT inside DOCS. The walk is
       undone before this returns, whichever way it returns; the full path
       is what close() will need to walk again. */
    char walk[PATH_MAX + 1];
    for (int i = 0; i <= PATH_MAX; i++) { walk[i] = name[i]; if (!name[i]) break; }
    char *leaf; int depth;
    err = path_enter(walk, 1, &leaf, &depth);
    if (err) return err;

    char *buf = (char *)kmalloc(OPEN_MAX_FILE + 1);
    if (!buf) { path_leave(depth); return -ENOMEM; }
    int n = vfs_read_file(leaf, buf, OPEN_MAX_FILE + 1);
    if (n > OPEN_MAX_FILE) { kfree(buf); path_leave(depth); return -EINVAL; } /* bigger than one open can hold; refused, never truncated */
    /* A backend read returns a byte count and nothing else, so a
       zero-length file and a missing file are the same answer here. v1
       already resolved that ambiguity as "missing" and this keeps it. */
    int exists = (n > 0);
    if (!exists && !(flags & JT_O_CREAT)) { kfree(buf); path_leave(depth); return -ENOENT; }

    u32 size = exists ? (u32)n : 0;
    if (flags & JT_O_TRUNC) size = 0;

    /* O_CREAT and O_TRUNC take effect now, not at close: the file exists,
       and is empty, the moment open() returns. Only the bytes a program
       goes on to write are deferred to close. Doing half of it at open and
       half at close would be the confusing shape. */
    if (!exists || (flags & JT_O_TRUNC)) {
        if (!vfs_replace_file(leaf, "", 0)) { kfree(buf); path_leave(depth); return -ENOSPC; }
    }
    path_leave(depth);

    fds[id][fd].data  = buf;
    fds[id][fd].size  = size;
    fds[id][fd].pos   = (flags & JT_O_APPEND) ? size : 0;
    fds[id][fd].flags = flags;
    fds[id][fd].dirty = 0;
    for (int i = 0; i <= PATH_MAX; i++) { fds[id][fd].name[i] = name[i]; if (!name[i]) break; }
    serial_puts("syscall: open ok\n");
    return fd;
}

/* The file half of write(). Binary clean on purpose: unlike the console
   path above it does not stop at a NUL, because a file is a byte store and
   there is no v1 behaviour here to preserve. Bounded by OPEN_MAX_FILE
   rather than by the backend, because the backend cannot be asked how much
   room it has; a write past the buffer is -EFBIG and writes nothing, never
   a short write the caller has to notice. */
static int write_file_fd(int id, u32 fd, u32 buf, u32 len) {
    if (fd >= MAX_FDS || !fds[id][fd].data) return -EBADF;
    struct open_file *f = &fds[id][fd];
    if ((f->flags & JT_O_ACCMODE) == JT_O_RDONLY) return -EBADF;
    if (f->flags & JT_O_APPEND) f->pos = f->size; /* O_APPEND ignores the seek offset, Linux's own rule */
    if (len == 0) return 0;
    if (f->pos + len > OPEN_MAX_FILE) return -EFBIG;
    if (!paging_user_range_ok(buf, len)) return -EFAULT;
    const char *src = (const char *)buf;
    for (u32 i = 0; i < len; i++) f->data[f->pos + i] = src[i];
    f->pos += len;
    if (f->pos > f->size) f->size = f->pos;
    f->dirty = 1;
    return (int)len;
}

static int sys_write(u32 fd, u32 buf, u32 len) {
    if (len > WRITE_MAX) len = WRITE_MAX;
    if (fd == 1 || fd == 2) return console_write(fd, buf, len);
    if (fd == 0) return -EBADF; /* stdin, as in v1 */
    int id = task_current();
    if (id < 0 || id >= TASK_SLOTS) return -EBADF;
    return write_file_fd(id, fd, buf, len);
}

/* lseek(fd, offset, whence). Meaningful here precisely because a
   descriptor is a whole-file buffer: pos is a real index into it, so a
   seek followed by a write really does overwrite bytes in the middle of a
   file. Seeking to exactly size is legal (that is end of file, where an
   append lands); past it is -EINVAL rather than a hole, because a hole
   would mean inventing zero bytes the program never wrote. */
static int sys_lseek(u32 fd, u32 off, u32 whence) {
    if (fd < FIRST_FD) return -ESPIPE; /* 0/1/2 are streams, not files */
    int id = task_current();
    if (id < 0 || id >= TASK_SLOTS) return -EBADF;
    if (fd >= MAX_FDS || !fds[id][fd].data) return -EBADF;
    struct open_file *f = &fds[id][fd];
    int base;
    if      (whence == JT_SEEK_SET) base = 0;
    else if (whence == JT_SEEK_CUR) base = (int)f->pos;
    else if (whence == JT_SEEK_END) base = (int)f->size;
    else return -EINVAL;
    int np = base + (int)off;
    if (np < 0 || (u32)np > f->size) return -EINVAL;
    f->pos = (u32)np;
    return np;
}

/* read(fd, buf, len). fd 0 is the keyboard and is non-blocking by
   necessity, not by preference: int 0x80 is an interrupt gate, so
   interrupts are off inside this function, and the keyboard IRQ that
   would deliver the awaited byte cannot fire. Blocking here would hang
   the machine, so a read with nothing queued is -EAGAIN and the program
   is expected to come back (sched_yield, then read again). */
static int sys_read(u32 fd, u32 buf, u32 len) {
    if (len > WRITE_MAX) len = WRITE_MAX;
    if (len == 0) return 0;
    if (!paging_user_range_ok(buf, len)) return -EFAULT;
    char *dst = (char *)buf;

    if (fd == 0) {
        u32 n = 0;
        while (n < len) {
            int ch = console_read_key();
            if (ch < 0) break;
            dst[n++] = (char)ch;
        }
        return n ? (int)n : -EAGAIN;
    }
    if (fd == 1 || fd == 2) return -EBADF; /* the console is write-only through 1 and 2 */

    int id = task_current();
    if (id < 0 || id >= TASK_SLOTS) return -EBADF;
    if (fd >= MAX_FDS || !fds[id][fd].data) return -EBADF;

    struct open_file *f = &fds[id][fd];
    if ((f->flags & JT_O_ACCMODE) == JT_O_WRONLY) return -EBADF; /* v2: write-only means write-only */
    u32 left = f->size - f->pos;
    if (left == 0) return 0; /* real end of file, the one case that is 0 rather than an errno */
    u32 n = len < left ? len : left;
    for (u32 i = 0; i < n; i++) dst[i] = f->data[f->pos + i];
    f->pos += n;
    return (int)n;
}

/* Writes a dirty descriptor's buffer back through the VFS, then reads it
   straight back and compares.

   The readback is not belt and braces, it is the only way this layer can
   tell the truth. vfs_replace_file() answers "1" or "0" and nothing else,
   and a backend whose own per-file cap is smaller than OPEN_MAX_FILE
   (ramfs: 4096 bytes, see drivers/ramfs.c) truncates to that cap and still
   answers 1. Without the readback, close() would report success for a file
   that lost its tail. With it, that case is -EIO, a real error a program
   can act on. The cost is one extra whole-file read and one extra
   OPEN_MAX_FILE allocation per close of a descriptor that was written to,
   and it is paid only then. */
static int flush_fd(struct open_file *f) {
    if (!f->dirty) return 0;
    /* 1.9.13: the name is the relative path open() was given; walk into
       its directory the same way, and back out before returning. */
    char walk[PATH_MAX + 1];
    for (int i = 0; i <= PATH_MAX; i++) { walk[i] = f->name[i]; if (!f->name[i]) break; }
    char *leaf; int depth;
    if (path_enter(walk, 1, &leaf, &depth)) return -EIO; /* the directory went away since open */
    if (!vfs_replace_file(leaf, f->data, f->size)) { path_leave(depth); return -EIO; }
    char *back = (char *)kmalloc(OPEN_MAX_FILE + 1);
    if (!back) { path_leave(depth); return -ENOMEM; }
    int n = vfs_read_file(leaf, back, OPEN_MAX_FILE + 1);
    int ok = (n >= 0) && ((u32)n == f->size);
    for (u32 i = 0; ok && i < f->size; i++) if (back[i] != f->data[i]) ok = 0;
    kfree(back);
    path_leave(depth);
    if (!ok) return -EIO;
    f->dirty = 0;
    return 0;
}

/* The descriptor is released whether or not the flush worked. A close that
   returns -EIO has still closed: leaving a half-open fd behind would mean
   a program that ignores the error slowly runs out of descriptors, which
   is a worse failure than the one being reported. */
static int close_fd(int id, u32 fd) {
    if (fd < FIRST_FD || fd >= MAX_FDS || !fds[id][fd].data) return -EBADF;
    int err = flush_fd(&fds[id][fd]);
    kfree(fds[id][fd].data);
    fds[id][fd].data = 0;
    fds[id][fd].size = fds[id][fd].pos = 0;
    fds[id][fd].flags = 0;
    fds[id][fd].dirty = 0;
    fds[id][fd].name[0] = 0;
    return err;
}

static int sys_close(u32 fd, u32 b, u32 c) {
    (void)b; (void)c;
    int id = task_current();
    if (id < 0 || id >= TASK_SLOTS) return -EBADF;
    return close_fd(id, fd);
}

static void r3win_release(int task);
void syscall_release_task(int id) {
    if (id < 0 || id >= TASK_SLOTS) return;
    window_release(id);
    brk_release(id, task_page_dir(id)); /* 1.9.27: every heap page back to the PMM, exit or crash alike */
    /* Every teardown path lands here (exit, idt.c's fault reap, and the
       launcher's failed exec never mapped anything), so this is the one
       place to insist: no owner, no user-accessible framebuffer. Cheap,
       idempotent, and it holds even if a future open path fails halfway. */
    if (!window_owned()) paging_clear_user((void *)JT_USER_FB, JT_USER_FB_BYTES);
    for (int i = FIRST_FD; i < MAX_FDS; i++) {
        /* v2: this also flushes. A program that writes and then exits
           without closing still gets its bytes out, which is what a caller
           expects and what Linux does. Nobody is left to receive an error
           at this point, so a failed exit-time flush is reported on the
           serial log rather than swallowed entirely. */
        int err = close_fd(id, (u32)i);
        if (err && err != -EBADF) serial_puts("syscall: exit-time flush failed, file not written\n");
    }
}

/* ---- time ---------------------------------------------------------------
   Seconds since 1970-01-01T00:00:00Z, read from the CMOS RTC. kernel.c
   reads the same registers for the menu-bar clock, but only ever wants
   the broken-down fields, so the epoch conversion lives here rather than
   being pulled out of a GUI file for one caller.

   The UIP guard is the standard MC146818 erratum handling kernel.c
   already documents: don't read while an update is in progress, and
   re-check afterwards, because a read that lands inside the update window
   can come back torn. Register 0x0B bit 2 says whether the fields are
   binary or BCD, bit 1 whether hours are 24-hour; QEMU gives BCD/24h, but
   reading the flag instead of assuming it is two lines. */
static inline void outb(unsigned short p, u8 v) { __asm__ volatile ("outb %0,%1" :: "a"(v), "Nd"(p)); }
static inline u8   inb(unsigned short p)        { u8 v; __asm__ volatile ("inb %1,%0" : "=a"(v) : "Nd"(p)); return v; }
static u8 cmos(u8 reg) { outb(0x70, reg); return inb(0x71); }
static int cmos_updating(void) { return cmos(0x0A) & 0x80; }

/* Days from 1970-01-01 to y-m-d, Howard Hinnant's days_from_civil (the
   same closed form the C++20 <chrono> implementations use). Borrowed
   rather than reinvented: it is branch-free, correct across the Gregorian
   leap rules, and needs no lookup table. */
static int days_from_civil(int y, int m, int d) {
    y -= m <= 2;
    int era = (y >= 0 ? y : y - 399) / 400;
    int yoe = y - era * 400;
    int doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

static u32 rtc_epoch_seconds(void) {
    u8 s, mi, h, d, mo, y, cent = 0;
    for (int tries = 0; tries < 8; tries++) {
        while (cmos_updating()) { }
        s = cmos(0x00); mi = cmos(0x02); h = cmos(0x04);
        d = cmos(0x07); mo = cmos(0x08); y = cmos(0x09); cent = cmos(0x32);
        if (!cmos_updating()) break;
    }
    u8 status_b = cmos(0x0B);
    int pm = !(status_b & 0x02) && (h & 0x80); /* 12-hour mode flags PM in the high bit, before BCD decoding */
    if (!(status_b & 0x04)) { /* BCD */
        #define BCD(v) (((v) & 0x0F) + (((v) >> 4) * 10))
        s = BCD(s); mi = BCD(mi); h = BCD(h & 0x7F); d = BCD(d); mo = BCD(mo); y = BCD(y);
        cent = BCD(cent);
        #undef BCD
    } else {
        h &= 0x7F;
    }
    if (!(status_b & 0x02)) { /* 12-hour mode: noon and midnight are the two that trip people up */
        if (pm && h < 12) h = (u8)(h + 12);
        if (!pm && h == 12) h = 0;
    }
    /* Register 0x32 is the ACPI century byte. QEMU fills it; hardware that
       doesn't leaves something implausible there, in which case a two-digit
       year is assumed to be 20xx, which is right for every year this
       kernel will plausibly run in and wrong in a way that is obvious
       rather than subtle. */
    int year = (cent >= 19 && cent <= 21) ? cent * 100 + y : 2000 + y;
    int days = days_from_civil(year, mo ? mo : 1, d ? d : 1);
    return (u32)days * 86400u + (u32)h * 3600u + (u32)mi * 60u + s;
}

static int sys_time(u32 out, u32 b, u32 c) {
    (void)b; (void)c;
    u32 now = rtc_epoch_seconds();
    if (out) {
        /* Linux's time(2) shape: a non-null pointer is also written
           through. Same access_ok discipline as every other pointer
           here, and the value is still returned in eax either way. */
        if (!paging_user_range_ok(out, sizeof(u32))) return -EFAULT;
        *(u32 *)out = now;
    }
    return (int)now;
}

static int sys_getpid(u32 a, u32 b, u32 c) {
    (void)a; (void)b; (void)c;
    return task_current();
}

static int sys_sched_yield(u32 a, u32 b, u32 c) {
    (void)a; (void)b; (void)c;
    /* yield() is `int $32`, a software interrupt, so it still fires with
       IF clear inside this gate; it saves this task's kernel-stack frame
       exactly the way a timer tick would and comes back here when the
       round-robin returns. A lone runnable task yields to nobody and
       returns immediately, which is still a successful 0. */
    yield();
    return 0;
}

/* ---- windows (v3, 1.7.7) -------------------------------------------------
   The first step of "apps leave the kernel": a ring-3 program gets the
   app window the desktop already opened for it. No new windowing code:
   the desktop draws the chrome and sets the viewport exactly as it does
   for an in-kernel app (gui_launch_from_dock), then the program the
   launcher exec'd asks for that viewport here. It gets a framebuffer of
   the viewport's size at JT_USER_FB, mapped user-accessible, and draws
   into it directly. SYS_WINDOW_POLL with JT_POLL_PRESENT copies the
   buffer into the viewport through window_pixel, the same primitive every
   in-kernel app draws with, so clipping and the back buffer come for free.

   One window, one owner. The owner is recorded by task slot so
   syscall_release_task can tear it down whether the program exited or was
   reaped by idt.c's fault path: either way the pages go back to
   supervisor-only and the desktop repaints, nothing waits on a program
   that is no longer there. */
static int win_owner = -1;
static u32 text_n = 0, text_used = 0; /* SYS_TEXT queue, below */
static u32 win_w = 0, win_h = 0;

int syscall_window_owner(void) { return win_owner; }
static int window_owned(void) { return win_owner >= 0; }

/* Desktop side, kernel.c. */
int  gui_app_view_size(unsigned int *w, unsigned int *h); /* 1 if an app viewport is open */
int  gui_poll_event(int *a, int *b);                        /* non-blocking: JT_EV_* or 0 */

/* 1.9.23: compositor windows. A ring-3 program launched through
   exec_user_window gets a row here before it runs: a framebuffer of the
   window's size, kmalloc'd and mapped only into that task's directory at
   JT_USER_FB (paging_task_map_private), plus a 64-deep event ring the
   desktop fills (syscall_window_push_event) and SYS_WINDOW_POLL drains.
   JT_POLL_PRESENT no longer copies anything: it marks the row dirty and
   kernel.c's compositor blits the buffer at the window's position on its
   next frame. The legacy single owner (win_owner, above) stays for the
   blocking launcher, so nothing unconverted changes. */
#define R3WIN_MAX 4
#define R3WIN_RING 64
struct r3win { int task; u32 *fb; void *fb_raw; void *old_raw; u32 old_w, old_h; u32 want_w, want_h; void *image; u32 w, h; int dirty; struct jt_event ring[R3WIN_RING]; u32 rh, rt; };
static struct r3win r3wins[R3WIN_MAX];
static struct r3win *r3win_of(int task) {
    if (task < 0) return 0;
    for (int i = 0; i < R3WIN_MAX; i++) if (r3wins[i].task == task) return &r3wins[i];
    return 0;
}
int syscall_window_register(int task, u32 w, u32 h, void *image) {
    if (task <= 0 || !w || !h || w * h * 4 > JT_USER_FB_BYTES) return 0;
    struct r3win *r = 0;
    for (int i = 0; i < R3WIN_MAX; i++) if (r3wins[i].task <= 0) { r = &r3wins[i]; break; }
    if (!r) return 0;
    u32 bytes = (w * h * 4 + 4095) & ~4095u;
    u8 *raw = (u8 *)kmalloc(bytes + 4096);
    if (!raw) return 0;
    u32 *fb = (u32 *)(((u32)raw + 4095) & ~4095u);
    for (u32 i = 0; i < w * h; i++) fb[i] = 0;
    if (!paging_task_map_private(task_page_dir(task), JT_USER_FB, (u32)fb, bytes)) { kfree(raw); return 0; }
    r->task = task; r->fb = fb; r->fb_raw = raw; r->old_raw = 0; r->old_w = r->old_h = 0; r->want_w = w; r->want_h = h; r->image = image; r->w = w; r->h = h; r->dirty = 0; r->rh = r->rt = 0;
    return 1;
}
/* 1.10.x resize: the buffer a re-open replaced is freed here, on the next
   compositor look at the window, not inside the swap: a blit that was
   already reading it (the timer can run the app mid-blit) never sees freed
   memory. Zeroed first, same rule as the release path. Kernel PDEs are
   synced into every task directory, so no CR3 switch is needed (and a
   mid-syscall one is what 1.9.28 removed). */
static void r3win_free_old(struct r3win *r) {
    if (!r->old_raw) return;
    u32 *o = (u32 *)(((u32)r->old_raw + 4095) & ~4095u);
    for (u32 i = 0; i < r->old_w * r->old_h; i++) o[i] = 0;
    kfree(r->old_raw);
    r->old_raw = 0; r->old_w = r->old_h = 0;
}
void syscall_window_request_size(int task, u32 w, u32 h) {
    unsigned int f = irq_save();
    struct r3win *r = r3win_of(task);
    if (r && w && h && (w != r->want_w || h != r->want_h) && w * h * 4 <= JT_USER_FB_BYTES
        && r->rt - r->rh < R3WIN_RING) { /* a full ring defers the ask to the next frame; an oversize rect keeps the old buffer clipped */
        struct jt_event *e = &r->ring[r->rt % R3WIN_RING];
        e->kind = JT_EV_RESIZE; e->a = (int)w; e->b = (int)h; r->rt++;
        r->want_w = w; r->want_h = h;
    }
    irq_restore(f);
}
/* SYS_WINDOW_OPEN on a window that already has its buffer, with a resize
   pending: a new buffer at the wanted size replaces the old one. */
static int r3win_resize(struct r3win *r) {
    u32 w = r->want_w, h = r->want_h;
    if (w == r->w && h == r->h) return 0;
    u32 bytes = (w * h * 4 + 4095) & ~4095u;
    u8 *raw = (u8 *)kmalloc(bytes + 4096);
    if (!raw) return -ENOMEM; /* keeps the old buffer; the app stays at its old size */
    u32 *fb = (u32 *)(((u32)raw + 4095) & ~4095u);
    for (u32 i = 0; i < w * h; i++) fb[i] = 0; /* zeroed before it is mapped user-accessible */
    unsigned int f = irq_save();
    r3win_free_old(r); /* a still-pending earlier swap, if any */
    u32 old_bytes = (r->w * r->h * 4 + 4095) & ~4095u;
    if (!paging_task_remap_private(task_page_dir(r->task), JT_USER_FB, (u32)fb, bytes, old_bytes)) { irq_restore(f); kfree(raw); return -ENOMEM; }
    r->old_raw = r->fb_raw; r->old_w = r->w; r->old_h = r->h;
    r->fb = fb; r->fb_raw = raw; r->w = w; r->h = h; r->dirty = 1;
    irq_restore(f);
    serial_puts("syscall: window resized, new buffer mapped\n");
    return 0;
}
const u32 *syscall_window_fb(int task, u32 *w, u32 *h, int *dirty) {
    struct r3win *r = r3win_of(task);
    if (!r) return 0;
    r3win_free_old(r);
    *w = r->w; *h = r->h; *dirty = r->dirty; r->dirty = 0;
    return r->fb;
}
void syscall_window_push_event(int task, int kind, int a, int b) {
    unsigned int f = irq_save(); /* 1.9.23: the compositor fills with IF on, SYS_WINDOW_POLL drains under the gate; the slot is written before rt moves */
    struct r3win *r = r3win_of(task);
    if (r && r->rt - r->rh < R3WIN_RING) { /* full: the oldest stays, the newest is dropped, same as a full keyboard ring */
        struct jt_event *e = &r->ring[r->rt % R3WIN_RING];
        e->kind = (u32)kind; e->a = a; e->b = b; r->rt++;
    }
    irq_restore(f);
}
static void r3win_release(int task) {
    struct r3win *r = r3win_of(task);
    if (!r) return;
    /* The exiting task runs this on its own directory, a copy made before
       the heap grew to where this window's buffers live; those pages are only
       guaranteed mapped in the kernel directory. Touch them from there. */
    u32 cr3_was; __asm__ volatile ("mov %%cr3, %0" : "=r"(cr3_was));
    __asm__ volatile ("mov %0, %%cr3" :: "r"(paging_kernel_directory()) : "memory");
    for (u32 i = 0; i < r->w * r->h; i++) r->fb[i] = 0;
    r3win_free_old(r);
    kfree(r->fb_raw); kfree(r->image);
    __asm__ volatile ("mov %0, %%cr3" :: "r"(cr3_was) : "memory");
    r->task = -1; r->fb = 0; r->fb_raw = 0; r->image = 0; r->w = r->h = 0;
    serial_puts("syscall: window released, task gone\n");
}
void syscall_windows_init(void) { for (int i = 0; i < R3WIN_MAX; i++) r3wins[i].task = -1; }

static int sys_window_open(u32 info, u32 b, u32 c) {
    (void)b; (void)c;
    if (!paging_user_range_ok(info, sizeof(struct jt_window_info))) return -EFAULT;
    int id = task_current();
    if (id <= 0 || id >= TASK_SLOTS) return -EBADF;
    struct r3win *r = r3win_of(id);
    if (r) {
        struct jt_window_info *out = (struct jt_window_info *)info;
        int rr = r3win_resize(r); /* 1.10.x: a second open is the answer to JT_EV_RESIZE; a no-op when the size already matches */
        if (rr < 0) return rr;
        out->width = r->w; out->height = r->h; out->pitch = r->w * 4; out->pixels = (u32 *)JT_USER_FB;
        serial_puts("syscall: window opened for ring-3 task\n");
        return 0;
    }
    if (win_owner >= 0 && win_owner != id) return -EBUSY;
    u32 w, h;
    if (!gui_app_view_size(&w, &h) || !w || !h) return -ENODEV;
    if (w * h * 4 > JT_USER_FB_BYTES) return -ENOMEM;
    u32 *fb = (u32 *)JT_USER_FB;
    for (u32 i = 0; i < w * h; i++) fb[i] = 0; /* the previous program's frame is not this one's to read */
    /* Mapped user-accessible only now, after the last check that can
       fail: a refused open leaves the pages supervisor-only. */
    for (u32 off = 0; off < JT_USER_FB_BYTES; off += 4096) paging_set_user((void *)(JT_USER_FB + off));
    win_owner = id; win_w = w; win_h = h;
    text_n = 0; text_used = 0;
    struct jt_window_info *out = (struct jt_window_info *)info;
    out->width = w; out->height = h; out->pitch = w * 4; out->pixels = fb;
    serial_puts("syscall: window opened for ring-3 task\n");
    return 0;
}

/* 1.9.21: SYS_TEXT, real typeface text for a ring-3 window. The window's
   pixel buffer is logical resolution and is doubled on the way to the
   screen, so glyphs a program stamps into it itself are blocky. This call
   queues a string instead; window_present_user draws the queue after the
   pixel copy, through font_draw_string, which is the physical-resolution
   anti-aliased path every kernel string uses. The queue lives until the
   program clears it (op 2), so a present with no redraw keeps its text.
   Strings are copied out of user memory byte by byte, printable ASCII
   only, so nothing the kernel later draws can read past the caller's own
   page. */
#define TEXT_OPS  192
#define TEXT_POOL 6144
struct text_op { int x, y; u32 fg; unsigned short off; };
static struct text_op text_ops[TEXT_OPS];
static char text_pool[TEXT_POOL];

static int sys_text(u32 req, u32 op, u32 c) {
    (void)c;
    if (win_owner != task_current()) return -EBADF;
    if (op == JT_TEXT_CLEAR) { text_n = 0; text_used = 0; return 0; }
    if (op != JT_TEXT_DRAW && op != JT_TEXT_MEASURE) return -EINVAL;
    if (!paging_user_range_ok(req, sizeof(struct jt_text))) return -EFAULT;
    const struct jt_text *t = (const struct jt_text *)req;
    char buf[JT_TEXT_MAX + 1];
    u32 n = 0;
    for (;; n++) {
        if (n > JT_TEXT_MAX) return -EINVAL;
        if (!paging_user_range_ok((u32)t->s + n, 1)) return -EFAULT;
        char ch = t->s[n];
        if (!ch) break;
        if ((unsigned char)ch < 32 || (unsigned char)ch > 126) return -EINVAL;
        buf[n] = ch;
    }
    buf[n] = 0;
    int w = font_string_width(buf);
    if (op == JT_TEXT_MEASURE) return w;
    if (text_n >= TEXT_OPS || text_used + n + 1 > TEXT_POOL) return -ENOMEM;
    for (u32 i = 0; i <= n; i++) text_pool[text_used + i] = buf[i];
    text_ops[text_n].x = t->x; text_ops[text_n].y = t->y; text_ops[text_n].fg = t->fg & 0x00FFFFFFu;
    text_ops[text_n].off = (unsigned short)text_used;
    text_n++; text_used += n + 1;
    return w;
}

static void window_present_user(void) {
    const u32 *fb = (const u32 *)JT_USER_FB;
    for (u32 y = 0; y < win_h; y++)
        for (u32 x = 0; x < win_w; x++)
            window_pixel((int)x, (int)y, fb[y * win_w + x]);
    for (u32 i = 0; i < text_n; i++)
        font_draw_string(text_pool + text_ops[i].off, text_ops[i].x, text_ops[i].y, text_ops[i].fg, -1);
    window_present();
}

static int sys_window_poll(u32 ev, u32 flags, u32 c) {
    (void)c;
    if (!paging_user_range_ok(ev, sizeof(struct jt_event))) return -EFAULT;
    if (flags & ~(u32)JT_POLL_PRESENT) return -EINVAL;
    struct r3win *r = r3win_of(task_current());
    if (r) {
        if (syscall_stress_on) r3stress_syscall_round(); /* 1.9.23: stress=r3 boot flag, see r3stress.c */
        if (pde_stress_on) pdestress_syscall_round(); /* 1.9.24: stress=pde, touches heap mapped after this task was born */
        if (flags & JT_POLL_PRESENT) r->dirty = 1;
        if (r->rh == r->rt) return -EAGAIN;
        struct jt_event *out = (struct jt_event *)ev;
        *out = r->ring[r->rh % R3WIN_RING]; r->rh++;
        return 1;
    }
    if (win_owner != task_current()) return -EBADF;
    if (flags & JT_POLL_PRESENT) window_present_user();
    int a = 0, b = 0;
    int kind = gui_poll_event(&a, &b);
    if (!kind) return -EAGAIN;
    struct jt_event *out = (struct jt_event *)ev;
    out->kind = (u32)kind; out->a = a; out->b = b;
    return 1;
}

/* 1.9.6: SYS_TASKS, what the Activity app shows. One call: fill the
   snapshot (uptime, memory, which scheduler slots are live), and if ecx
   names a slot, kill it first through task_kill, the same primitive the
   shell's `kill` uses. Slot 0 is the shell/GUI and the caller's own slot is
   the app itself, so both are refused with -EPERM; a free slot is -ENOENT. */
static int sys_tasks(u32 out, u32 kill, u32 c) {
    (void)c;
    if (!paging_user_range_ok(out, sizeof(struct jt_tasks))) return -EFAULT;
    int rc = 0;
    if (kill != 0xFFFFFFFFu) {
        if (kill == 0 || kill >= TASK_SLOTS || (int)kill == task_current()) rc = -EPERM;
        else if (!task_used((int)kill)) rc = -ENOENT;
        else task_kill((int)kill);
    }
    struct jt_tasks *o = (struct jt_tasks *)out;
    o->ticks = ticks();
    o->free_kb = pmm_free_frames() * 4;
    o->total_kb = pmm_total_frames() * 4;
    o->current = (u32)task_current();
    o->used = 0;
    for (int i = 0; i < TASK_SLOTS; i++) if (task_used(i)) o->used |= 1u << i;
    return rc;
}

/* 1.9.11: SYS_HTTP_GET, what the Curbfind app fetches its live rows with.
   This is the first syscall that puts the kernel's network stack behind a
   ring-3 caller, so it is deliberately narrow: the host and port are
   fixed here, the caller names only the path, and the path is copied out
   of user memory with the same per-byte check copy_path_from_user does,
   then refused unless it is a plain absolute path of printable ASCII.
   The request line is "GET <path> HTTP/1.0\r\n" followed by headers, so a
   space would end the path early and a CR or LF would let the caller
   write a header of its own; neither can get through.

   The fetch itself runs with interrupts on. The syscall gate clears IF,
   but net.c times every wait off ticks(), which only advances on the
   timer IRQ, so with IF clear a missing reply would spin forever instead
   of timing out. Re-enabling IF here is the same preemption a ring-3
   program lives under between syscalls (sys_sched_yield's int $32 already
   lets the scheduler run inside this gate), and IF is cleared again before
   the iret restores the caller's own flags. The reply lands in a kernel
   bounce buffer and is copied out only after the whole exchange is over
   and only if the status was 200. One caller at a time: a second task
   calling while a fetch is in flight is -EBUSY, since the bounce buffer
   and net.c's one-connection stack are both singletons. */
#define HTTP_HOST_MAX 64
static char http_host_buf[HTTP_HOST_MAX] = "joshuatree.heyitsmejosh.com";
static unsigned short http_port_val = 80;
#define HTTP_HOST http_host_buf
#define HTTP_PORT http_port_val
/* 1.9.26: facehost=HOST[:PORT] on the boot command line moves where Samantha's face frames come from
   (the checks point it at a local stub). It lived in the kernel chat's face code; this is its new home. */
static int noblink_on = 0; /* `noblink` on the boot line: ring-3 Samantha never blinks, so a check can read exact pixels */
int jt_noblink(void) { return noblink_on; }
static int http_stat_on = 0; /* `httpstat` on the boot line: one serial census line per ring-3 fetch, for tools/checks/httpstress-check.py */
void jt_facehost_cmdline(const char *cl) {
    for (const char *p = cl; p && *p; p++) {
        if (p[0]=='h' && p[1]=='t' && p[2]=='t' && p[3]=='p' && p[4]=='s' && p[5]=='t' && p[6]=='a' && p[7]=='t' && (p == cl || p[-1] == ' ')) http_stat_on = 1;
        if (p[0]=='n' && p[1]=='o' && p[2]=='b' && p[3]=='l' && p[4]=='i' && p[5]=='n' && p[6]=='k' && (p == cl || p[-1] == ' ') && (p[7] == 0 || p[7] == ' ')) noblink_on = 1;
        if (p[0]=='f' && p[1]=='a' && p[2]=='c' && p[3]=='e' && p[4]=='h' && p[5]=='o' && p[6]=='s' && p[7]=='t' && p[8]=='=') {
            p += 9; int n = 0;
            while (*p && *p != ' ' && *p != ':' && n < HTTP_HOST_MAX - 1) http_host_buf[n++] = *p++;
            if (n > 0) http_host_buf[n] = 0;
            if (*p == ':') {
                unsigned int pt = 0; p++;
                while (*p >= '0' && *p <= '9') pt = pt * 10 + (unsigned int)(*p++ - '0');
                if (pt && pt < 65536) http_port_val = (unsigned short)pt;
            }
            return;
        }
    }
}
#define HTTP_BIG_TICKS 300 /* 3s: one face frame */
#define HTTP_WAIT_TICKS 100 /* 1s: how long a ring-3 fetch queues behind the desktop's own fetch before -EBUSY */
#define HTTP_REPLY_TICKS 150 /* 1500ms at 100Hz, the same budget kernel/curbfind.h used */
static char http_bounce[JT_HTTP_BODY_MAX];
static int http_busy = 0;
static void http_stat_line(int n) {
    u32 fb, lg, nb, tl; kheap_stats(&fb, &lg, &nb, &tl);
    char d[96]; int k = 0;
    const char *pre = "httpstat: n=";
    while (*pre) d[k++] = *pre++;
    u32 vals[4] = { (u32)(n < 0 ? -n : n), fb, lg, nb };
    for (int vi = 0; vi < 4; vi++) {
        char t[12]; int nt = 0; u32 v = vals[vi];
        if (!v) t[nt++] = '0';
        while (v) { t[nt++] = (char)('0' + v % 10); v /= 10; }
        if (vi == 0 && n < 0) d[k++] = '-';
        if (vi) { const char *nm = vi == 1 ? " free=" : vi == 2 ? " largest=" : " blocks="; while (*nm) d[k++] = *nm++; }
        while (nt) d[k++] = t[--nt];
    }
    d[k++] = '\n'; d[k] = 0;
    serial_puts(d);
}
static int sys_http_get(u32 path, u32 buf, u32 len) {
    char kpath[JT_HTTP_PATH_MAX + 1];
    u32 i;
    for (i = 0; i <= JT_HTTP_PATH_MAX; i++) {
        if (!paging_user_range_ok(path + i, 1)) return -EFAULT;
        char ch = ((const char *)path)[i];
        kpath[i] = ch;
        if (!ch) break;
        if (ch < 0x21 || ch > 0x7E) return -EINVAL; /* space, CR, LF, control, high bit: not a path */
    }
    if (i > JT_HTTP_PATH_MAX) return -EINVAL; /* no NUL within the cap */
    if (i == 0 || kpath[0] != '/') return -EINVAL;
    /* Caller-supplied larger buffer (Samantha's 320x320 JPEG face frames are ~21KB): over the 2KB
       bounce size the reply lands straight in the user buffer, checked whole first, capped at
       JT_HTTP_BIG_MAX. Up to 2KB keeps the bounce copy exactly as before. */
    if (len > JT_HTTP_BIG_MAX) len = JT_HTTP_BIG_MAX;
    if (!paging_user_range_ok(buf, len ? len : 1)) return -EFAULT;
    int big = len > JT_HTTP_BODY_MAX;
    if (http_busy) return -EBUSY;
    if (task_stack_room() < JT_NET_STACK_MIN) return -ENOMEM; /* 2.0.0: never enter the net path without stack room, see task_stack_room */
    http_busy = 1;
    __asm__ volatile ("sti");
    int n = -1, st = 0;
    if (!http_wait_idle(HTTP_WAIT_TICKS)) { n = -EBUSY; } /* the desktop holds the connection: busy, not a network failure */
    else if (!net_init(0x0A00020F)) { n = -ENODEV; }
    else if (big) {
        n = http_get_timeout(HTTP_HOST, kpath, HTTP_PORT, (char *)buf, len, HTTP_BIG_TICKS);
        http_post_set_bearer(0); /* belt and braces: a failed resolve must not leave it armed */
        st = http_last_status();
        if (n < 0) n = -EIO;
        else if (st != 200) n = st >= 100 && st <= 599 ? -st : -EIO;
    }
    else {
        n = http_get_timeout(HTTP_HOST, kpath, HTTP_PORT, http_bounce, sizeof(http_bounce), HTTP_REPLY_TICKS);
        st = http_last_status();
        if (n < 0) n = -EIO;
        else if (st != 200) n = st >= 100 && st <= 599 ? -st : -EIO;
    }
    __asm__ volatile ("cli");
    http_busy = 0;
    if (http_stat_on) http_stat_line(n);
    if (n < 0) return n;
    if ((u32)n > len) n = (int)len;
    if (big) return n; /* already in place */
    char *out = (char *)buf;
    for (i = 0; i < (u32)n; i++) out[i] = http_bounce[i];
    return n;
}

/* 1.9.26: SYS_HTTP_POST, the chat request Samantha needs once she is a ring-3
   program. Same shape as sys_http_get: every user range is checked before the
   network is touched, the body is copied into a kernel bounce buffer first (so
   the program cannot rewrite it mid-send), the host comes from Settings and
   never from the caller, interrupts go on only around the wait, and the reply
   is copied out after the exchange is over and only on a 200. The two bounce
   buffers are static (14 KB would not fit the 4KB kernel stack) and shared
   with http_get through the same http_busy flag. */
static int path_is_mail_send(const char *p) {
    const char *m = "/api/mail/send";
    while (*m && *p == *m) { p++; m++; }
    return !*m && !*p;
}
static char http_post_body[JT_HTTP_POST_BODY_MAX];
static char http_post_reply[JT_HTTP_POST_REPLY_MAX];
static int sys_http_post(u32 argp, u32 flags, u32 unused2) {
    (void)unused2;
    if (flags & ~(u32)(JT_POST_WORKER | JT_POST_BIG)) return -EINVAL;
    int big = (flags & JT_POST_BIG) != 0;
    if (!paging_user_range_ok(argp, sizeof(struct jt_http_post))) return -EFAULT;
    struct jt_http_post a = *(const struct jt_http_post *)argp; /* one copy; the user struct is not read again */
    char kpath[JT_HTTP_PATH_MAX + 1];
    u32 i;
    for (i = 0; i <= JT_HTTP_PATH_MAX; i++) {
        if (!paging_user_range_ok((u32)a.path + i, 1)) return -EFAULT;
        char ch = a.path[i];
        kpath[i] = ch;
        if (!ch) break;
        if (ch < 0x21 || ch > 0x7E) return -EINVAL;
    }
    if (i > JT_HTTP_PATH_MAX) return -EINVAL;
    if (i == 0 || kpath[0] != '/') return -EINVAL;
    if (a.body_len > (big ? (u32)JT_HTTP_BIG_MAX : (u32)JT_HTTP_POST_BODY_MAX)) return -EINVAL;
    if (!paging_user_range_ok((u32)a.body, a.body_len ? a.body_len : 1)) return -EFAULT;
    if (a.out_len > (big ? (u32)JT_HTTP_BIG_MAX : (u32)JT_HTTP_POST_REPLY_MAX)) a.out_len = big ? JT_HTTP_BIG_MAX : JT_HTTP_POST_REPLY_MAX;
    if (!paging_user_range_ok((u32)a.out, a.out_len ? a.out_len : 1)) return -EFAULT;
    u32 ticks = a.reply_ticks ? a.reply_ticks : JT_HTTP_POST_TICKS_DEFAULT;
    if (ticks > JT_HTTP_POST_TICKS_MAX) ticks = JT_HTTP_POST_TICKS_MAX;
    if (http_busy) return -EBUSY;
    if (task_stack_room() < JT_NET_STACK_MIN) return -ENOMEM; /* 2.0.0: never enter the net path without stack room, see task_stack_room */
    /* Big: no bounce at all. net's POST builder already copies the body into its own
       kmalloc'd request, and the reply is written straight into the checked user range. */
    if (!big) for (i = 0; i < a.body_len; i++) http_post_body[i] = a.body[i];
    http_busy = 1;
    __asm__ volatile ("sti");
    int n, st = 0;
    if (!http_wait_idle(HTTP_WAIT_TICKS)) { n = -EBUSY; }
    else if (!net_init(0x0A00020F)) { n = -ENODEV; }
    else {
        const char *host = (flags & JT_POST_WORKER) ? HTTP_HOST : llm_host_get();
        unsigned short port = (flags & JT_POST_WORKER) ? HTTP_PORT : (unsigned short)llm_port_get();
        /* 1.9.27: the Mail token never crosses into ring 3. The kernel adds the bearer itself, only
           for the Worker's mail route, from the Settings-owned token. */
        if ((flags & JT_POST_WORKER) && path_is_mail_send(kpath)) http_post_set_bearer(mail_token_get());
        if (big) n = http_post_timeout(host, kpath, port, a.body, a.body_len, a.out, a.out_len, ticks);
        else n = http_post_timeout(host, kpath, port, http_post_body, a.body_len, http_post_reply, sizeof(http_post_reply), ticks);
        st = http_last_status();
        if (n < 0) n = -EIO;
        else if (st != 200) n = st >= 100 && st <= 599 ? -st : -EIO;
    }
    __asm__ volatile ("cli");
    http_busy = 0;
    if (n < 0) return n;
    if ((u32)n > a.out_len) n = (int)a.out_len;
    if (big) return n; /* already in place; scratch bytes in out are possible on failure */
    for (i = 0; i < (u32)n; i++) a.out[i] = http_post_reply[i];
    return n;
}

/* 1.9.26: SYS_SYSINFO and SYS_LAUNCH_REQUEST (contract in syscall.h). Both keep the state on the
   kernel side of the gate: sysinfo fills a private copy and copies out once, launch only records
   an index that gui_run picks up with IF on. */
static int sys_sysinfo(u32 out, u32 size, u32 unused) {
    (void)unused;
    struct jt_sysinfo si;
    if (size < 8) return -EINVAL;
    if (size > sizeof si) size = sizeof si;
    if (!paging_user_range_ok(out, size)) return -EFAULT;
    jt_sysinfo_fill(&si);
    si.epoch = rtc_epoch_seconds();
    for (u32 i = 0; i < size; i++) ((char *)out)[i] = ((const char *)&si)[i];
    return (int)size;
}
static int sys_launch_request(u32 name, u32 b, u32 c) {
    (void)b; (void)c;
    char kn[JT_APP_NAME_MAX + 1];
    u32 i;
    for (i = 0; i <= JT_APP_NAME_MAX; i++) {
        if (!paging_user_range_ok(name + i, 1)) return -EFAULT;
        kn[i] = ((const char *)name)[i];
        if (!kn[i]) break;
    }
    if (i > JT_APP_NAME_MAX || i == 0) return -EINVAL;
    return jt_launch_request(kn);
}

static int sys_refresh(u32 kind, u32 arg, u32 c) { (void)c; return jt_refresh_request((int)kind, (int)arg); }
/* 1.9.28: SYS_CLIPBOARD (contract in syscall.h). One kernel buffer shared by every ring-3 app; the
   user bytes are range-checked, then copied in or out under irq_save so a paste never sees a
   half-written copy. The serial lines are what tools/checks/clipboard-check.py greps: the length
   always, an FNV-1a hash only under `cliptrace` (the serial log is host-readable and an unkeyed
   hash of a short pasted password is dictionary-recoverable), never the text. */
static char clip_buf[JT_CLIP_MAX];
static u32 clip_len = 0;
static int clip_trace = 0;
void jt_clip_cmdline(const char *cl) {
    for (const char *p = cl; p && *p; p++)
        if (p[0]=='c' && p[1]=='l' && p[2]=='i' && p[3]=='p' && p[4]=='t' && p[5]=='r' && p[6]=='a' && p[7]=='c' && p[8]=='e') { clip_trace = 1; return; }
}
static void clip_log(const char *tag, const char *s, u32 n) {
    char out[24]; int k = 0; char d[10]; int dn = 0; u32 v = n;
    do { d[dn++] = (char)('0' + v % 10); v /= 10; } while (v);
    while (dn) out[k++] = d[--dn];
    if (clip_trace) {
        u32 h = 2166136261u;
        for (u32 i = 0; i < n; i++) { h ^= (unsigned char)s[i]; h *= 16777619u; }
        out[k++] = ':';
        for (int sh = 28; sh >= 0; sh -= 4) out[k++] = "0123456789abcdef"[(h >> sh) & 15];
    }
    out[k++] = '\n'; out[k] = 0;
    serial_puts(tag);
    serial_puts(out);
}
static int sys_clipboard(u32 op, u32 buf, u32 len) {
    if (op == JT_CLIP_SET) {
        if (len > JT_CLIP_MAX) return -EINVAL;
        if (len && !paging_user_range_ok(buf, len)) return -EFAULT;
        unsigned int f = irq_save();
        for (u32 i = 0; i < len; i++) clip_buf[i] = ((const char *)buf)[i];
        clip_len = len;
        irq_restore(f);
        clip_log("CLIPCOPY:", clip_buf, len);
        return (int)len;
    }
    if (op == JT_CLIP_GET) {
        if (len && !paging_user_range_ok(buf, len)) return -EFAULT;
        unsigned int f = irq_save();
        u32 have = clip_len, n = have < len ? have : len;
        for (u32 i = 0; i < n; i++) ((char *)buf)[i] = clip_buf[i];
        irq_restore(f);
        if (n) {
            clip_log("CLIPPASTE:", clip_buf, n);
            if (n < have) serial_puts("CLIPTRUNC\n");
        }
        return (int)n;
    }
    return -EINVAL;
}

/* 1.9.26: SYS_AUDIO (contract in syscall.h). User PCM goes through paging_user_range_ok, then a
   bounded copy into the driver's ring (sb16_queue takes the irq lock itself). Nothing waits. */
static int sys_audio(u32 op, u32 arg, u32 size) {
    if (op == JT_AUDIO_STOP) { sb16_queue_stop(); return 0; }
    if (op == JT_AUDIO_STATUS) {
        struct jt_audio_status st;
        struct sb16_qstat q;
        if (size < 8) return -EINVAL;
        if (size > sizeof st) size = sizeof st;
        if (!paging_user_range_ok(arg, size)) return -EFAULT;
        sb16_queue_status(&q);
        st.version = JT_AUDIO_STATUS_VERSION; st.size = sizeof st;
        st.playing = q.playing; st.queued = q.queued; st.space = q.space;
        st.rate = q.rate; st.played = q.played;
        for (u32 i = 0; i < size; i++) ((char *)arg)[i] = ((const char *)&st)[i];
        return (int)size;
    }
    if (op != JT_AUDIO_PLAY) return -EINVAL;
    if (size < sizeof(struct jt_audio_play)) return -EINVAL;
    if (!paging_user_range_ok(arg, sizeof(struct jt_audio_play))) return -EFAULT;
    struct jt_audio_play p = *(const struct jt_audio_play *)arg;
    if (!p.len) return -EINVAL;
    if (!sb16_present()) return -ENODEV;
    if (p.len > JT_AUDIO_CHUNK_MAX) { p.len = JT_AUDIO_CHUNK_MAX; p.flags &= ~(u32)JT_AUDIO_END; }
    if (!paging_user_range_ok((u32)p.pcm, p.len)) return -EFAULT;
    return (int)sb16_queue((const unsigned char *)p.pcm, p.len, p.rate, (p.flags & JT_AUDIO_END) ? 1 : 0);
}

/* 1.9.26: SYS_AUDIO_RECORD (contract in syscall.h). Start and stop only flip driver state; read
   copies out of the capture ring after paging_user_range_ok and never waits. */
static int sys_audio_record(u32 op, u32 arg, u32 size) {
    if (op == JT_REC_STOP) { sb16_rec_stop(); return 0; }
    if (op == JT_REC_START) {
        if (!sb16_present()) return -ENODEV;
        return sb16_rec_start(arg) ? 0 : -EBUSY;
    }
    if (op != JT_REC_READ) return -EINVAL;
    if (!size) return -EINVAL;
    if (size > JT_REC_CHUNK_MAX) size = JT_REC_CHUNK_MAX;
    if (!paging_user_range_ok(arg, size)) return -EFAULT;
    return (int)sb16_rec_read((unsigned char *)arg, size);
}

/* 1.9.13: SYS_READDIR, the listing the Search app shows (and Files will).
   The contract is in syscall.h. Order of operations is the point: the
   path is copied out of user space with a hard cap and the output range
   is checked for the whole array before a single vfs call runs; the
   entries are collected into a kernel-side staging table (static, not on
   the 4KB kernel stack) with a hard count; the walk into the directory
   is undone before anything is copied out; and the copy stops at the
   smaller of what was found and what the caller asked for. The user array
   is never touched on an error path. */
static struct jt_dirent readdir_stage[JT_READDIR_MAX];
static u32 readdir_total;
static void readdir_collect(const char *name, unsigned int size, int is_dir) {
    u32 i = readdir_total++;
    if (i >= JT_READDIR_MAX) return; /* counted, not stored: the return value says the listing was cut */
    struct jt_dirent *d = &readdir_stage[i];
    int k = 0;
    while (name[k] && k < JT_DIRENT_NAME - 1) { d->name[k] = name[k]; k++; }
    while (k < JT_DIRENT_NAME) d->name[k++] = 0;
    d->size = is_dir ? 0 : size;
    d->is_dir = is_dir ? 1u : 0u;
}
static int sys_readdir(u32 path, u32 out, u32 max) {
    char walk[PATH_MAX + 1];
    int err = copy_path_from_user(path, walk);
    if (err) return err;
    if (max > JT_READDIR_MAX) max = JT_READDIR_MAX;
    if (max && !paging_user_range_ok(out, max * sizeof(struct jt_dirent))) return -EFAULT;
    if (walk[0] == '.' && !walk[1]) walk[0] = 0; /* "." is the cwd, same as "" */
    char *leaf; int depth;
    err = path_enter(walk, 0, &leaf, &depth);
    if (err) return err;
    readdir_total = 0;
    vfs_list(readdir_collect);
    path_leave(depth);
    u32 n = readdir_total < max ? readdir_total : max;
    struct jt_dirent *dst = (struct jt_dirent *)out;
    for (u32 i = 0; i < n; i++) dst[i] = readdir_stage[i];
    return (int)readdir_total;
}

/* SYS_MKDIR / SYS_UNLINK: same path rules as sys_open (copy_path_from_user
   checks every byte with paging_user_range_ok, path_enter keeps one leaf),
   the walk is undone before return. No heap is touched, so there is no lock
   to take; the gate already runs with IF clear. */
static int sys_mkdir(u32 path, u32 b, u32 c) {
    (void)b; (void)c;
    char name[PATH_MAX + 1];
    int err = copy_path_from_user(path, name);
    if (err) return err;
    char *leaf; int depth;
    err = path_enter(name, 1, &leaf, &depth);
    if (err) return err;
    int ok = vfs_mkdir(leaf);
    path_leave(depth);
    return ok ? 0 : -ENOSPC;
}
/* SYS_UNLINK must never take a folder (vfs_delete would remove a directory
   entry as readily as a file): the listing says which the leaf is. */
static const char *unlink_leaf;
static int unlink_leaf_is_dir;
static void unlink_probe(const char *name, unsigned int size, int is_dir) {
    (void)size;
    const char *a = name, *b = unlink_leaf;
    for (; *a && *b; a++, b++) {
        char x = *a, y = *b;
        if (x >= 'a' && x <= 'z') x -= 32;
        if (y >= 'a' && y <= 'z') y -= 32;
        if (x != y) return;
    }
    if (*a || *b) return;
    if (is_dir) unlink_leaf_is_dir = 1;
}
static int sys_unlink(u32 path, u32 b, u32 c) {
    (void)b; (void)c;
    char name[PATH_MAX + 1];
    int err = copy_path_from_user(path, name);
    if (err) return err;
    char *leaf; int depth;
    err = path_enter(name, 1, &leaf, &depth);
    if (err) return err;
    unlink_leaf = leaf; unlink_leaf_is_dir = 0;
    vfs_list(unlink_probe);
    if (unlink_leaf_is_dir) { path_leave(depth); return -EISDIR; }
    int ok = vfs_delete(leaf);
    path_leave(depth);
    return ok ? 0 : -ENOENT;
}

/* SYS_SHELL_RUN: both user pointers are range checked before anything is
   read or written, the line is copied in with a hard bound, and the work is
   shellsys_run's allowlist (static buffers, no blocking, no windows). */
static int sys_shell_run(u32 line, u32 out, u32 outlen) {
    if (outlen == 0 || outlen > 4096) return -EINVAL;
    if (!paging_user_range_ok(out, outlen)) return -EFAULT;
    char k[JT_SHELL_LINE_MAX + 1];
    u32 n = 0;
    for (;; n++) {
        if (n > JT_SHELL_LINE_MAX) return -EINVAL;
        if (!paging_user_range_ok(line + n, 1)) return -EFAULT;
        char c = ((const char *)line)[n];
        k[n] = c;
        if (!c) break;
    }
    char *res;
    u32 len = shellsys_run(k, &res);
    if (len > outlen - 1) len = outlen - 1;
    char *dst = (char *)out;
    for (u32 i = 0; i < len; i++) dst[i] = res[i];
    dst[len] = 0;
    return (int)len;
}

/* Called from syscall_release_task on exit or fault. 1.7.8: the pages go
   back to supervisor-only here, not just zeroed. Before this, a program
   that had opened a window left JT_USER_FB user-accessible for good, so
   the next ring-3 program (or the same one after exit, in the launcher's
   next exec) could write those pages with no window open, and a syscall
   handed a pointer into them would have passed paging_user_range_ok.
   tools/checks/userfb-release-check.py proves both doors are shut. */
static void window_release(int id) {
    r3win_release(id); /* 1.9.23: a compositor window, if this task had one */
    if (win_owner != id) return;
    u32 *fb = (u32 *)JT_USER_FB;
    for (u32 i = 0; i < win_w * win_h; i++) fb[i] = 0;
    win_owner = -1; win_w = win_h = 0;
    paging_clear_user((void *)JT_USER_FB, JT_USER_FB_BYTES);
    serial_puts("syscall: window released, task gone\n");
}

/* SYS_READFILE (401): one whole file straight into a caller buffer, no 8KB open() snapshot. The
   Music app needs it: SYS_OPEN refuses anything over OPEN_MAX_FILE and SYS_READ moves 255 bytes
   a call. Same relative paths as open; the buffer is checked as user memory for all cap bytes
   before the VFS writes a byte of it. Reads at most cap bytes, so a bigger file is a short read
   the caller can see against the size SYS_READDIR reported. */
#define READFILE_CAP_MAX (6u << 20)
static int sys_readfile(u32 path, u32 buf, u32 cap) {
    if (cap == 0 || cap > READFILE_CAP_MAX) return -EINVAL;
    char name[PATH_MAX + 1];
    int err = copy_path_from_user(path, name);
    if (err) return err;
    if (!paging_user_range_ok(buf, cap)) return -EFAULT;
    char *leaf; int depth;
    err = path_enter(name, 1, &leaf, &depth);
    if (err) return err;
    int n = vfs_read_file(leaf, (void *)buf, cap);
    path_leave(depth);
    return n > 0 ? n : -ENOENT; /* empty and missing read the same through the VFS, as in open() */
}

static int sys_brk(u32 top, u32 b, u32 c) {
    (void)b; (void)c;
    int id = task_current();
    return brk_set(id, task_page_dir(id), top);
}

static const syscall_fn table[NSYSCALLS] = {
    [SYS_EXIT]        = sys_exit,
    [SYS_READ]        = sys_read,
    [SYS_WRITE]       = sys_write,
    [SYS_OPEN]        = sys_open,
    [SYS_CLOSE]       = sys_close,
    [SYS_LSEEK]       = sys_lseek,
    [SYS_TIME]        = sys_time,
    [SYS_GETPID]      = sys_getpid,
    [SYS_SCHED_YIELD] = sys_sched_yield,
    [SYS_WINDOW_OPEN] = sys_window_open,
    [SYS_WINDOW_POLL] = sys_window_poll,
    [SYS_TASKS]       = sys_tasks,
    [SYS_HTTP_GET]    = sys_http_get,
    [SYS_READDIR]     = sys_readdir,
    [SYS_MKDIR]       = sys_mkdir,
    [SYS_UNLINK]      = sys_unlink,
    [SYS_SHELL_RUN]   = sys_shell_run,
    [SYS_HTTP_POST]   = sys_http_post,
    [SYS_AUDIO]       = sys_audio,
    [SYS_AUDIO_RECORD] = sys_audio_record,
    [SYS_SYSINFO]     = sys_sysinfo,
    [SYS_LAUNCH_REQUEST] = sys_launch_request,
    [SYS_BRK]         = sys_brk,
    [SYS_REFRESH]     = sys_refresh,
    [SYS_CLIPBOARD]   = sys_clipboard,
    [SYS_TEXT]        = sys_text,
    [SYS_READFILE]    = sys_readfile,
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
