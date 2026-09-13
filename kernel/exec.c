#include "exec.h"
#include "fat.h"
#include "kheap.h"

#define MAX_FLAT_SIZE (64 * 1024)

int exec_flat(const char *name) {
    void *buf = kmalloc(MAX_FLAT_SIZE);
    if (!buf) return 0;

    int n = fat_read_file(name, buf, MAX_FLAT_SIZE);
    if (n <= 0) return 0;

    void (*entry)(void) = (void (*)(void))buf;
    entry();
    return 1;
}
