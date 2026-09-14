#ifndef RAMFS_H
#define RAMFS_H
/* v29's second real VFS backend, deliberately tiny: proves vfs.c's ops
   table actually abstracts something. Flat namespace only (no real
   directories, chdir/mkdir are honest no-ops that report failure), fixed
   file count and size, in-memory only, gone on reboot. Scratch space, not
   a competitor to FAT. */
void ramfs_init(void); /* registers itself with vfs_register(), call once at boot */
#endif
