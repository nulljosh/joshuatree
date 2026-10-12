#ifndef EMMC_H
#define EMMC_H
/* The Pi 4's SD card through EMMC2 (emmc.c), and its first FAT partition mounted through drivers/fat.c.
   The OS writes only inside the JT folder, and never a name the firmware boots from.
   On QEMU's virt machine the card is the virtio disk (M3d), with the same MBR and FAT partition; with no disk there is none. */
int emmc_init(void);                                    /* reset the host, bring the card up; 1 when it can be read */
unsigned long emmc_blocks(void);                        /* the card's size in 512-byte blocks, 0 with no card */
int emmc_read(unsigned lba, unsigned count, void *buf); /* CMD17 for one block, CMD18 for more; 0 past the end or on error */
int emmc_write(unsigned lba, unsigned count, const void *buf);   /* CMD24 or CMD25, then the busy wait; 0 past the end or on error */
int sd_mount(void);                                     /* emmc_init, the MBR, the first FAT partition, fat_mount */
int sd_ready(void);                                     /* 1 once sd_mount has mounted the partition */
/* JT/NAME on the card, NAME an 8.3 name. 1 done, 0 not (no card, no such file, card or folder full). A boot file's name
   (config.txt, kernel8.img, start4.elf, *.dtb, overlays and the rest) or a name that is not 8.3 prints one
   `sd refused NAME, ...` line and returns -1. */
int sd_write(const char *name, const void *data, unsigned len);  /* create or replace JT/NAME (JT is made on the first write) */
int sd_remove(const char *name);                        /* delete JT/NAME and free its clusters */
int sd_mkdir(const char *name);                         /* make the folder JT/NAME */
int sd_enter_jt(int make);   /* M3d, for ufile.c: from the root into JT/ (made first when make); 0 with no card or no JT/ */
#endif
