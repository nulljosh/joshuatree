/* M4 SD card: the Pi 4 reads the card it booted from. The slot is wired to EMMC2, the BCM2711's own SD Host Controller
   (spec 3.0) at 0xFE340000, on pins of its own: no GPIO setup here, and the Wi-Fi chip's host (the Arasan at 0xFE300000,
   wifi.c) is never touched. Shape from Circle's lib/sdcard/emmc.cpp and the SD Host Controller and SD Physical Layer specs.
   Bring-up: reset the host, bus power on at 3.3 V, the base clock from the firmware, 400 kHz, CMD0, CMD8 (a version 2
   card echoes 0x1AA), ACMD41 with HCS until the card is ready (its CCS bit then says SDHC or SDXC, block addressed),
   CMD2, CMD3, CMD9 (the size), CMD7, CMD16 512, then 25 MHz on a 1-bit bus. Reads are polled PIO, 32 bits at a time:
   CMD17 for one block, CMD18 with Auto CMD12 for more. Writes (2.57.0) are the same shape the other way: CMD24 for one
   block, CMD25 with Auto CMD12 for more, then a wait while the card is busy programming (transfer done, then CMD13
   until it is back in the transfer state). Every wait ends on the generic counter (never `ticks`), and any failure
   prints one `sd ...` line and returns, so the boot carries on.
   Then the MBR's first FAT partition is mounted through drivers/fat.c, the i386 kernel's own FAT code, which reads and
   writes through blockdev_read_sector and blockdev_write_sector below: sector numbers inside the partition, refused
   past its end, and a write never lands on its boot sectors. The OS's own files go in a JT folder (sd_write, sd_remove,
   sd_mkdir), and no name the firmware boots from is ever written. QEMU's raspi4b models EMMC2 at the same address;
   tools/checks/arm64-sd-check.py plugs a card into it. M3d: on QEMU's virt machine the four emmc_* calls go to the
   virtio disk instead, and everything from the partition up is the same code; the apps' file calls (ufile.c) root
   their paths in JT/ through sd_enter_jt. */
#include "emmc.h"
#include "../../drivers/fat.h"
struct vfs_ops;
void vfs_register(const char *name, const struct vfs_ops *ops) { (void)name; (void)ops; }   /* fat_vfs_register's call: ARM has no VFS yet, cmd.c calls fat_* itself */
void kputs(const char *s); void kdec(unsigned v); void kx(unsigned v);
static unsigned long blocks;   /* the card's size in 512-byte blocks */
#ifdef PI_BUILD
#define R32(a) (*(volatile unsigned *)(unsigned long)(a))
unsigned mbox_clock_rate(unsigned id);   /* main.c: a firmware clock's rate in Hz, 0 when the firmware does not answer */
#define EMMC 0xFE340000UL
#define BLK     0x04   /* block size (11:0) and count (31:16) */
#define ARG     0x08
#define CMDTM   0x0C   /* transfer mode (15:0) and command (31:16): one 32-bit store sends the command */
#define RESP    0x10   /* four words */
#define DATA    0x20
#define STATE   0x24   /* bit 0 command inhibit, bit 1 data inhibit */
#define CTL0    0x28   /* host control (bus width, speed) and power: bit 8 on, 11:9 the voltage, 7 is 3.3 V */
#define CTL1    0x2C   /* clock (15:0), data timeout (19:16), software resets (26:24) */
#define INT     0x30   /* bit 0 command done, 1 transfer done, 4 write ready, 5 read ready, 15 any error */
#define INTMASK 0x34
#define INTEN   0x38
#define CTL2    0x3C
/* Response types, the command half of CMDTM: bits 17:16 the length, 19 check the CRC, 20 check the index, 21 data. */
#define RNONE 0
#define R1    0x1A0000u   /* 48 bits, CRC and index checked: R1, R6, R7 */
#define R1B   0x1B0000u   /* R1 with busy on DAT0 */
#define R2    0x090000u   /* 136 bits, CRC checked: the CID and CSD */
#define R3    0x020000u   /* 48 bits, nothing checked: ACMD41's OCR */
#define DAT   0x200000u
#define TM_READ  0x10u
#define TM_MULTI 0x26u    /* multi-block, Auto CMD12, block count on */

static unsigned long now(void) { unsigned long t; __asm__ volatile ("isb\n mrs %0, cntpct_el0" : "=r"(t)); return t; }
static unsigned long ms(unsigned n) { unsigned long f; __asm__ volatile ("mrs %0, cntfrq_el0" : "=r"(f)); return (f ? f : 54000000) / 1000 * n; }
static void mdelay(unsigned n) { unsigned long t0 = now(), d = ms(n); while (now() - t0 < d) {} }
static int wait_clear(unsigned reg, unsigned bits, unsigned n) {   /* 1 once `bits` of `reg` read 0, within n ms */
    unsigned long t0 = now(), d = ms(n);
    while (R32(EMMC + reg) & bits) if (now() - t0 > d) return 0;
    return 1;
}
static unsigned last_int;   /* INT when the last wait gave up on an error bit; 0 for a timeout */
static int wait_int(unsigned mask, unsigned n) {   /* 1 when a bit of `mask` fired with no error, within n ms */
    unsigned long t0 = now(), d = ms(n);
    for (;;) {
        unsigned s = R32(EMMC + INT);
        if (s & 0x8000) { last_int = s; R32(EMMC + INT) = s; return 0; }
        if (s & mask) { R32(EMMC + INT) = s & mask; return 1; }
        if (now() - t0 > d) return 0;
    }
}
static void reset_line(unsigned bit) {   /* SRST_CMD (bit 25) or SRST_DAT (bit 26), one at a time */
    R32(EMMC + CTL1) = (R32(EMMC + CTL1) & 0x00FFFFFFu) | bit;
    wait_clear(CTL1, bit, 100);
}
static void recover(void) { R32(EMMC + INT) = 0xFFFFFFFFu; reset_line(1u << 25); reset_line(1u << 26); }
static unsigned resp[4];
static int cmd(unsigned idx, unsigned arg, unsigned type, unsigned mode) {
    last_int = 0;
    if (!wait_clear(STATE, (type & DAT) || type == R1B ? 3 : 1, 100)) return 0;
    R32(EMMC + INT) = 0xFFFFFFFFu;
    R32(EMMC + ARG) = arg;
    R32(EMMC + CMDTM) = idx << 24 | type | mode;
    if (!wait_int(1, 100) || (type == R1B && !wait_int(2, 500))) { recover(); return 0; }
    for (unsigned i = 0; i < 4; i++) resp[i] = R32(EMMC + RESP + 4 * i);
    return 1;
}
static unsigned base_hz;
static int set_clock(unsigned hz) {   /* the spec 3.0 10-bit divider: base / (2 * N), N = 0 is the base itself */
    unsigned n = base_hz <= hz ? 0 : (base_hz + 2 * hz - 1) / (2 * hz);
    if (n > 0x3FF) n = 0x3FF;
    if (!wait_clear(STATE, 3, 100)) return 0;
    R32(EMMC + CTL1) = R32(EMMC + CTL1) & ~4u;                              /* SD clock off while it changes */
    R32(EMMC + CTL1) = 1 | (n & 0xFF) << 8 | (n >> 8) << 6 | 0xEu << 16;    /* internal clock on, the divider, data timeout 2^27 */
    unsigned long t0 = now(), d = ms(100);
    while (!(R32(EMMC + CTL1) & 2)) if (now() - t0 > d) return 0;           /* internal clock stable */
    R32(EMMC + CTL1) = R32(EMMC + CTL1) | 4;                                /* SD clock on */
    mdelay(2);
    return 1;
}
static unsigned rca, ready, sdhc;
static int fail(const char *step) {
    kputs("sd FAIL "); kputs(step);
    if (last_int) { kputs(" int "); kx(last_int); }
    kputs("\n");
    return 0;
}
int emmc_init(void) {
    ready = 0; blocks = 0;
    R32(EMMC + CTL0) = 0xF00;                    /* bus power on at 3.3 V before the reset, as Circle does on the Pi 4 */
    mdelay(3);
    R32(EMMC + CTL1) = 1u << 24;                 /* reset the whole host, clocks off */
    if (!wait_clear(CTL1, 7u << 24, 100)) return fail("reset");
    R32(EMMC + CTL0) = 0xF00;                    /* the reset took the power off: on again, 1-bit bus, default speed */
    R32(EMMC + CTL2) = 0;
    base_hz = mbox_clock_rate(12);               /* the firmware's EMMC2 clock (id 12) */
    if (!base_hz) base_hz = 250000000;           /* no answer: assume a fast base, so the card runs slow rather than too fast */
    if (!set_clock(400000)) return fail("clock");
    R32(EMMC + INTEN) = 0;                       /* polled: nothing goes to the interrupt controller */
    R32(EMMC + INTMASK) = 0xFFFFFFFFu;           /* every status bit latches in INT */
    R32(EMMC + INT) = 0xFFFFFFFFu;
    if (!cmd(0, 0, RNONE, 0)) return fail("cmd0");
    int v2 = cmd(8, 0x1AA, R1, 0);               /* SEND_IF_COND, 2.7 to 3.6 V, pattern 0xAA; no answer means a version 1 card */
    if (v2 && (resp[0] & 0xFFF) != 0x1AA) return fail("cmd8");
    unsigned long t0 = now(), d = ms(1000);
    for (unsigned tries = 0;; tries++) {         /* ACMD41 until the card says it has powered up (bit 31) */
        if (!cmd(55, 0, R1, 0) || !cmd(41, 0x00FF8000u | (v2 ? 1u << 30 : 0), R3, 0)) {
            if (!tries) { kputs("sd no card\n"); return 0; }   /* nothing answered at all: no card in the slot */
            return fail("acmd41");
        }
        if (resp[0] >> 31) break;
        if (now() - t0 > d) return fail("acmd41 busy");
        mdelay(10);
    }
    sdhc = v2 && (resp[0] >> 30 & 1);
    if (!cmd(2, 0, R2, 0)) return fail("cmd2");
    if (!cmd(3, 0, R1, 0)) return fail("cmd3");
    rca = resp[0] >> 16;
    if (!cmd(9, rca << 16, R2, 0)) return fail("cmd9");
    if ((resp[3] >> 22 & 3) == 1) blocks = ((unsigned long)(resp[1] >> 8 & 0x3FFFFF) + 1) * 1024;   /* CSD 2.0: (C_SIZE + 1) x 512 KB */
    else {                                                                                       /* CSD 1.0 */
        unsigned c = resp[1] >> 22 | (resp[2] & 3) << 10, m = resp[1] >> 7 & 7, bl = resp[2] >> 8 & 15;
        blocks = (unsigned long)(c + 1) << (m + 2) << bl >> 9;
    }
    if (!cmd(7, rca << 16, R1B, 0)) return fail("cmd7");
    if (!cmd(16, 512, R1, 0)) return fail("cmd16");
    if (!set_clock(25000000)) return fail("clock 25 MHz");
    ready = 1;
    kputs("sd card "); kputs(sdhc ? "sdhc " : "sdsc "); kdec((unsigned)(blocks / 2048));
    kputs(" MB, base clock "); kdec(base_hz / 1000000); kputs(" MHz\n");
    return 1;
}
unsigned long emmc_blocks(void) { return blocks; }
int emmc_read(unsigned lba, unsigned count, void *buf) {
    unsigned char *p = buf;
    if (!ready || !count || count > 0xFFFF) return 0;
    if (lba >= blocks || count > blocks - lba) { kputs("sd refused block "); kdec(lba); kputs(", past the end of the card\n"); return 0; }
    R32(EMMC + BLK) = count << 16 | 512;
    if (!cmd(count > 1 ? 18 : 17, sdhc ? lba : lba * 512, R1 | DAT, TM_READ | (count > 1 ? TM_MULTI : 0))) goto bad;
    for (unsigned b = 0; b < count; b++, p += 512) {
        if (!wait_int(0x20, 200)) goto bad;      /* read ready: one block sits in the buffer */
        for (unsigned i = 0; i < 512; i += 4) {   /* byte stores: the caller's buffer may be unaligned */
            unsigned w = R32(EMMC + DATA);
            p[i] = (unsigned char)w; p[i + 1] = (unsigned char)(w >> 8); p[i + 2] = (unsigned char)(w >> 16); p[i + 3] = (unsigned char)(w >> 24);
        }
    }
    if (wait_int(2, 200)) return 1;              /* transfer done, after the Auto CMD12 on a CMD18 */
bad:
    recover();
    kputs("sd FAIL read block "); kdec(lba);
    if (last_int) { kputs(" int "); kx(last_int); }
    kputs("\n");
    return 0;
}
int emmc_write(unsigned lba, unsigned count, const void *buf) {
    const unsigned char *p = buf;
    if (!ready || !count || count > 0xFFFF) return 0;
    if (lba >= blocks || count > blocks - lba) { kputs("sd refused write to block "); kdec(lba); kputs(", past the end of the card\n"); return 0; }
    R32(EMMC + BLK) = count << 16 | 512;
    if (!cmd(count > 1 ? 25 : 24, sdhc ? lba : lba * 512, R1 | DAT, count > 1 ? TM_MULTI : 0)) goto bad;
    for (unsigned b = 0; b < count; b++, p += 512) {
        if (!wait_int(0x10, 500)) goto bad;      /* write ready: room in the buffer for one block */
        for (unsigned i = 0; i < 512; i += 4)     /* byte loads: the caller's buffer may be unaligned */
            R32(EMMC + DATA) = p[i] | p[i + 1] << 8 | p[i + 2] << 16 | (unsigned)p[i + 3] << 24;
    }
    if (!wait_int(2, 1000)) goto bad;            /* transfer done: the host saw the card let go of DAT0 (and sent the CMD12 on a CMD25) */
    unsigned long t0 = now(), d = ms(1000);
    for (;;) {                                   /* CMD13 until the card is back in the transfer state, done programming */
        if (!cmd(13, rca << 16, R1, 0)) goto bad;
        if (resp[0] & 0xFDF80000u) { last_int = 0; kputs("sd FAIL write status "); kx(resp[0]); kputs("\n"); return 0; }
        if ((resp[0] >> 9 & 15) == 4 && resp[0] & 0x100) return 1;
        if (now() - t0 > d) goto bad;
    }
bad:
    recover();
    kputs("sd FAIL write block "); kdec(lba);
    if (last_int) { kputs(" int "); kx(last_int); }
    kputs("\n");
    return 0;
}

#else
/* M3d: QEMU's virt machine has no SD host. Its card is the virtio disk (main.c's blk_*), with the same MBR and FAT
   partition a Pi card has, so everything from here down (the partition bounds, the boot files never written, JT/) is
   the code the Pi runs. With no disk every call answers 0 and nothing is printed. */
int vblk_ready(void); unsigned long vblk_sectors(void); int vblk_rw(unsigned long sector, void *buf, int write);   /* main.c */
int emmc_init(void) {
    blocks = vblk_ready() ? vblk_sectors() : 0;
    if (!blocks) return 0;
    kputs("sd virtio disk "); kdec((unsigned)(blocks / 2048)); kputs(" MB\n");
    return 1;
}
unsigned long emmc_blocks(void) { return blocks; }
static int vblk_span(unsigned lba, unsigned count, unsigned char *p, int write) {
    if (!blocks || !count) return 0;
    if (lba >= blocks || count > blocks - lba) { kputs("sd refused block "); kdec(lba); kputs(", past the end of the card\n"); return 0; }
    for (unsigned b = 0; b < count; b++)
        if (!vblk_rw(lba + b, p + 512UL * b, write)) { kputs(write ? "sd FAIL write block " : "sd FAIL read block "); kdec(lba + b); kputs("\n"); return 0; }
    return 1;
}
int emmc_read(unsigned lba, unsigned count, void *buf) { return vblk_span(lba, count, buf, 0); }
int emmc_write(unsigned lba, unsigned count, const void *buf) { return vblk_span(lba, count, (unsigned char *)buf, 1); }
#endif

/* ---- The FAT partition, as drivers/fat.c's block device ---- */
static unsigned part_start, part_blocks;   /* in card blocks */
static unsigned part_reserved, part_fsinfo;   /* the boot sectors fat.c never writes, and the one it may (FAT32's FSInfo) */
static int mounted;
int blockdev_read_sector(unsigned int lba, void *buf) {   /* fat.c's only way to the card: sector numbers inside the partition */
    if (lba >= part_blocks) { kputs("sd refused block "); kdec(lba); kputs(", past the end of the partition\n"); return 0; }
    return emmc_read(part_start + lba, 1, buf);
}
int blockdev_write_sector(unsigned int lba, const void *buf) {   /* the same bound, and never the boot sector or its backup */
    if (lba >= part_blocks || (lba < part_reserved && lba != part_fsinfo)) {
        kputs("sd refused write to block "); kdec(lba); kputs(lba >= part_blocks ? ", past the end of the partition\n" : ", a boot sector\n");
        return 0;
    }
    return emmc_write(part_start + lba, 1, buf);
}
static unsigned rd32(const unsigned char *p) { return p[0] | p[1] << 8 | p[2] << 16 | (unsigned)p[3] << 24; }
int sd_mount(void) {
    static unsigned char b[1024];
    mounted = 0; part_blocks = 0; part_reserved = 0xFFFFFFFFu;
    if (!emmc_init() || !emmc_read(0, 1, b)) return 0;
    if (b[510] != 0x55 || b[511] != 0xAA) { kputs("sd no partition table\n"); return 0; }
    unsigned start = 0, size = 0;
    for (unsigned i = 0; i < 4 && !size; i++) {   /* the first FAT partition: 0x0B and 0x0C are FAT32, 0x04, 0x06 and 0x0E FAT16 */
        const unsigned char *e = b + 446 + 16 * i;
        if (e[4] == 0x0B || e[4] == 0x0C || e[4] == 0x04 || e[4] == 0x06 || e[4] == 0x0E) { start = rd32(e + 8); size = rd32(e + 12); }
    }
    if (!size) { kputs("sd no FAT partition\n"); return 0; }
    if (start >= blocks || size > blocks - start) { kputs("sd FAIL partition runs past the card\n"); return 0; }
    if (!emmc_read(start, 2, b)) return 0;        /* its boot sector and the block after it in one CMD18 */
    if (b[510] != 0x55 || b[511] != 0xAA) { kputs("sd FAIL boot sector\n"); return 0; }
    int f32 = !(b[22] | b[23]);
    if (f32 && b[48] == 1 && !b[49] && rd32(b + 512) != 0x41615252) { kputs("sd FAIL second block of a CMD18 (no FSInfo)\n"); return 0; }
    part_start = start; part_blocks = size;
    part_reserved = b[14] | b[15] << 8;
    part_fsinfo = f32 ? (unsigned)(b[48] | b[49] << 8) : 0xFFFFFFFFu;
    if (!part_fsinfo) part_fsinfo = 0xFFFFFFFFu;  /* sector 0 is the boot sector, never FSInfo */
    if (!fat_mount()) { part_blocks = 0; kputs("sd FAIL fat mount\n"); return 0; }
    mounted = 1;
    kputs("sd "); kputs(f32 ? "fat32" : "fat16"); kputs(" partition at block "); kdec(start);
    kputs(", "); kdec(size / 2048); kputs(" MB, writes go in JT/\n");
    return 1;
}
int sd_ready(void) { return mounted; }

/* ---- The OS's own files: JT/NAME, never a file the firmware needs ---- */
static int lc(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }
static int match(const char *p, const char *s) {   /* p may hold one '*', any run of characters; case does not matter */
    for (; *p && *p != '*'; p++, s++) if (lc(*p) != lc(*s)) return 0;
    if (!*p) return !*s;
    unsigned np = 0, ns = 0;
    while (p[1 + np]) np++;
    while (s[ns]) ns++;
    return ns >= np && match(p + 1, s + ns - np);
}
static const char *const boot_files[] = { "bootcode.bin", "start*.elf", "fixup*.dat", "*.dtb", "config.txt", "cmdline.txt",
                                          "kernel*.img", "armstub*", "overlays" };
static int sd_name_ok(const char *name) {   /* 1 for an 8.3 name that is not a boot file; else one `sd refused` line */
    unsigned base = 0, ext = 0, dot = 0;
    for (const char *c = name; *c; c++) {
        int ok = (*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z') || (*c >= '0' && *c <= '9') || *c == '_' || *c == '-' || *c == '~';
        if (*c == '.' && !dot) dot = 1;
        else if (!ok) { base = 99; break; }
        else if (dot) ext++;
        else base++;
    }
    if (!base || base > 8 || ext > 3 || (dot && !ext)) { kputs("sd refused "); kputs(name); kputs(", not an 8.3 name\n"); return 0; }
    for (unsigned i = 0; i < sizeof boot_files / sizeof boot_files[0]; i++)
        if (match(boot_files[i], name)) { kputs("sd refused "); kputs(name); kputs(", the firmware boots from it\n"); return 0; }
    return 1;
}
static int jt_open(int make) {   /* into JT/, made on the first write; fat.c works on its current directory */
    if (fat_chdir("JT")) return 1;
    return make && fat_mkdir("JT") && fat_chdir("JT");
}
int sd_enter_jt(int make) { return mounted && jt_open(make); }
int sd_write(const char *name, const void *data, unsigned len) {
    if (!mounted) return 0;
    if (!sd_name_ok(name)) return -1;
    if (!jt_open(1)) return 0;
    int ok = fat_replace_file(name, data, len);   /* creates it too */
    fat_chdir("..");
    return ok;
}
int sd_remove(const char *name) {
    if (!mounted) return 0;
    if (!sd_name_ok(name)) return -1;
    if (!jt_open(0)) return 0;
    int ok = fat_delete(name);
    fat_chdir("..");
    return ok;
}
int sd_mkdir(const char *name) {
    if (!mounted) return 0;
    if (!sd_name_ok(name)) return -1;
    if (!jt_open(1)) return 0;
    int ok = fat_mkdir(name);
    fat_chdir("..");
    return ok;
}
