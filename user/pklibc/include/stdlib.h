#ifndef PK_STDLIB_H
#define PK_STDLIB_H
#include <stddef.h>
void *malloc(unsigned long n); void *calloc(unsigned long n, unsigned long s); void *realloc(void *p, unsigned long n); void free(void *p);
void exit(int c) __attribute__((noreturn)); void abort(void) __attribute__((noreturn)); char *getenv(const char *n);
double strtod(const char *s, char **e); long long strtoll(const char *s, char **e, int b); long strtol(const char *s, char **e, int b);
int atoi(const char *s); long long atoll(const char *s); unsigned long strtoul(const char *s, char **e, int b); int abs(int v); void qsort(void *b, size_t n, size_t s, int (*cmp)(const void *, const void *));
#endif
