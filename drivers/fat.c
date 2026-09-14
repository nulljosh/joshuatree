/* FAT16 over ata.c. ponytail: FAT16 only (no FAT32 -- QEMU test images and
   anything this kernel writes itself will be small enough that FAT16 is
   plenty), single fixed volume starting at sector 0 (no MBR partition
   table yet), no long filenames (8.3 exactly as FAT stores them). */
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

static u32 fat_start, root_dir_start, root_dir_sectors, data_start, total_clusters;
static int mounted = 0;

/* 0 means root (the fixed pre-data-area directory FAT16 has no cluster
   number for); any other value is the first cluster of a real subdirectory,
   same convention "." and ".." entries use. */
static u16 current_dir_cluster = 0;

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

    u32 total_sectors_16 = rd16(&boot[19]);
    u32 total_sectors_32 = (u32)boot[32] | ((u32)boot[33] << 8) | ((u32)boot[34] << 16) | ((u32)boot[35] << 24);
    u32 total_sectors = total_sectors_16 ? total_sectors_16 : total_sectors_32;
    total_clusters = total_sectors > data_start ? (total_sectors - data_start) / sectors_per_cluster : 0;

    current_dir_cluster = 0;
    mounted = 1;
    return 1;
}

static u32 cluster_to_lba(u16 cluster) {
    return data_start + ((u32)cluster - 2) * sectors_per_cluster;
}

/* FAT[cluster]: the next cluster in the chain, or an end-of-chain/free
   marker. Shared by file-data reads, directory-cluster-chain walks, and
   free-cluster search, the three things that all need to ask "what comes
   after this cluster". */
static u16 fat_entry_read(u16 cluster) {
    u32 fat_byte_off = (u32)cluster * 2;
    u32 fat_sector = fat_start + fat_byte_off / 512;
    u8 fatbuf[512];
    if (!ata_read_sector(fat_sector, fatbuf)) return 0xFFFF;
    return rd16(&fatbuf[fat_byte_off % 512]);
}

static int fat_entry_write(u16 cluster, u16 value) {
    u32 fat_byte_off = (u32)cluster * 2;
    u32 row = fat_byte_off / 512;
    u8 fatbuf[512];
    if (!ata_read_sector(fat_start + row, fatbuf)) return 0;
    fatbuf[fat_byte_off % 512]     = (u8)(value & 0xFF);
    fatbuf[fat_byte_off % 512 + 1] = (u8)(value >> 8);
    for (u8 f = 0; f < num_fats; f++) {
        if (!ata_write_sector(fat_start + (u32)f * fat_size_sectors + row, fatbuf)) return 0;
    }
    return 1;
}

static u16 alloc_cluster(void) {
    for (u32 c = 2; c < total_clusters + 2; c++) {
        if (fat_entry_read((u16)c) == 0) {
            if (!fat_entry_write((u16)c, 0xFFFF)) return 0;
            return (u16)c;
        }
    }
    return 0;
}

/* The lba of the index'th sector of a directory, root (fixed area, can't
   grow) or a real subdirectory (cluster chain, walked via the FAT). 0 means
   past the end: root is simply full, a subdirectory's chain ran out. */
static u32 dir_get_sector(u16 dir_cluster, u32 index) {
    if (dir_cluster == 0) {
        if (index >= root_dir_sectors) return 0;
        return root_dir_start + index;
    }
    u16 cluster = dir_cluster;
    for (u32 skip = index / sectors_per_cluster; skip > 0; skip--) {
        cluster = fat_entry_read(cluster);
        if (cluster < 2 || cluster >= 0xFFF8) return 0;
    }
    return cluster_to_lba(cluster) + (index % sectors_per_cluster);
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

static u32 matched_lba;
static int matched_index;
static struct dir_entry *find_entry_in(u16 dir_cluster, const char *name) {
    static u8 sector[512];
    static struct dir_entry match;
    u8 want[11];
    to_fat_name(name, want);

    for (u32 s = 0; ; s++) {
        u32 lba = dir_get_sector(dir_cluster, s);
        if (!lba) return 0;
        if (!ata_read_sector(lba, sector)) return 0;
        struct dir_entry *entries = (struct dir_entry *)sector;
        for (int i = 0; i < 512 / 32; i++) {
            if (entries[i].name[0] == 0x00) return 0;       /* end of directory */
            if (entries[i].name[0] == 0xE5) continue;         /* deleted */
            if (entries[i].attr & ATTR_VOLUME_ID) continue;
            if (names_eq(entries[i].name, want)) { matched_lba = lba; matched_index = i; match = entries[i]; return &match; }
        }
    }
}

static int find_free_slot(u16 dir_cluster, u32 *out_lba, int *out_index) {
    u8 sector[512];
    for (u32 s = 0; ; s++) {
        u32 lba = dir_get_sector(dir_cluster, s);
        if (!lba) return 0; /* root exhausted, or subdirectory needs another cluster (not done yet) */
        if (!ata_read_sector(lba, sector)) return 0;
        struct dir_entry *entries = (struct dir_entry *)sector;
        for (int i = 0; i < 512 / 32; i++) {
            if (entries[i].name[0] == 0x00 || entries[i].name[0] == 0xE5) {
                *out_lba = lba; *out_index = i;
                return 1;
            }
        }
    }
}

int fat_read_file(const char *name, void *buf, unsigned int bufsize) {
    if (!mounted) return -1;
    struct dir_entry *e = find_entry_in(current_dir_cluster, name);
    if (!e || (e->attr & ATTR_DIRECTORY)) return -1;

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
        cluster = fat_entry_read(cluster);
    }
    return (int)(total - remaining);
}

int fat_delete(const char *name) {
    if (!mounted) return 0;
    u8 want[11];
    to_fat_name(name, want);
    u8 sector[512];

    for (u32 s = 0; ; s++) {
        u32 lba = dir_get_sector(current_dir_cluster, s);
        if (!lba) return 0;
        if (!ata_read_sector(lba, sector)) return 0;
        struct dir_entry *entries = (struct dir_entry *)sector;
        for (int i = 0; i < 512 / 32; i++) {
            if (entries[i].name[0] == 0x00) return 0;
            if (entries[i].name[0] == 0xE5) continue;
            if (entries[i].attr & (ATTR_VOLUME_ID | ATTR_DIRECTORY)) continue;
            if (names_eq(entries[i].name, want)) {
                entries[i].name[0] = 0xE5;
                return ata_write_sector(lba, sector);
            }
        }
    }
}

void fat_list(void (*cb)(const char *name, unsigned int size, int is_dir)) {
    if (!mounted) return;
    u8 sector[512];
    char namebuf[13];

    for (u32 s = 0; ; s++) {
        u32 lba = dir_get_sector(current_dir_cluster, s);
        if (!lba) return;
        if (!ata_read_sector(lba, sector)) return;
        struct dir_entry *entries = (struct dir_entry *)sector;
        for (int i = 0; i < 512 / 32; i++) {
            if (entries[i].name[0] == 0x00) return;
            if (entries[i].name[0] == 0xE5) continue;
            if (entries[i].attr & ATTR_VOLUME_ID) continue;
            if (entries[i].name[0] == '.') continue; /* skip "." and ".." */

            int n = 0;
            for (int c = 0; c < 8 && entries[i].name[c] != ' '; c++) namebuf[n++] = entries[i].name[c];
            if (entries[i].name[8] != ' ') {
                namebuf[n++] = '.';
                for (int c = 8; c < 11 && entries[i].name[c] != ' '; c++) namebuf[n++] = entries[i].name[c];
            }
            namebuf[n] = 0;
            cb(namebuf, entries[i].file_size, (entries[i].attr & ATTR_DIRECTORY) != 0);
        }
    }
}

int fat_chdir(const char *name) {
    if (!mounted) return 0;
    if (!strcmp(name, ".")) return 1;
    if (!strcmp(name, "..")) {
        if (current_dir_cluster == 0) return 1; /* already at root */
        u8 want[11];
        for (int i = 0; i < 11; i++) want[i] = ' ';
        want[0] = '.'; want[1] = '.';
        u8 sector[512];
        for (u32 s = 0; ; s++) {
            u32 lba = dir_get_sector(current_dir_cluster, s);
            if (!lba) return 0;
            if (!ata_read_sector(lba, sector)) return 0;
            struct dir_entry *entries = (struct dir_entry *)sector;
            for (int i = 0; i < 512 / 32; i++) {
                if (names_eq(entries[i].name, want)) { current_dir_cluster = entries[i].first_cluster_low; return 1; }
            }
        }
    }
    struct dir_entry *e = find_entry_in(current_dir_cluster, name);
    if (!e || !(e->attr & ATTR_DIRECTORY)) return 0;
    current_dir_cluster = e->first_cluster_low;
    return 1;
}

int fat_mkdir(const char *name) {
    if (!mounted || !*name) return 0;
    if (find_entry_in(current_dir_cluster, name)) return 0; /* name taken */

    u16 new_cluster = alloc_cluster();
    if (!new_cluster) return 0;

    u8 first[512];
    memset(first, 0, sizeof(first));
    struct dir_entry *entries = (struct dir_entry *)first;
    entries[0].name[0] = '.';
    for (int i = 1; i < 11; i++) entries[0].name[i] = ' ';
    entries[0].attr = ATTR_DIRECTORY;
    entries[0].first_cluster_low = new_cluster;
    entries[1].name[0] = '.'; entries[1].name[1] = '.';
    for (int i = 2; i < 11; i++) entries[1].name[i] = ' ';
    entries[1].attr = ATTR_DIRECTORY;
    entries[1].first_cluster_low = current_dir_cluster; /* 0 (root) or a real parent cluster */

    u32 lba = cluster_to_lba(new_cluster);
    if (!ata_write_sector(lba, first)) return 0;

    u8 blank[512];
    memset(blank, 0, sizeof(blank));
    for (u8 s = 1; s < sectors_per_cluster; s++) {
        if (!ata_write_sector(lba + s, blank)) return 0;
    }

    u32 slot_lba; int slot_idx;
    if (!find_free_slot(current_dir_cluster, &slot_lba, &slot_idx)) return 0; /* directory full, ponytail: no growth yet, see fat.h */

    u8 sector[512];
    if (!ata_read_sector(slot_lba, sector)) return 0;
    struct dir_entry *slot = &((struct dir_entry *)sector)[slot_idx];
    to_fat_name(name, slot->name);
    slot->attr = ATTR_DIRECTORY;
    for (int i = 0; i < 8; i++) slot->reserved[i] = 0;
    slot->first_cluster_high = 0;
    slot->write_time = 0;
    slot->write_date = 0;
    slot->first_cluster_low = new_cluster;
    slot->file_size = 0;
    return ata_write_sector(slot_lba, sector);
}

static void release_chain(u16 cluster) {
    for (u32 count = 0; cluster >= 2 && cluster < 0xFFF8 && count < total_clusters; count++) {
        u16 next = fat_entry_read(cluster);
        if (!fat_entry_write(cluster, 0)) return;
        cluster = next;
    }
}

static int write_file(const char *name, const void *data, unsigned int len, int replace) {
    if (!mounted || !*name) return 0;
    struct dir_entry *existing = find_entry_in(current_dir_cluster, name);
    if (existing && (!replace || (existing->attr & ATTR_DIRECTORY))) return 0;
    u16 old_cluster = existing ? existing->first_cluster_low : 0;
    u32 slot_lba = matched_lba; int slot_idx = matched_index;
    if (!existing && !find_free_slot(current_dir_cluster, &slot_lba, &slot_idx)) return 0;

    const u8 *src = (const u8 *)data;
    u16 first_cluster = 0, prev_cluster = 0;
    u32 remaining = len;

    while (remaining > 0) {
        u16 c = alloc_cluster();
        if (!c) goto failed;
        if (first_cluster == 0) first_cluster = c;
        else if (!fat_entry_write(prev_cluster, c)) { release_chain(c); goto failed; }
        prev_cluster = c;

        u32 lba = cluster_to_lba(c);
        for (u8 s = 0; s < sectors_per_cluster && remaining > 0; s++) {
            u8 buf[512];
            memset(buf, 0, sizeof(buf)); /* zero-pad the tail of the last sector */
            u32 chunk = remaining < 512 ? remaining : 512;
            for (u32 i = 0; i < chunk; i++) buf[i] = src[i];
            if (!ata_write_sector(lba + s, buf)) goto failed;
            src += chunk;
            remaining -= chunk;
        }
    }
    if (prev_cluster && !fat_entry_write(prev_cluster, 0xFFFF)) goto failed;

    u8 sector[512];
    if (!ata_read_sector(slot_lba, sector)) goto failed;
    struct dir_entry *slot = &((struct dir_entry *)sector)[slot_idx];
    to_fat_name(name, slot->name);
    slot->attr = 0;
    for (int i = 0; i < 8; i++) slot->reserved[i] = 0;
    slot->first_cluster_high = 0;
    slot->write_time = 0;
    slot->write_date = 0;
    slot->first_cluster_low = first_cluster;
    slot->file_size = len;
    if (!ata_write_sector(slot_lba, sector)) return 0;
    release_chain(old_cluster);
    return 1;
failed:
    release_chain(first_cluster);
    return 0;
}

int fat_write_file(const char *name, const void *data, unsigned int len) {
    return write_file(name, data, len, 0);
}

int fat_replace_file(const char *name, const void *data, unsigned int len) {
    return write_file(name, data, len, 1);
}
