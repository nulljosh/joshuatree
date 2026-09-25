#include "stdio.h"
#include "string.h"
#include "../jtsys.h"

/* All-zero FILE structs (stdin) are still an initialised global, not
   .bss material, but the compiler is free to place a zero-initialised
   object in .bss regardless of its initialiser unless pinned like this
   -- and a flat user binary gets no .bss (see user/hello.ld). */
JT_DATA FILE jt_stdin_file  = { 0, 0, 0 };
JT_DATA FILE jt_stdout_file = { 1, 0, 0 };
JT_DATA FILE jt_stderr_file = { 2, 0, 0 };

int putchar(int c) {
    char ch = (char)c;
    jt_write(1, &ch, 1);
    return c;
}

int puts(const char *s) {
    unsigned long n = strlen(s);
    jt_write(1, s, (unsigned)n);
    putchar('\n');
    return 0;
}

int fputs(const char *s, FILE *f) {
    unsigned long n = strlen(s);
    return jt_write(f->fd, s, (unsigned)n);
}

/* Appends up to `size`-1 bytes of `s` into buf at *pos, tracking the
   would-have-been-written total in *total the way real snprintf does, so
   the return value is the length that would have been written even when
   the buffer is too small. */
struct jt_sink { char *buf; unsigned long size; unsigned long pos; };

static void sink_char(struct jt_sink *sk, char c) {
    if (sk->pos + 1 < sk->size) sk->buf[sk->pos] = c;
    sk->pos++;
}
static void sink_str(struct jt_sink *sk, const char *s) {
    while (*s) sink_char(sk, *s++);
}

static void utoa_base(unsigned long v, int base, int upper, char *out) {
    char tmp[32];
    const char *digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    int i = 0;
    if (v == 0) tmp[i++] = '0';
    while (v) { tmp[i++] = digits[v % (unsigned)base]; v /= (unsigned)base; }
    int j = 0;
    while (i) out[j++] = tmp[--i];
    out[j] = 0;
}

int vsnprintf(char *buf, unsigned long size, const char *fmt, va_list ap) {
    struct jt_sink sk = { buf, size, 0 };

    for (const char *p = fmt; *p; p++) {
        if (*p != '%') { sink_char(&sk, *p); continue; }
        p++;
        if (*p == 0) break;

        int zero_pad = 0, width = 0;
        if (*p == '0') { zero_pad = 1; p++; }
        while (*p >= '0' && *p <= '9') { width = width * 10 + (*p - '0'); p++; }

        char num[32];
        const char *s = 0;

        switch (*p) {
            case 'd': case 'i': {
                int v = va_arg(ap, int);
                unsigned long uv = v < 0 ? (unsigned long)(-(long)v) : (unsigned long)v;
                utoa_base(uv, 10, 0, num);
                char signed_buf[33];
                int k = 0;
                if (v < 0) signed_buf[k++] = '-';
                for (const char *q = num; *q; q++) signed_buf[k++] = *q;
                signed_buf[k] = 0;
                s = signed_buf;
                int len = (int)strlen(s);
                for (int pad = width - len; pad > 0; pad--) sink_char(&sk, zero_pad ? '0' : ' ');
                sink_str(&sk, s);
                continue;
            }
            case 'u': utoa_base(va_arg(ap, unsigned int), 10, 0, num); s = num; break;
            case 'x': utoa_base(va_arg(ap, unsigned int), 16, 0, num); s = num; break;
            case 'X': utoa_base(va_arg(ap, unsigned int), 16, 1, num); s = num; break;
            case 'p': {
                unsigned long v = (unsigned long)va_arg(ap, void *);
                num[0] = '0'; num[1] = 'x';
                utoa_base(v, 16, 0, num + 2);
                s = num;
                break;
            }
            case 'c': {
                char cs[2] = { (char)va_arg(ap, int), 0 };
                int len = 1;
                for (int pad = width - len; pad > 0; pad--) sink_char(&sk, ' ');
                sink_char(&sk, cs[0]);
                continue;
            }
            case 's': s = va_arg(ap, const char *); if (!s) s = "(null)"; break;
            case '%': sink_char(&sk, '%'); continue;
            default: sink_char(&sk, '%'); sink_char(&sk, *p); continue;
        }

        int len = (int)strlen(s);
        for (int pad = width - len; pad > 0; pad--) sink_char(&sk, zero_pad ? '0' : ' ');
        sink_str(&sk, s);
    }

    if (size > 0) buf[sk.pos < size ? sk.pos : size - 1] = 0;
    return (int)sk.pos;
}

int snprintf(char *buf, unsigned long size, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int r = vsnprintf(buf, size, fmt, ap);
    va_end(ap);
    return r;
}

int printf(const char *fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    int r = vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    jt_write(1, buf, (unsigned)(r < (int)sizeof buf ? r : (int)sizeof buf - 1));
    return r;
}

/* mode is intentionally ignored beyond choosing read vs write flags:
   v2's open() flags are the only knobs there are, "r"/"w"/"a" map onto
   them directly and anything else falls back to read-only. */
FILE *fopen(const char *path, const char *mode) {
    JT_DATA static FILE files[4];
    JT_DATA static int used = 0;
    int flags = JT_O_RDONLY;
    if (mode && (mode[0] == 'w')) flags = JT_O_WRONLY | JT_O_CREAT | JT_O_TRUNC;
    else if (mode && (mode[0] == 'a')) flags = JT_O_WRONLY | JT_O_CREAT | JT_O_APPEND;

    int fd = jt_open(path, flags);
    if (fd < 0) return 0;
    if (used >= 4) { jt_close(fd); return 0; }
    FILE *f = &files[used++];
    f->fd = fd; f->eof = 0; f->error = 0;
    return f;
}

unsigned long fread(void *ptr, unsigned long size, unsigned long nmemb, FILE *f) {
    unsigned long want = size * nmemb;
    unsigned char *p = ptr;
    unsigned long got = 0;
    while (got < want) {
        unsigned long chunk = want - got;
        if (chunk > 255) chunk = 255;
        int n = jt_read(f->fd, p + got, (unsigned)chunk);
        if (n <= 0) { if (n == 0) f->eof = 1; else f->error = 1; break; }
        got += (unsigned long)n;
    }
    return size ? got / size : 0;
}

unsigned long fwrite(const void *ptr, unsigned long size, unsigned long nmemb, FILE *f) {
    unsigned long want = size * nmemb;
    const unsigned char *p = ptr;
    unsigned long put = 0;
    while (put < want) {
        unsigned long chunk = want - put;
        if (chunk > 255) chunk = 255;
        int n = jt_write(f->fd, p + put, (unsigned)chunk);
        if (n <= 0) { f->error = 1; break; }
        put += (unsigned long)n;
    }
    return size ? put / size : 0;
}

/* Byte-at-a-time: v1/v2 give no buffered file descriptor underneath, and
   this keeps fgets() correct (never reads past the line it returns)
   without needing a pushback buffer. */
char *fgets(char *buf, int size, FILE *f) {
    if (size <= 0) return 0;
    int i = 0;
    while (i < size - 1) {
        char c;
        int n = jt_read(f->fd, &c, 1);
        if (n <= 0) { if (n == 0) f->eof = 1; break; }
        buf[i++] = c;
        if (c == '\n') break;
    }
    if (i == 0) return 0;
    buf[i] = 0;
    return buf;
}

int fclose(FILE *f) {
    if (!f) return -1;
    return jt_close(f->fd);
}
