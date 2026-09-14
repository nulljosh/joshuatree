#ifndef RAMDISK_H
#define RAMDISK_H
/* v33 (0.33.0)'s second real blockdev backend, proving the abstraction:
   a fixed, in-memory, gone-on-reboot disk. Real sector I/O (same
   read_sector/write_sector contract ata.c honors), just backed by a
   static array instead of real hardware. Starts all-zero, so fat_mount()
   against it correctly reports "no filesystem found", exactly what a
   real unformatted disk would do, not special-cased. */
void ramdisk_init(void); /* registers itself with blockdev_register(), call once at boot */
#endif
