#ifndef FAT_H
#define FAT_H
/* FAT16, single fixed volume on the ATA primary master starting at sector 0
   (no partition table yet -- QEMU's raw disk image is the whole filesystem).
   Call fat_mount() once at boot before any other fat_* call. Every fat_*
   call below operates on the current directory, root at mount time, moved
   by fat_chdir(). No long filenames -- 8.3 names exactly as FAT stores
   them. */
int fat_mount(void);

/* Copies up to bufsize bytes of the named file (in the current directory)
   into buf. Returns the number of bytes read, or -1 if the file doesn't
   exist or is a directory. */
int fat_read_file(const char *name_8_3, void *buf, unsigned int bufsize);

/* Lists the current directory's entries by calling cb(name, size, is_dir)
   for each one ("." and ".." excluded). */
void fat_list(void (*cb)(const char *name, unsigned int size, int is_dir));

/* Marks the named file's directory entry deleted (0xE5) and writes that
   sector back. Returns 1 on success, 0 if the file wasn't found or is a
   directory. Doesn't free its clusters in the FAT -- ponytail: no
   free-space reclaim yet, just enough to make a file stop showing up in
   ls/cat. */
int fat_delete(const char *name);

/* Moves the current directory to the named subdirectory, ".." (parent,
   root's own ".." is a no-op), or "." (no-op). Returns 1 on success, 0 if
   the name doesn't exist or isn't a directory. */
int fat_chdir(const char *name);

/* Creates a subdirectory in the current directory: allocates one cluster,
   writes its "." and ".." entries, links it in. Returns 1 on success, 0 if
   the name already exists, the disk is full, or (ponytail: no directory
   growth yet, see fat.c) the current directory's already-allocated space
   is full. */
int fat_mkdir(const char *name);
#endif
