#ifndef LIBC_H
#define LIBC_H
/* Minimal freestanding subset -- just what this kernel actually calls.
   Not a real libc: no headers match glibc/musl signatures beyond what's
   used here, no locale/wide-char/anything. Add a function when something
   above the kernel needs it, not preemptively. */
void *memcpy(void *dst, const void *src, unsigned int n);
void *memset(void *dst, int c, unsigned int n);
int   memcmp(const void *a, const void *b, unsigned int n);
unsigned int strlen(const char *s);
int   strcmp(const char *a, const char *b);
#endif
