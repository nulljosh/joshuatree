/* ATA PIO, LBA28, primary bus, master drive only. ponytail: no IRQ (polls the
   BSY/DRQ status bits instead), no slave/secondary-bus support, no LBA48 for
   >128GB disks -- none of that exists yet to need handling. QEMU always
   attaches the boot disk as primary master, which is all this targets. */
#include "ata.h"
#include "blockdev.h"

typedef unsigned short u16;
typedef unsigned char u8;

static inline u8 inb(u16 p){ u8 v; __asm__ volatile("inb %1,%0":"=a"(v):"Nd"(p)); return v; }
static inline void outb(u16 p, u8 v){ __asm__ volatile("outb %0,%1"::"a"(v),"Nd"(p)); }
static inline u16 inw(u16 p){ u16 v; __asm__ volatile("inw %1,%0":"=a"(v):"Nd"(p)); return v; }
static inline void outw(u16 p, u16 v){ __asm__ volatile("outw %0,%1"::"a"(v),"Nd"(p)); }

#define ATA_DATA        0x1F0
#define ATA_ERROR       0x1F1
#define ATA_SECCOUNT    0x1F2
#define ATA_LBA_LOW     0x1F3
#define ATA_LBA_MID     0x1F4
#define ATA_LBA_HIGH    0x1F5
#define ATA_DRIVE_HEAD  0x1F6
#define ATA_STATUS      0x1F7
#define ATA_COMMAND     0x1F7

#define ATA_CMD_READ    0x20
#define ATA_CMD_WRITE   0x30

#define STATUS_ERR 0x01
#define STATUS_DRQ 0x08
#define STATUS_BSY 0x80

static int wait_ready(void) {
    for (int i = 0; i < 100000; i++) {
        u8 s = inb(ATA_STATUS);
        if (s & STATUS_ERR) return 0;
        if (!(s & STATUS_BSY) && (s & STATUS_DRQ)) return 1;
    }
    return 0; /* timeout */
}

static void select_lba(unsigned int lba) {
    outb(ATA_DRIVE_HEAD, 0xE0 | ((lba >> 24) & 0x0F)); /* master, LBA mode */
    outb(ATA_SECCOUNT, 1);
    outb(ATA_LBA_LOW,  lba & 0xFF);
    outb(ATA_LBA_MID,  (lba >> 8) & 0xFF);
    outb(ATA_LBA_HIGH, (lba >> 16) & 0xFF);
}

int ata_read_sector(unsigned int lba, void *buf) {
    select_lba(lba);
    outb(ATA_COMMAND, ATA_CMD_READ);
    if (!wait_ready()) return 0;
    u16 *p = (u16 *)buf;
    for (int i = 0; i < 256; i++) p[i] = inw(ATA_DATA);
    return 1;
}

int ata_write_sector(unsigned int lba, const void *buf) {
    select_lba(lba);
    outb(ATA_COMMAND, ATA_CMD_WRITE);
    if (!wait_ready()) return 0;
    const u16 *p = (const u16 *)buf;
    for (int i = 0; i < 256; i++) outw(ATA_DATA, p[i]);
    outb(ATA_COMMAND, 0xE7); /* CACHE FLUSH, so QEMU actually persists it */
    wait_ready();
    return 1;
}

/* v33 (0.33.0): ata's own functions already match struct blockdev_ops's
   signatures exactly, same "no adapter needed" shape as fat.c's own
   vfs_ops registration. */
static const struct blockdev_ops ata_blockdev_ops = { "ata", ata_read_sector, ata_write_sector };

void ata_blockdev_register(void) {
    blockdev_register("ata", &ata_blockdev_ops);
}
