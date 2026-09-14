#include "ramdisk.h"
#include "blockdev.h"

#define RAMDISK_SECTORS 256 /* 128KB, deliberately small, this proves the abstraction, not a real disk */
#define SECTOR_SIZE 512

static unsigned char disk[RAMDISK_SECTORS][SECTOR_SIZE];

static int ramdisk_read_sector(unsigned int lba, void *buf) {
    if (lba >= RAMDISK_SECTORS) return 0;
    unsigned char *dst = (unsigned char *)buf;
    for (int i = 0; i < SECTOR_SIZE; i++) dst[i] = disk[lba][i];
    return 1;
}

static int ramdisk_write_sector(unsigned int lba, const void *buf) {
    if (lba >= RAMDISK_SECTORS) return 0;
    const unsigned char *src = (const unsigned char *)buf;
    for (int i = 0; i < SECTOR_SIZE; i++) disk[lba][i] = src[i];
    return 1;
}

static const struct blockdev_ops ramdisk_ops = { "ramdisk", ramdisk_read_sector, ramdisk_write_sector };

void ramdisk_init(void) {
    for (int i = 0; i < RAMDISK_SECTORS; i++) for (int j = 0; j < SECTOR_SIZE; j++) disk[i][j] = 0;
    blockdev_register("ramdisk", &ramdisk_ops);
}
