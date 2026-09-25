#ifndef LIBJT_STDIO_H
#define LIBJT_STDIO_H
/* libjt: a minimal stdio.h. FILE wraps one jt syscall fd plus a tiny
   read-ahead buffer for fgets(); there is no host FILE* underneath,
   just jt_open/jt_read/jt_write/jt_close from jtsys.h. printf/vsnprintf
   support %d %i %u %x %X %c %s %p %% with a width and zero-pad flag,
   which is what the userland programs in this repo actually print --
   no floats, no positional args, no '*' width. */

typedef __builtin_va_list va_list;
#define va_start(ap, last) __builtin_va_start(ap, last)
#define va_end(ap)         __builtin_va_end(ap)
#define va_arg(ap, type)   __builtin_va_arg(ap, type)

typedef struct {
    int fd;
    int eof;
    int error;
} FILE;

extern FILE jt_stdin_file, jt_stdout_file, jt_stderr_file;
#define stdin  (&jt_stdin_file)
#define stdout (&jt_stdout_file)
#define stderr (&jt_stderr_file)

int putchar(int c);
int puts(const char *s);
int fputs(const char *s, FILE *f);

int vsnprintf(char *buf, unsigned long size, const char *fmt, va_list ap);
int snprintf(char *buf, unsigned long size, const char *fmt, ...);
int printf(const char *fmt, ...);

FILE *fopen(const char *path, const char *mode);
unsigned long fread(void *ptr, unsigned long size, unsigned long nmemb, FILE *f);
unsigned long fwrite(const void *ptr, unsigned long size, unsigned long nmemb, FILE *f);
char *fgets(char *buf, int size, FILE *f);
int fclose(FILE *f);

#endif
