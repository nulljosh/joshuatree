/* Freestanding <string.h> for the vendored BearSSL pieces. inner.h wants
   memcpy/memset/memcmp/memmove and size_t; the kernel's lib/libc.h has
   the first three with unsigned int (== size_t on i386). */
#ifndef BEARSSL_SHIM_STRING_H
#define BEARSSL_SHIM_STRING_H
#include <stddef.h>
#include "libc.h"
void *memmove(void *dst, const void *src, unsigned int n);
#endif
