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

#define EBADF   9
#define EAGAIN  11
#define ENOMEM  12
#define EFAULT  14
#define EINVAL  22
#define EMFILE  24
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
};
static struct open_file fds[TASK_SLOTS][MAX_FDS];

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
    if (flags != 0) return -EINVAL; /* v1 is read-only: O_RDONLY is the only accepted flag word */
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
    if (n <= 0) { kfree(buf); return -ENOENT; }
    if (n > OPEN_MAX_FILE) { kfree(buf); return -EINVAL; } /* bigger than one open can hold; refused, never truncated */

    fds[id][fd].data = buf;
    fds[id][fd].size = (u32)n;
    fds[id][fd].pos  = 0;
    serial_puts("syscall: open ok\n");
    return fd;
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
    u32 left = f->size - f->pos;
    if (left == 0) return 0; /* real end of file, the one case that is 0 rather than an errno */
    u32 n = len < left ? len : left;
    for (u32 i = 0; i < n; i++) dst[i] = f->data[f->pos + i];
    f->pos += n;
    return (int)n;
}

static int close_fd(int id, u32 fd) {
    if (fd < FIRST_FD || fd >= MAX_FDS || !fds[id][fd].data) return -EBADF;
    kfree(fds[id][fd].data);
    fds[id][fd].data = 0;
    fds[id][fd].size = fds[id][fd].pos = 0;
    return 0;
}

static int sys_close(u32 fd, u32 b, u32 c) {
    (void)b; (void)c;
    int id = task_current();
    if (id < 0 || id >= TASK_SLOTS) return -EBADF;
    return close_fd(id, fd);
}

void syscall_release_task(int id) {
    if (id < 0 || id >= TASK_SLOTS) return;
    for (int i = FIRST_FD; i < MAX_FDS; i++) close_fd(id, (u32)i);
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
