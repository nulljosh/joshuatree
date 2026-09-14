#ifndef TRASH_H
#define TRASH_H
/* v39 (0.39.0): a real trash, not a rename of delete. Before this, `rm`
   went straight to the filesystem and the file was gone; FAT marks the
   directory entry 0xE5 and fat.c's own note says it doesn't even free the
   clusters, so the bytes lingered on disk with no way to ever reach them
   again. Recoverable-by-default is the actual behaviour every desktop OS
   has had for thirty years, and it's the difference between a delete key
   you can use confidently and one you hesitate over.
   Deliberately in RAM, not on disk: a disk-backed trash needs a reserved
   directory, a naming scheme for collisions, and its own free-space
   policy, none of which this kernel has an answer for yet. Gone on
   reboot, honest about it, and still catches the mistake it exists to
   catch, which happens seconds after the delete, not days. */
#define TRASH_MAX_ITEMS 8
#define TRASH_MAX_SIZE  4096
#define TRASH_NAME_LEN  16

void trash_init(void);
/* Copies a file's bytes aside before the caller deletes it. Returns 1 if
   it's safely held, 0 if the trash is full or the file is too big, and
   the caller must decide whether an unrecoverable delete is still what
   the user wanted. */
int  trash_put(const char *name, const void *data, unsigned int len);
int  trash_count(void);
const char *trash_name(int i);
unsigned int trash_size(int i);
/* Writes item i back to the active filesystem and drops it from the
   trash. Returns 1 on success, 0 if the write failed (disk full, name
   taken), in which case the item stays in the trash rather than being
   lost twice. */
int  trash_restore(int i);
void trash_empty(void);
#endif
