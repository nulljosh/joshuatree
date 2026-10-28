#ifndef PK_STDIO_H
#define PK_STDIO_H
/* pklibc (6.57.0): the C library Plank's runtime needs on Joshua Tree, enough of it for its programs to run as apps. */
#include <stddef.h>
#include <stdarg.h>
typedef long ssize_t;
typedef struct { int fd, eof, err; } FILE;
extern FILE pk_stdin, pk_stdout, pk_stderr;
#define stdin (&pk_stdin)
#define stdout (&pk_stdout)
#define stderr (&pk_stderr)
#define EOF (-1)
int printf(const char *f, ...); int fprintf(FILE *s, const char *f, ...); int sprintf(char *b, const char *f, ...);
int snprintf(char *b, size_t n, const char *f, ...); int vsnprintf(char *b, size_t n, const char *f, va_list ap); int vfprintf(FILE *s, const char *f, va_list ap);
int fputs(const char *s, FILE *f); int fputc(int c, FILE *f); int putchar(int c); int puts(const char *s); int fflush(FILE *f);
FILE *fopen(const char *p, const char *m); size_t fread(void *b, size_t s, size_t n, FILE *f); size_t fwrite(const void *b, size_t s, size_t n, FILE *f);
int fclose(FILE *f); ssize_t getline(char **line, size_t *cap, FILE *f); FILE *popen(const char *c, const char *m); int pclose(FILE *f);
int remove(const char *p);
#endif
