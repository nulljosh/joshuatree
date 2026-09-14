#ifndef BLOCKDEV_H
#define BLOCKDEV_H
/* v33 (0.33.0): a real block device abstraction, the same reasoning v29's
   VFS used and correctly deferred until now for the same reason, ata.c
   was the only implementation that existed. fat.c calls blockdev_*
   instead of ata_* directly, one active backend at a time, real to prove
   with a second backend (a RAM disk), not hypothetical. */
struct blockdev_ops {
    const char *name;
    int (*read_sector)(unsigned int lba, void *buf);
    int (*write_sector)(unsigned int lba, const void *buf);
};

void blockdev_register(const char *name, const struct blockdev_ops *ops);
int  blockdev_switch(const char *name);
const char *blockdev_current_name(void);

int blockdev_read_sector(unsigned int lba, void *buf);
int blockdev_write_sector(unsigned int lba, const void *buf);
#endif
