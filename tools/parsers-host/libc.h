/* Host-build shim: the kernel's libc.h subset (memcpy/memset/memcmp/strlen/
   strcmp) is a strict subset of string.h, so drivers/fat.c's own
   `#include "libc.h"` resolves to this instead of lib/libc.h's freestanding
   declarations when this directory is first on the include path. Same
   pattern tools/png-host/libc.h and tools/jpeg-host/libc.h already use. */
#include <string.h>
