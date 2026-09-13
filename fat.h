#ifndef FAT_H
#define FAT_H
/* Read-only FAT16, single fixed volume on the ATA primary master starting
   at sector 0 (no partition table yet -- QEMU's raw disk image is the whole
   filesystem). Call fat_mount() once at boot before any other fat_* call. */
int fat_mount(void);

/* Copies up to bufsize bytes of the named root-directory file into buf.
   Returns the number of bytes read, or -1 if the file doesn't exist.
   ponytail: root directory only, no subdirectories, no long filenames --
   8.3 names exactly as FAT stores them. Add directory traversal when
   something needs to live in a folder. */
int fat_read_file(const char *name_8_3, void *buf, unsigned int bufsize);

/* Lists root-directory entries by calling cb(name, size) for each file. */
void fat_list(void (*cb)(const char *name, unsigned int size));
#endif
