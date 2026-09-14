#ifndef ATA_H
#define ATA_H
/* Reads/writes one 512-byte sector via LBA28 on the primary ATA bus, master
   drive. Returns 1 on success, 0 on failure (no drive, timeout, or error
   status). buf must be at least 512 bytes. */
int ata_read_sector(unsigned int lba, void *buf);
int ata_write_sector(unsigned int lba, const void *buf);

/* v33 (0.33.0): registers ata's own functions with blockdev.c as the
   "ata" backend. Call once at boot. */
void ata_blockdev_register(void);
#endif
