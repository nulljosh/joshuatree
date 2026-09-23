/* Host-build shim for tools/checks/parser-fuzz-check.sh: maps the kernel
   heap API onto the host's malloc so drivers/http.c compiles unchanged
   natively, the same pattern tools/png-host/kheap.h and
   tools/jpeg-host/kheap.h already use for their own decoders. */
#include <stdlib.h>
static inline void *kmalloc(unsigned int n) { return malloc(n); }
static inline void kfree(void *p) { free(p); }
