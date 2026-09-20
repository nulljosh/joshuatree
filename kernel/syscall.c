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
#include "idt.h"
#include "task.h"
#include "paging.h"
#include "console.h"
#include "serial.h"
#include "kheap.h"
#include "irq.h"
#include "vfs.h"

typedef unsigned int u32;
typedef unsigned char u8;
typedef int (*syscall_fn)(u32 a, u32 b, u32 c);

#define EIO      5
#define EBADF   9
#define EAGAIN  11
#define ENOMEM  12
#define EFAULT  14
#define EINVAL  22
#define EMFILE  24
#define ENOSPC  28
#define EFBIG   27
#define ESPIPE  29
#define ENOSYS  38
#define ENOENT   2

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

    char *buf = (char *)kmalloc(OPEN_MAX_FILE + 1);
    if (!buf) return -ENOMEM;
    int n = vfs_read_file(name, buf, OPEN_MAX_FILE + 1);
    if (n > OPEN_MAX_FILE) { kfree(buf); return -EINVAL; } /* bigger than one open can hold; refused, never truncated */
    /* A backend read returns a byte count and nothing else, so a
       zero-length file and a missing file are the same answer here. v1
       already resolved that ambiguity as "missing" and this keeps it. */
    int exists = (n > 0);
    if (!exists && !(flags & JT_O_CREAT)) { kfree(buf); return -ENOENT; }

    u32 size = exists ? (u32)n : 0;
    if (flags & JT_O_TRUNC) size = 0;

    /* O_CREAT and O_TRUNC take effect now, not at close: the file exists,
       and is empty, the moment open() returns. Only the bytes a program
       goes on to write are deferred to close. Doing half of it at open and
       half at close would be the confusing shape. */
    if (!exists || (flags & JT_O_TRUNC)) {
        if (!vfs_replace_file(name, "", 0)) { kfree(buf); return -ENOSPC; }
    }

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
    if (!vfs_replace_file(f->name, f->data, f->size)) return -EIO;
    char *back = (char *)kmalloc(OPEN_MAX_FILE + 1);
    if (!back) return -ENOMEM;
    int n = vfs_read_file(f->name, back, OPEN_MAX_FILE + 1);
    int ok = (n >= 0) && ((u32)n == f->size);
    for (u32 i = 0; ok && i < f->size; i++) if (back[i] != f->data[i]) ok = 0;
    kfree(back);
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

void syscall_release_task(int id) {
    if (id < 0 || id >= TASK_SLOTS) return;
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
