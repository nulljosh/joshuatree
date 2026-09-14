#include "exec.h"
#include "vfs.h"
#include "kheap.h"

#define MAX_FLAT_SIZE (64 * 1024)

int exec_flat(const char *name) {
    void *buf = kmalloc(MAX_FLAT_SIZE);
    if (!buf) return 0;

    /* v29's VFS migration missed this call site (and editor.h's two),
       so `exec` silently stayed hardwired to FAT no matter what
       `fsuse` said was active. Real bug, not cosmetic: the whole point
       of the VFS is that callers stop naming a backend. */
    int n = vfs_read_file(name, buf, MAX_FLAT_SIZE);
    if (n <= 0) { kfree(buf); return 0; } /* the failure path used to leak the whole 64KB buffer */

    void (*entry)(void) = (void (*)(void))buf;
    entry();
    return 1;
}
