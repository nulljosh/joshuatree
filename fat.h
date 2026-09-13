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

/* Marks the named file's directory entry deleted (0xE5) and writes that
   sector back. Returns 1 on success, 0 if the file wasn't found. Doesn't
   free its clusters in the FAT -- ponytail: no free-space reclaim yet,
   just enough to make a file stop showing up in ls/cat. */
int fat_delete(const char *name);
#endif
