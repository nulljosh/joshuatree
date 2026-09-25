/* Host-build shim for tools/ttf-host-check.sh: maps the kernel heap API
   onto the host's malloc so drivers/ttf.c compiles unchanged natively. */
#include <stdlib.h>
static inline void *kmalloc(unsigned int n) { return malloc(n); }
static inline void kfree(void *p) { free(p); }
