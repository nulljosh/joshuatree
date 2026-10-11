#ifndef EMMC_H
#define EMMC_H
/* The Pi 4's SD card through EMMC2 (emmc.c), and its first FAT partition mounted read only through drivers/fat.c.
   On QEMU's virt machine every call is a stub that says there is no card. */
int emmc_init(void);                                    /* reset the host, bring the card up; 1 when it can be read */
unsigned long emmc_blocks(void);                        /* the card's size in 512-byte blocks, 0 with no card */
int emmc_read(unsigned lba, unsigned count, void *buf); /* CMD17 for one block, CMD18 for more; 0 past the end or on error */
int sd_mount(void);                                     /* emmc_init, the MBR, the first FAT partition, fat_mount */
int sd_ready(void);                                     /* 1 once sd_mount has mounted the partition */
#endif
