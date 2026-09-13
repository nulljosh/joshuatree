/* Read-only FAT16 over ata.c. ponytail: root directory only (no subdirs,
   see fat.h), FAT16 only (no FAT32 -- QEMU test images and anything this
   kernel writes itself will be small enough that FAT16 is plenty), single
   fixed volume starting at sector 0 (no MBR partition table yet). */
#include "fat.h"
#include "ata.h"
#include "libc.h"

typedef unsigned int   u32;
typedef unsigned short u16;
typedef unsigned char  u8;

static u16 bytes_per_sector;
static u8  sectors_per_cluster;
static u16 reserved_sectors;
static u8  num_fats;
static u16 root_entry_count;
static u16 fat_size_sectors;

static u32 fat_start, root_dir_start, root_dir_sectors, data_start;
static int mounted = 0;

struct dir_entry {
    u8  name[11];
    u8  attr;
    u8  reserved[8];
    u16 first_cluster_high; /* unused in FAT16 */
    u16 write_time, write_date;
    u16 first_cluster_low;
    u32 file_size;
} __attribute__((packed));

#define ATTR_VOLUME_ID 0x08
#define ATTR_DIRECTORY 0x10

static u16 rd16(u8 *p) { return p[0] | (p[1] << 8); }

int fat_mount(void) {
    u8 boot[512];
    if (!ata_read_sector(0, boot)) return 0;

    bytes_per_sector   = rd16(&boot[11]);
    sectors_per_cluster = boot[13];
    reserved_sectors   = rd16(&boot[14]);
    num_fats           = boot[16];
    root_entry_count   = rd16(&boot[17]);
    fat_size_sectors   = rd16(&boot[22]);

    if (bytes_per_sector != 512 || sectors_per_cluster == 0 || num_fats == 0) return 0;

    fat_start        = reserved_sectors;
    root_dir_start   = fat_start + (u32)num_fats * fat_size_sectors;
    root_dir_sectors = ((u32)root_entry_count * 32 + 511) / 512;
    data_start       = root_dir_start + root_dir_sectors;

    mounted = 1;
    return 1;
}

static u32 cluster_to_lba(u16 cluster) {
    return data_start + ((u32)cluster - 2) * sectors_per_cluster;
}

/* format "TEST.TXT" into FAT's fixed 11-byte "TEST    TXT" layout for comparison */
static void to_fat_name(const char *in, u8 out[11]) {
    for (int i = 0; i < 11; i++) out[i] = ' ';
    int i = 0, j = 0;
    while (in[i] && in[i] != '.' && j < 8) {
        char c = in[i++];
        out[j++] = (c >= 'a' && c <= 'z') ? c - 32 : c;
    }
    while (in[i] && in[i] != '.') i++;
    if (in[i] == '.') {
        i++;
        int k = 8;
        while (in[i] && k < 11) {
            char c = in[i++];
            out[k++] = (c >= 'a' && c <= 'z') ? c - 32 : c;
        }
    }
}

static int names_eq(const u8 a[11], const u8 b[11]) {
    return memcmp(a, b, 11) == 0;
}

static struct dir_entry *find_entry(const char *name) {
    static u8 sector[512];
    static struct dir_entry match;
    u8 want[11];
    to_fat_name(name, want);

    for (u32 s = 0; s < root_dir_sectors; s++) {
        if (!ata_read_sector(root_dir_start + s, sector)) return 0;
        struct dir_entry *entries = (struct dir_entry *)sector;
        for (int i = 0; i < 512 / 32; i++) {
            if (entries[i].name[0] == 0x00) return 0;       /* end of directory */
            if (entries[i].name[0] == 0xE5) continue;         /* deleted */
            if (entries[i].attr & (ATTR_VOLUME_ID | ATTR_DIRECTORY)) continue;
            if (names_eq(entries[i].name, want)) { match = entries[i]; return &match; }
        }
    }
    return 0;
}

int fat_read_file(const char *name, void *buf, unsigned int bufsize) {
    if (!mounted) return -1;
    struct dir_entry *e = find_entry(name);
    if (!e) return -1;

    u32 remaining = e->file_size < bufsize ? e->file_size : bufsize;
    u32 total = remaining;
    u16 cluster = e->first_cluster_low;
    u8 *out = (u8 *)buf;
    u8 sector[512];

    while (remaining > 0 && cluster >= 2 && cluster < 0xFFF8) {
        u32 lba = cluster_to_lba(cluster);
        for (u8 s = 0; s < sectors_per_cluster && remaining > 0; s++) {
            if (!ata_read_sector(lba + s, sector)) return (int)(total - remaining);
            u32 chunk = remaining < 512 ? remaining : 512;
            for (u32 i = 0; i < chunk; i++) out[i] = sector[i];
            out += chunk;
            remaining -= chunk;
        }
        /* follow the FAT16 chain: entry = 2 bytes at fat_start + cluster*2 */
        u32 fat_byte_off = (u32)cluster * 2;
        u32 fat_sector = fat_start + fat_byte_off / 512;
        u8 fatbuf[512];
        if (!ata_read_sector(fat_sector, fatbuf)) break;
        cluster = rd16(&fatbuf[fat_byte_off % 512]);
    }
    return (int)(total - remaining);
}

int fat_delete(const char *name) {
    if (!mounted) return 0;
    u8 want[11];
    to_fat_name(name, want);
    u8 sector[512];

    for (u32 s = 0; s < root_dir_sectors; s++) {
        if (!ata_read_sector(root_dir_start + s, sector)) return 0;
        struct dir_entry *entries = (struct dir_entry *)sector;
        for (int i = 0; i < 512 / 32; i++) {
            if (entries[i].name[0] == 0x00) return 0;
            if (entries[i].name[0] == 0xE5) continue;
            if (entries[i].attr & (ATTR_VOLUME_ID | ATTR_DIRECTORY)) continue;
            if (names_eq(entries[i].name, want)) {
                entries[i].name[0] = 0xE5;
                return ata_write_sector(root_dir_start + s, sector);
            }
        }
    }
    return 0;
}

void fat_list(void (*cb)(const char *name, unsigned int size)) {
    if (!mounted) return;
    u8 sector[512];
    char namebuf[13];

    for (u32 s = 0; s < root_dir_sectors; s++) {
        if (!ata_read_sector(root_dir_start + s, sector)) return;
        struct dir_entry *entries = (struct dir_entry *)sector;
        for (int i = 0; i < 512 / 32; i++) {
            if (entries[i].name[0] == 0x00) return;
            if (entries[i].name[0] == 0xE5) continue;
            if (entries[i].attr & (ATTR_VOLUME_ID | ATTR_DIRECTORY)) continue;

            int n = 0;
            for (int c = 0; c < 8 && entries[i].name[c] != ' '; c++) namebuf[n++] = entries[i].name[c];
            if (entries[i].name[8] != ' ') {
                namebuf[n++] = '.';
                for (int c = 8; c < 11 && entries[i].name[c] != ' '; c++) namebuf[n++] = entries[i].name[c];
            }
            namebuf[n] = 0;
            cb(namebuf, entries[i].file_size);
        }
    }
}
