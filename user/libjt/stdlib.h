#ifndef LIBJT_STDLIB_H
#define LIBJT_STDLIB_H
/* libjt: stdlib.h. malloc/calloc/realloc/free sit over a fixed static
   arena in .bss (JT_ARENA_SIZE below), because v1/v2 give a program no
   brk/mmap: the eight pages it is loaded with are all the memory it will
   ever have, and one page of that is the stack (see jtsys.h and
   docs/SYSCALL-ABI.md). The arena is a bump allocator with a free list
   for exact-size reuse; free() cannot reclaim a partial block and merge
   is not attempted, so long-running allocation churn will eventually
   exhaust JT_ARENA_SIZE -- that ceiling, not "out of RAM", is the real
   limit a libjt program has to respect. */

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
