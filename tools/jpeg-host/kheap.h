/* Host-build shim for tools/checks/jpeg-host-check.sh: maps the kernel
   heap API onto the host's malloc so drivers/jpeg.c compiles unchanged
   natively. Identical to tools/png-host/kheap.h on purpose: both decoders
   share the same kmalloc/kfree contract. */
#include <stdlib.h>
static inline void *kmalloc(unsigned int n) { return malloc(n); }
static inline void kfree(void *p) { free(p); }
