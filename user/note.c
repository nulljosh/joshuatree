/* note: a tiny file scratchpad, and the reference program for the v2
 * syscall ABI.
 *
 *     note FILE                 print FILE
 *     note FILE text...         append "text...\n" to FILE, creating it
 *     note FILE @N text...      overwrite FILE's bytes at offset N
 *
 * Unlike user/hello.c, which exists only to exercise the interface, this
 * is a program someone would actually run: it is a note taker that keeps
 * a plain text file, and the third form is the smallest useful editing
 * primitive a file can have. It is also the only thing in this repo that
 * can change a file from ring 3, so it is the real proof that v2's write
 * path works end to end rather than on paper.
 *
 * Built exactly like hello: compiled on its own, against user/jtsys.h and
 * no kernel header, linked flat at the window boot/linker.ld reserves,
 * entered at offset 0 with no crt0. What is new is the entry signature.
 * The loader leaves the i386 cdecl frame a plain C function expects, so
 * _start takes argc and argv directly with no assembly shim (see
 * kernel/exec.c's build_user_stack and docs/SYSCALL-ABI.md's v2 section).
 * It must not return: there is no caller, and the return address on the
 * stack is 0.
 */
#include "jtsys.h"

#define STDOUT 1
#define STDERR 2

#define LINE_MAX 200 /* one note line, bounded because this program has one page of stack */

static unsigned slen(const char *s) { unsigned n = 0; while (s[n]) n++; return n; }
static void err(const char *s)  { jt_write(STDERR, s, slen(s)); }

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

/* One label-plus-number line as a single write, the same discipline
   hello.c uses and for the same reason: write() is how this program is
   observed, so one logical line per syscall is what makes the output
   assertable instead of merely legible. */
static void put_num(const char *label, int v) {
    char line[64], num[16];
    unsigned i = 0;
    for (const char *p = label; *p && i < 44; p++) line[i++] = *p;
    for (const char *p = utoa(v, num); *p && i < 62; p++) line[i++] = *p;
    line[i++] = '\n';
    jt_write(STDOUT, line, i);
}

/* Decimal, no sign, no leading-zero handling: returns -1 on anything that
   is not a run of digits, which is what makes "@x" a usage error rather
   than an accidental offset 0. */
static int atou(const char *s) {
    if (!*s) return -1;
    int v = 0;
    for (; *s; s++) {
        if (*s < '0' || *s > '9') return -1;
        v = v * 10 + (*s - '0');
        if (v > 1000000) return -1;
    }
    return v;
}

/* Streams a whole file to stdout in ABI-sized bites. 255 is write()'s and
   read()'s own per-call ceiling, so this is the largest honest chunk, and
   the loop rather than one big call is the whole reason the limit is not
   a problem. */
static int cat(const char *path) {
    int fd = jt_open(path, JT_O_RDONLY);
    if (fd < 0) return fd;
    char buf[255];
    for (;;) {
        int n = jt_read(fd, buf, sizeof buf);
        if (n <= 0) { if (n < 0) { jt_close(fd); return n; } break; }
        jt_write(STDOUT, buf, (unsigned)n);
    }
    /* The size, read back through lseek rather than counted, so the number
       printed is the kernel's own idea of the file's length. */
    int size = jt_lseek(fd, 0, JT_SEEK_END);
    jt_close(fd);
    return size;
}

/* Joins argv[from..argc) with single spaces into buf. Returns the length,
   or -1 if it would not fit. Bounded rather than truncated: a note that
   silently lost its tail is worse than a note that was refused. */
static int join(char **argv, int from, int argc, char *buf, unsigned cap) {
    unsigned n = 0;
    for (int i = from; i < argc; i++) {
        if (i > from) { if (n + 1 >= cap) return -1; buf[n++] = ' '; }
        for (const char *p = argv[i]; *p; p++) { if (n + 1 >= cap) return -1; buf[n++] = *p; }
    }
    return (int)n;
}

__attribute__((section(".text.start"), used))
void _start(int argc, char **argv) {
    const char *self = argc > 0 ? argv[0] : "note";

    if (argc < 2) {
        err("usage: ");
        err(self);
        err(" FILE [@OFFSET] [text...]\n");
        jt_exit(2);
    }

    const char *path = argv[1];

    /* note FILE -- print it. */
    if (argc == 2) {
        int size = cat(path);
        if (size < 0) { put_num("note: cannot read, errno ", -size); jt_exit(1); }
        put_num("note: bytes=", size);
        jt_exit(0);
    }

    /* note FILE @N text... -- overwrite in place. This is the form that
       needs a real seek: the descriptor is opened for writing without
       O_TRUNC, so it still holds the file's existing bytes, lseek moves
       the cursor into the middle of them, and write() replaces exactly
       what it covers. */
    if (argv[2][0] == '@') {
        int off = atou(argv[2] + 1);
        if (off < 0 || argc < 4) {
            err("usage: note FILE @OFFSET text...\n");
            jt_exit(2);
        }
        char line[LINE_MAX];
        int n = join(argv, 3, argc, line, sizeof line);
        if (n < 0) { err("note: replacement too long\n"); jt_exit(2); }

        int fd = jt_open(path, JT_O_WRONLY);
        if (fd < 0) { put_num("note: cannot open for writing, errno ", -fd); jt_exit(1); }
        int at = jt_lseek(fd, off, JT_SEEK_SET);
        if (at < 0) { put_num("note: cannot seek there, errno ", -at); jt_close(fd); jt_exit(1); }
        int w = jt_write(fd, line, (unsigned)n);
        if (w < 0) { put_num("note: write failed, errno ", -w); jt_close(fd); jt_exit(1); }
        /* close() is where the bytes actually reach the filesystem, and it
           is the call that can still fail, so its result is checked like
           any other. */
        int cerr = jt_close(fd);
        if (cerr != 0) { put_num("note: close failed, errno ", -cerr); jt_exit(1); }
        put_num("note: patched at ", at);
        int size = cat(path);
        if (size < 0) { put_num("note: cannot read back, errno ", -size); jt_exit(1); }
        put_num("note: bytes=", size);
        jt_exit(0);
    }

    /* note FILE text... -- append a line, creating the file if needed. */
    {
        char line[LINE_MAX];
        int n = join(argv, 2, argc, line, sizeof line - 1);
        if (n < 0) { err("note: line too long\n"); jt_exit(2); }
        line[n++] = '\n';

        int fd = jt_open(path, JT_O_WRONLY | JT_O_CREAT | JT_O_APPEND);
        if (fd < 0) { put_num("note: cannot open for append, errno ", -fd); jt_exit(1); }
        int w = jt_write(fd, line, (unsigned)n);
        if (w != n) { put_num("note: short write ", w); jt_close(fd); jt_exit(1); }
        int cerr = jt_close(fd);
        if (cerr != 0) { put_num("note: close failed, errno ", -cerr); jt_exit(1); }
        put_num("note: added ", w);

        int size = cat(path);
        if (size < 0) { put_num("note: cannot read back, errno ", -size); jt_exit(1); }
        put_num("note: bytes=", size);
        jt_exit(0);
    }
}
