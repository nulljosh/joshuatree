#ifndef EXEC_H
#define EXEC_H
/* Loads a flat (headerless) binary from the FAT root directory and calls
   into it like a cdecl void(void) function. Returns 0 if the file wasn't
   found or was too big, 1 on success (after the binary returns via `ret`).
   ponytail: runs in ring 0 -- v3's ring-3/TSS work is deliberately deferred
   (see roadmap.md), so there's no real isolation yet. A flat binary loaded
   this way has full kernel privileges. Treat it as a kernel module, not an
   untrusted user program, until user mode actually exists. */
int exec_flat(const char *name);
#endif
