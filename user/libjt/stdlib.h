#ifndef LIBJT_STDLIB_H
#define LIBJT_STDLIB_H
/* libjt: stdlib.h. malloc/calloc/realloc/free run over the task heap SYS_BRK
   grows on demand (1.9.27, kernel/brk.c): first-fit over a free list with
   split and neighbour merge, and the heap top moves up in 64KB steps as
   blocks are needed. Where brk is refused (a host build, or the kernel's
   per-task cap and free-memory floor) the small static arena below is
   the fallback, so a program that never asks for much still works. */

#ifndef JT_ARENA_SIZE
#define JT_ARENA_SIZE (16 * 1024)
#endif

int atoi(const char *s);
long strtol(const char *s, char **endptr, int base);
int abs(int v);
void exit(int code);

void *malloc(unsigned long size);
void *calloc(unsigned long nmemb, unsigned long size);
void *realloc(void *ptr, unsigned long size);
void free(void *ptr);

#endif
