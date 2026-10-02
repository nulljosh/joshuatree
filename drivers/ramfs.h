#ifndef RAMFS_H
#define RAMFS_H
/* v29's second real VFS backend, deliberately tiny: proves vfs.c's ops
   table actually abstracts something. Flat namespace except for kernel-seeded
   root-level folders (the demo's DOCS/); mkdir from a program still fails, fixed
   file count and size, in-memory only, gone on reboot. Scratch space, not
   a competitor to FAT. */
int ramfs_seed_dir(const char *name); /* kernel-only: create a root-level folder (programs still cannot mkdir here); 1 on success */
void ramfs_init(void); /* registers itself with vfs_register(), call once at boot */
#endif
