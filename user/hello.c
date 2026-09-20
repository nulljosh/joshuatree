/* hello: the reference program for the v1 syscall ABI.
 *
 * This is the first thing in this repo that is a real external consumer of
 * the kernel: it is compiled on its own, links against no kernel header,
 * calls no kernel function, and reaches the kernel only through the eight
 * numbers in docs/SYSCALL-ABI.md. If any of those numbers or their
 * argument meanings change, this program breaks, which is exactly what a
 * MAJOR version is supposed to protect.
 *
 * It walks one real path end to end: open a file off the VFS, read it,
 * write what it read to stdout, ask for its own pid, ask for the time,
 * give up the CPU, and exit with a code the kernel observes.
 *
 * No libc, no crt0. _start is the entry point and it is the first thing
 * in the binary (see hello.ld), because exec_user() jumps to the load
 * address itself, not to an ELF header's e_entry. Nothing here may
 * `return` from _start either: there is no caller to return to.
 */
#include "jtsys.h"

#define STDOUT 1
#define STDERR 2

static unsigned slen(const char *s) { unsigned n = 0; while (s[n]) n++; return n; }
static void put(const char *s) { jt_write(STDOUT, s, slen(s)); }

/* Decimal, into a caller-supplied buffer. Handles the one negative case
   that matters here (a -errno the program decided to print) rather than
   pretending everything is unsigned. */
static char *utoa(int v, char *buf) {
    char tmp[12];
    int i = 0, neg = v < 0;
    unsigned u = neg ? (unsigned)(-v) : (unsigned)v;
    do { tmp[i++] = (char)('0' + u % 10); u /= 10; } while (u);
    char *p = buf;
    if (neg) *p++ = '-';
    while (i) *p++ = tmp[--i];
    *p = 0;
    return buf;
}

/* One label-plus-number line, composed in a user buffer and handed to the
   kernel as a single write. Deliberately not three calls: write() is the
   only way this program can be observed, so "one logical line, one
   syscall" is what makes its output assertable by
   tools/checks/usertest-check.sh rather than something that happens to
   look right on a screen. */
static void put_num(const char *label, int v) {
    char line[64], num[16];
    unsigned i = 0;
    for (const char *p = label; *p && i < 40; p++) line[i++] = *p;
    for (const char *p = utoa(v, num); *p && i < 62; p++) line[i++] = *p;
    line[i++] = '\n';
    jt_write(STDOUT, line, i);
}

__attribute__((section(".text.start"), used))
void _start(void) {
    put("jt-hello: start\n");

    int fd = jt_open("HELLO.TXT", 0);
    if (fd < 0) {
        put_num("jt-hello: open failed ", fd);
        jt_exit(1);
    }
    put_num("jt-hello: fd=", fd);

    char buf[64];
    int n = jt_read(fd, buf, sizeof buf);
    if (n < 0) {
        put_num("jt-hello: read failed ", n);
        jt_exit(2);
    }
    /* Straight back out to stdout: the point is that the bytes made the
       whole trip, kernel -> user buffer -> kernel, with the kernel never
       trusting either pointer. */
    jt_write(STDOUT, buf, (unsigned)n);
    put_num("jt-hello: read=", n);

    /* A second read must report end of file, not the same bytes again:
       the per-fd offset is part of the contract. */
    int again = jt_read(fd, buf, sizeof buf);
    put_num("jt-hello: eof=", again);

    if (jt_close(fd) != 0) { put("jt-hello: close failed\n"); jt_exit(3); }
    /* Closing twice must fail, which is how we know close really released
       the descriptor instead of returning 0 unconditionally. */
    if (jt_close(fd) == 0) { put("jt-hello: double close succeeded\n"); jt_exit(4); }

    put_num("jt-hello: pid=", jt_getpid());

    unsigned now = 0;
    int t = jt_time(&now);
    /* Anything after 2020-01-01 is a real clock rather than a stuck zero.
       The exact value is the host's, so it can't be asserted, only its
       shape. */
    put(t > 1577836800 && now == (unsigned)t ? "jt-hello: time ok\n" : "jt-hello: time suspect\n");

    if (jt_sched_yield() != 0) { put("jt-hello: yield failed\n"); jt_exit(5); }
    put("jt-hello: yielded\n");

    /* An unassigned number must come back -ENOSYS, not wander into an
       empty table slot. 99 is not in the v1 set and is not reserved. */
    int bad = jt_syscall(99, 0, 0, 0);
    put_num("jt-hello: nosys=", bad);

    put("jt-hello: done\n");
    jt_exit(9);
}
