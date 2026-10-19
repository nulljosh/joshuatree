/* The kernel heap's one call drivers/fat.c makes (its mount scan's buffers, 4.14.0), as plain malloc for host builds. */
#include <stdlib.h>
void *kmalloc(unsigned int n) { return malloc(n); }
