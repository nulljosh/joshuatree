/* FAT16 over blockdev.c (v33/0.33.0: was ata.c directly, now goes through
   the same real block-device abstraction ata.c and ramdisk.c both
   register with). Single volume starting at blockdev sector 0 (the Pi's
   arch/arm64/emmc.c adds its partition's start), no long filenames (8.3
   exactly as FAT stores them). 2.53.0: FAT32 reads too, for the Pi's boot
   partition: 28-bit FAT entries, the root as a cluster chain, the high
   half of each first cluster. 2.57.0: FAT32 writes as well, so the Pi's
   files survive a reboot: u32 clusters, the top nibble of every FAT32
   entry kept, every FAT copy written, first_cluster_high set, and the
   FSInfo free count marked unknown on the first change. */
#include "fat.h"
#include "blockdev.h"
#include "libc.h"
#include "vfs.h"

typedef unsigned int   u32;
typedef unsigned short u16;
typedef unsigned char  u8;

static u16 bytes_per_sector;
static u8  sectors_per_cluster;
static u16 reserved_sectors;
static u8  num_fats;
static u16 root_entry_count;
static u32 fat_size_sectors;
static int fat32;              /* FAT32: 4-byte FAT entries, and the root is a cluster chain */
static u32 root_cluster, eoc;  /* FAT32's root cluster; the first end-of-chain value (0xFFF8 or 0x0FFFFFF8) */
static u32 eoc_mark;           /* what a chain's last entry gets: 0xFFFF or 0x0FFFFFFF */
static u32 fsinfo_lba;         /* FAT32's FSInfo sector, 0 for none */
static int fsinfo_done;        /* its free count already marked unknown since the mount */
static u32 alloc_hint;         /* where the last free-cluster search stopped; only a starting point */

static u32 fat_start, root_dir_start, root_dir_sectors, data_start, total_clusters;
static int mounted = 0;
static void fat_recover(void);

/* 0 means root (the fixed pre-data-area directory FAT16 has no cluster
   number for; on FAT32 the chain at root_cluster); any other value is the
   first cluster of a real subdirectory, same convention "." and ".."
   entries use. */
static u32 current_dir_cluster = 0;

struct dir_entry {
    u8  name[11];
    u8  attr;
    u8  reserved[8];
    u16 first_cluster_high; /* FAT32 only */
    u16 write_time, write_date;
    u16 first_cluster_low;
    u32 file_size;
} __attribute__((packed));

#define ATTR_VOLUME_ID 0x08
#define ATTR_DIRECTORY 0x10

static u16 rd16(u8 *p) { return p[0] | (p[1] << 8); }
static u32 rd32(u8 *p) { return rd16(p) | ((u32)rd16(p + 2) << 16); }

int fat_mount(void) {
    u8 boot[512];
    if (!blockdev_read_sector(0, boot)) return 0;

    bytes_per_sector   = rd16(&boot[11]);
    sectors_per_cluster = boot[13];
    reserved_sectors   = rd16(&boot[14]);
    num_fats           = boot[16];
    root_entry_count   = rd16(&boot[17]);
    fat_size_sectors   = rd16(&boot[22]);
    fat32 = fat_size_sectors == 0;   /* FAT32 keeps its FAT size and root cluster further on */
    if (fat32) {
        fat_size_sectors = rd32(&boot[36]);
        root_cluster = rd32(&boot[44]) & 0x0FFFFFFF;
        fsinfo_lba = rd16(&boot[48]);
        if (root_entry_count || root_cluster < 2 || fat_size_sectors > (1u << 21)) return 0;   /* 2^28 clusters at most: nothing below overflows */
    }
    eoc = fat32 ? 0x0FFFFFF8 : 0xFFF8;
    eoc_mark = fat32 ? 0x0FFFFFFF : 0xFFFF;
    fsinfo_done = 0; alloc_hint = 0;

    if (bytes_per_sector != 512 || sectors_per_cluster == 0 || num_fats == 0 || fat_size_sectors == 0) return 0;

    fat_start        = reserved_sectors;
    root_dir_start   = fat_start + (u32)num_fats * fat_size_sectors;
    root_dir_sectors = ((u32)root_entry_count * 32 + 511) / 512;
    data_start       = root_dir_start + root_dir_sectors;

    u32 total_sectors_16 = rd16(&boot[19]);
    u32 total_sectors_32 = (u32)boot[32] | ((u32)boot[33] << 8) | ((u32)boot[34] << 16) | ((u32)boot[35] << 24);
    u32 total_sectors = total_sectors_16 ? total_sectors_16 : total_sectors_32;
    total_clusters = total_sectors > data_start ? (total_sectors - data_start) / sectors_per_cluster : 0;

    /* Real bug, found by host-fuzzing a boot sector whose total_sectors_16
       was 0 and total_sectors_32 was 0xFFFFFFFF (both entirely attacker-
       controlled, unrelated to fat_size_sectors, the field that actually
       sizes the on-disk FAT table): total_clusters came out near 4.3
       billion, and alloc_cluster's free-cluster search (`for c=2; c<
       total_clusters+2; c++`) has no other bound, so any write on a
       mounted disk that hostile/corrupted (real risk on its own, not just
       a crafted attack: an interrupted format or a torn write can leave a
       BPB whose total_sectors disagrees with its own FAT size) hung for
       the whole search, a real denial of service (tools/fuzz-host/
       fuzz_parsers.c's "fat_alloc_cluster_dos" case reproduces it host-
       side under a 1-second alarm). A FAT16 table only ever has room to
       describe as many clusters as its own sectors can hold 2-byte
       entries for (minus the 2 reserved low entries), regardless of what
       total_sectors claims; clamping here fixes every consumer of
       total_clusters (alloc_cluster, release_chain) at the one place that
       computes it, not just the write path that happened to find it. */
    u32 max_clusters_from_fat = fat_size_sectors * (512 / (fat32 ? 4 : 2));
    if (max_clusters_from_fat >= 2) max_clusters_from_fat -= 2; else max_clusters_from_fat = 0;
    if (total_clusters > max_clusters_from_fat) total_clusters = max_clusters_from_fat;

    current_dir_cluster = 0;
    mounted = 1;
    fat_recover();
    return 1;
}

static u32 cluster_to_lba(u32 cluster) {
    return data_start + ((u32)cluster - 2) * sectors_per_cluster;
}

/* FAT[cluster]: the next cluster in the chain, or an end-of-chain/free
   marker. Shared by file-data reads, directory-cluster-chain walks, and
   free-cluster search, the three things that all need to ask "what comes
   after this cluster". */
static u32 fat_entry_read(u32 cluster) {
    u32 fat_byte_off = cluster * (fat32 ? 4 : 2);
    u32 fat_sector = fat_start + fat_byte_off / 512;
    u8 fatbuf[512];
    if (!blockdev_read_sector(fat_sector, fatbuf)) return 0xFFFFFFFF;   /* past every end-of-chain value */
    return fat32 ? rd32(&fatbuf[fat_byte_off % 512]) & 0x0FFFFFFF : rd16(&fatbuf[fat_byte_off % 512]);
}

/* FAT32 keeps a free-cluster count and a next-free hint in its FSInfo
   sector. Rather than keep them exact, the first FAT change after a mount
   sets both to 0xFFFFFFFF, "unknown" in the spec, so no OS trusts a stale
   count; fsck and every OS work it out again. */
static void fsinfo_unknown(void) {
    if (!fat32 || fsinfo_done) return;
    fsinfo_done = 1;
    u8 b[512];
    if (fsinfo_lba < 1 || fsinfo_lba >= reserved_sectors || !blockdev_read_sector(fsinfo_lba, b)) return;
    if (rd32(b) != 0x41615252 || rd32(&b[484]) != 0x61417272) return;
    if (rd32(&b[488]) == 0xFFFFFFFF && rd32(&b[492]) == 0xFFFFFFFF) return;
    memset(&b[488], 0xFF, 8);
    blockdev_write_sector(fsinfo_lba, b);
}

/* FAT[cluster] = value in every FAT copy. Refuses a cluster past the
   table, so a hostile chain can never aim a write past the FAT. FAT32
   entries are 28 bits: the top four belong to the volume and are kept. */
static int fat_entry_write(u32 cluster, u32 value) {
    if (cluster < 2 || cluster >= total_clusters + 2) return 0;
    u32 w = fat32 ? 4 : 2, fat_byte_off = cluster * w, row = fat_byte_off / 512;
    u8 fatbuf[512];
    if (!blockdev_read_sector(fat_start + row, fatbuf)) return 0;
    u8 *p = &fatbuf[fat_byte_off % 512];
    if (fat32) value = (value & 0x0FFFFFFF) | (rd32(p) & 0xF0000000);
    for (u32 i = 0; i < w; i++) p[i] = (u8)(value >> (8 * i));
    fsinfo_unknown();
    for (u8 f = 0; f < num_fats; f++) {
        if (!blockdev_write_sector(fat_start + (u32)f * fat_size_sectors + row, fatbuf)) return 0;
    }
    return 1;
}

/* One read per FAT sector, not one per cluster: a Pi's boot partition has
   thousands of used clusters before its first free one. Starts where the
   last search stopped and wraps; stops at the first sector it cannot
   read, so a hostile size never turns into a long search. */
static u32 alloc_cluster(void) {
    u32 w = fat32 ? 4 : 2, row = 0xFFFFFFFF;
    u8 buf[512];
    for (u32 k = 0; k < total_clusters; k++) {
        u32 c = 2 + (alloc_hint + k) % total_clusters, off = c * w;
        if (off / 512 != row) { row = off / 512; if (!blockdev_read_sector(fat_start + row, buf)) return 0; }
        u32 v = fat32 ? rd32(&buf[off % 512]) & 0x0FFFFFFF : rd16(&buf[off % 512]);
        if (v) continue;
        if (!fat_entry_write(c, eoc_mark)) return 0;
        alloc_hint = c - 1;   /* the next search starts at c + 1 */
        return c;
    }
    return 0;
}

static void release_chain(u32 cluster) {
    for (u32 count = 0; cluster >= 2 && cluster < eoc && count < total_clusters; count++) {
        u32 next = fat_entry_read(cluster);
        if (!fat_entry_write(cluster, 0)) return;
        cluster = next;
    }
}

/* Real bug, found by tracing what a corrupted FAT table (the honest result
   of an abnormal shutdown mid-write, not just a hostile input) does to
   every caller of this function: fat_list/find_entry_in/find_free_slot/
   fat_delete/fat_chdir(".. ") all walk a subdirectory by calling this with
   an ever-increasing index until it returns 0, and the only way it used to
   return 0 for a real subdirectory was hitting an invalid/end-of-chain FAT
   marker (<2 or >=0xFFF8). A cluster chain that cycles instead of properly
   terminating (cluster N's FAT entry pointing back to an earlier cluster
   in its own chain, exactly what a write interrupted between "allocate the
   next cluster" and "link the previous one to it" can leave behind) never
   produces that marker, so every one of those five callers spun forever
   re-reading the same clusters, a real denial-of-service hang on `ls`/`cd`/
   any FAT op inside a corrupted subdirectory. MAX_DIR_CLUSTER_HOPS is a
   sanity cap, not a real directory-size limit. This kernel's own fat_mkdir
   never grows a directory past its first cluster (no chain-growth support,
   see its own comment), so every directory this kernel has ever created
   needs zero hops past its own head cluster; the cap only matters for a
   directory some other real FAT16 tool created with a real multi-cluster
   chain, or a corrupted one. dir_get_sector restarts its walk from the
   directory's head on every call (see the callers' own
   `for (u32 s = 0; ; s++)` loops), so total work across a full directory
   scan is quadratic in the cap, not linear, a pre-existing inefficiency
   this fix doesn't take on fixing; measured directly against this kernel's
   own QEMU/ATA path (tools/checks/fatcyclehang-check.sh), a cap of 16 was
   still visibly grinding after 45 real seconds on a cyclic chain, an
   improvement over never finishing at all but not the fast, bounded
   failure this guard is supposed to be. 4 clusters (up to ~250 entries at
   sectors_per_cluster*16 each) is still generous for anything a real
   external tool would put in one directory on this scope of kernel, and
   keeps the quadratic cost small enough that a corrupted/cyclic chain
   fails within a couple of seconds instead of tens. */
#define MAX_DIR_CLUSTER_HOPS 4

/* The lba of the index'th sector of a directory, root (fixed area, can't
   grow) or a real subdirectory (cluster chain, walked via the FAT). 0 means
   past the end: root is simply full, a subdirectory's chain ran out (or,
   per the guard above, looped instead of ending). A FAT32 root is a chain
   like any subdirectory, under the same cap. */
static u32 dir_get_sector(u32 dir_cluster, u32 index) {
    if (dir_cluster == 0 && fat32) dir_cluster = root_cluster;
    if (dir_cluster == 0) {
        if (index >= root_dir_sectors) return 0;
        return root_dir_start + index;
    }
    u32 skip = index / sectors_per_cluster;
    if (skip >= MAX_DIR_CLUSTER_HOPS) return 0;
    u32 cluster = dir_cluster;
    for (; skip > 0; skip--) {
        cluster = fat_entry_read(cluster);
        if (cluster < 2 || cluster >= eoc) return 0;
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

static u32 entry_cluster(const struct dir_entry *e) {
    return e->first_cluster_low | (fat32 ? (u32)e->first_cluster_high << 16 : 0);
}

static void set_cluster(struct dir_entry *e, u32 c) {   /* FAT16 has no high half: it stays 0 there */
    e->first_cluster_low = (u16)c;
    e->first_cluster_high = fat32 ? (u16)(c >> 16) : 0;
}

static u32 matched_lba;
static int matched_index;
static struct dir_entry *find_entry_in(u32 dir_cluster, const char *name) {
    static u8 sector[512];
    static struct dir_entry match;
    u8 want[11];
    to_fat_name(name, want);

    for (u32 s = 0; ; s++) {
        u32 lba = dir_get_sector(dir_cluster, s);
        if (!lba) return 0;
        if (!blockdev_read_sector(lba, sector)) return 0;
        struct dir_entry *entries = (struct dir_entry *)sector;
        for (int i = 0; i < 512 / 32; i++) {
            if (entries[i].name[0] == 0x00) return 0;       /* end of directory */
            if (entries[i].name[0] == 0xE5) continue;         /* deleted */
            if (entries[i].attr & ATTR_VOLUME_ID) continue;
            if (names_eq(entries[i].name, want)) { matched_lba = lba; matched_index = i; match = entries[i]; return &match; }
        }
    }
}

static int find_free_slot(u32 dir_cluster, u32 *out_lba, int *out_index) {
    u8 sector[512];
    for (u32 s = 0; ; s++) {
        u32 lba = dir_get_sector(dir_cluster, s);
        if (!lba) return 0; /* root exhausted, or subdirectory needs another cluster (not done yet) */
        if (!blockdev_read_sector(lba, sector)) return 0;
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
    u32 cluster = entry_cluster(e);
    u8 *out = (u8 *)buf;
    u8 sector[512];

    while (remaining > 0 && cluster >= 2 && cluster < eoc) {
        u32 lba = cluster_to_lba(cluster);
        for (u8 s = 0; s < sectors_per_cluster && remaining > 0; s++) {
            if (!blockdev_read_sector(lba + s, sector)) return (int)(total - remaining);
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
        if (!blockdev_read_sector(lba, sector)) return 0;
        struct dir_entry *entries = (struct dir_entry *)sector;
        for (int i = 0; i < 512 / 32; i++) {
            if (entries[i].name[0] == 0x00) return 0;
            if (entries[i].name[0] == 0xE5) continue;
            if (entries[i].attr & (ATTR_VOLUME_ID | ATTR_DIRECTORY)) continue;
            if (names_eq(entries[i].name, want)) {
                u32 first = entry_cluster(&entries[i]);
                entries[i].name[0] = 0xE5;
                if (!blockdev_write_sector(lba, sector)) return 0;
                release_chain(first);   /* after the entry is gone: a stop in between loses clusters, never links one twice */
                return 1;
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
        if (!blockdev_read_sector(lba, sector)) return;
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
            if (!blockdev_read_sector(lba, sector)) return 0;
            struct dir_entry *entries = (struct dir_entry *)sector;
            for (int i = 0; i < 512 / 32; i++) {
                if (names_eq(entries[i].name, want)) { current_dir_cluster = entry_cluster(&entries[i]); return 1; }
            }
        }
    }
    struct dir_entry *e = find_entry_in(current_dir_cluster, name);
    if (!e || !(e->attr & ATTR_DIRECTORY)) return 0;
    current_dir_cluster = entry_cluster(e);
    return 1;
}

int fat_mkdir(const char *name) {
    if (!mounted || !*name) return 0;
    if (find_entry_in(current_dir_cluster, name)) return 0; /* name taken */
    u32 slot_lba; int slot_idx;   /* the slot first, so a full directory never costs a cluster */
    if (!find_free_slot(current_dir_cluster, &slot_lba, &slot_idx)) return 0; /* directory full, ponytail: no growth yet, see fat.h */

    u32 new_cluster = alloc_cluster();
    if (!new_cluster) return 0;

    u8 first[512];
    memset(first, 0, sizeof(first));
    struct dir_entry *entries = (struct dir_entry *)first;
    entries[0].name[0] = '.';
    for (int i = 1; i < 11; i++) entries[0].name[i] = ' ';
    entries[0].attr = ATTR_DIRECTORY;
    set_cluster(&entries[0], new_cluster);
    entries[1].name[0] = '.'; entries[1].name[1] = '.';
    for (int i = 2; i < 11; i++) entries[1].name[i] = ' ';
    entries[1].attr = ATTR_DIRECTORY;
    set_cluster(&entries[1], current_dir_cluster); /* 0 (root, on FAT32 too) or a real parent cluster */

    u32 lba = cluster_to_lba(new_cluster);
    u8 blank[512];
    memset(blank, 0, sizeof(blank));
    if (!blockdev_write_sector(lba, first)) goto failed;
    for (u8 s = 1; s < sectors_per_cluster; s++) {
        if (!blockdev_write_sector(lba + s, blank)) goto failed;
    }

    u8 sector[512];
    if (!blockdev_read_sector(slot_lba, sector)) goto failed;
    struct dir_entry *slot = &((struct dir_entry *)sector)[slot_idx];
    to_fat_name(name, slot->name);
    slot->attr = ATTR_DIRECTORY;
    for (int i = 0; i < 8; i++) slot->reserved[i] = 0;
    slot->write_time = 0;
    slot->write_date = 0;
    set_cluster(slot, new_cluster);
    slot->file_size = 0;
    if (blockdev_write_sector(slot_lba, sector)) return 1;
failed:
    release_chain(new_cluster);
    return 0;
}

/* Removes the named subdirectory of the current directory, only when it holds
   nothing but "." and "..". Same order as fat_delete: entry first, then the
   clusters. */
int fat_rmdir(const char *name) {
    if (!mounted || !*name) return 0;
    struct dir_entry *e = find_entry_in(current_dir_cluster, name);
    if (!e || !(e->attr & ATTR_DIRECTORY)) return 0;
    u32 first = entry_cluster(e);
    u32 elba = matched_lba; int eidx = matched_index;
    if (first < 2) return 0;
    u8 sector[512];
    for (u32 s = 0; ; s++) {
        u32 lba = dir_get_sector(first, s);
        if (!lba) break;
        if (!blockdev_read_sector(lba, sector)) return 0;
        struct dir_entry *d = (struct dir_entry *)sector;
        for (int i = 0; i < 512 / 32; i++) {
            if (d[i].name[0] == 0x00) goto empty;
            if (d[i].name[0] == 0xE5 || d[i].name[0] == '.') continue;
            return 0;
        }
    }
empty:
    if (!blockdev_read_sector(elba, sector)) return 0;
    ((struct dir_entry *)sector)[eidx].name[0] = 0xE5;
    if (!blockdev_write_sector(elba, sector)) return 0;
    release_chain(first);
    return 1;
}

static int write_file(const char *name, const void *data, unsigned int len, int replace) {
    if (!mounted || !*name) return 0;
    struct dir_entry *existing = find_entry_in(current_dir_cluster, name);
    if (existing && (!replace || (existing->attr & ATTR_DIRECTORY))) return 0;
    u32 old_cluster = existing ? entry_cluster(existing) : 0;
    u32 slot_lba = matched_lba; int slot_idx = matched_index;
    if (!existing && !find_free_slot(current_dir_cluster, &slot_lba, &slot_idx)) return 0;

    const u8 *src = (const u8 *)data;
    u32 first_cluster = 0, prev_cluster = 0;
    u32 remaining = len;

    while (remaining > 0) {
        u32 c = alloc_cluster();
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
            if (!blockdev_write_sector(lba + s, buf)) goto failed;
            src += chunk;
            remaining -= chunk;
        }
    }
    if (prev_cluster && !fat_entry_write(prev_cluster, eoc_mark)) goto failed;

    u8 sector[512];
    if (!blockdev_read_sector(slot_lba, sector)) goto failed;
    struct dir_entry *slot = &((struct dir_entry *)sector)[slot_idx];
    to_fat_name(name, slot->name);
    slot->attr = 0;
    for (int i = 0; i < 8; i++) slot->reserved[i] = 0;
    slot->write_time = 0;
    slot->write_date = 0;
    set_cluster(slot, first_cluster);
    slot->file_size = len;
    if (!blockdev_write_sector(slot_lba, sector)) return 0;
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


/* Crash safety (4.13.0). The write order does most of it: a replaced file's new data goes to fresh clusters and their FAT
   chain first, then ONE directory sector write flips the entry (sector writes are the atomic unit), and only then are the
   old clusters freed; delete, rmdir and mkdir keep the same shape (entry flipped in one sector, clusters after or before).
   So a power cut leaves either the old file or the new one, and at worst clusters marked used that no entry reaches.
   fat_recover runs at mount and puts that right: it marks every cluster reachable from the root, copies FAT 1 over any
   other FAT copy that differs (the first copy is always written first), frees what is used but unreachable, and writes
   FSInfo's free count exact. It gives up without changing anything if the tree cannot be read whole
   (a read error, a loop, a chain off the table, folders nested past SCAN_DEPTH) or the volume has more clusters than SCAN_MAX_CLUSTERS. */
#ifdef __aarch64__
#define SCAN_MAX_CLUSTERS (1u << 20)   /* a 32 GB card at 32 KB clusters */
#else
#define SCAN_MAX_CLUSTERS (1u << 17)   /* i386 has little BSS room before its user image; a FAT16 table holds 65K clusters */
#endif
#define SCAN_DEPTH 12
static u8 scan_live[SCAN_MAX_CLUSTERS / 8];
static u8 scan_sec[SCAN_DEPTH][512];
static int scan_bad;
static u32 scan_fat_lba;   /* the FAT sector scan_next holds; nothing writes the FAT until the scan is over */
static u8 scan_fat[512];

/* fat_entry_read with a one-sector cache: a chain's entries sit side by side, so the walk reads a FAT sector once, not once a cluster */
static u32 scan_next(u32 c) {
    u32 off = c * (fat32 ? 4 : 2), lba = fat_start + off / 512;
    if (lba != scan_fat_lba) { if (!blockdev_read_sector(lba, scan_fat)) return 0xFFFFFFFF; scan_fat_lba = lba; }
    return fat32 ? rd32(&scan_fat[off % 512]) & 0x0FFFFFFF : rd16(&scan_fat[off % 512]);
}

static int scan_dir_sector(u8 *sec, int depth);

static void scan_chain(u32 c, int dir, int depth) {
    int end = 0;   /* a folder's 0x00 entry ends its listing; the rest of its chain is still the folder's, so still walked */
    for (u32 n = 0; c >= 2 && c < eoc && !scan_bad; n++) {
        if (c >= total_clusters + 2 || n > total_clusters) { scan_bad = 1; return; }
        u32 bit = c - 2;
        if (scan_live[bit / 8] & (1u << (bit % 8))) return;   /* already seen: a shared or looping chain ends here */
        scan_live[bit / 8] |= (u8)(1u << (bit % 8));
        if (dir) {
            if (depth >= SCAN_DEPTH) { scan_bad = 1; return; }
            for (u8 s = 0; s < sectors_per_cluster && !scan_bad && !end; s++) {
                if (!blockdev_read_sector(cluster_to_lba(c) + s, scan_sec[depth])) { scan_bad = 1; return; }
                end = scan_dir_sector(scan_sec[depth], depth);
            }
        }
        u32 next = scan_next(c);
        if (next == 0xFFFFFFFF) { scan_bad = 1; return; }
        c = next;
    }
}

static int scan_dir_sector(u8 *sec, int depth) {   /* 1 at the folder's 0x00 end marker, as find_entry_in and fat_list stop there */
    struct dir_entry *d = (struct dir_entry *)sec;
    for (int i = 0; i < 512 / 32 && !scan_bad; i++) {
        if (d[i].name[0] == 0x00) return 1;
        if (d[i].name[0] == 0xE5 || !memcmp(d[i].name, ".          ", 11) || !memcmp(d[i].name, "..         ", 11) || (d[i].attr & ATTR_VOLUME_ID)) continue;
        u32 c = entry_cluster(&d[i]);
        if (c >= 2) scan_chain(c, (d[i].attr & ATTR_DIRECTORY) != 0, depth + 1);
    }
    return 0;
}

static void fat_recover(void) {
    if (total_clusters == 0 || total_clusters > SCAN_MAX_CLUSTERS) return;
    memset(scan_live, 0, (total_clusters + 7) / 8);
    scan_bad = 0; scan_fat_lba = 0;   /* sector 0 is the boot sector, never a FAT sector */
    if (fat32) scan_chain(root_cluster, 1, 0);
    else for (u32 s = 0; s < root_dir_sectors && !scan_bad; s++) {
        if (!blockdev_read_sector(root_dir_start + s, scan_sec[0])) { scan_bad = 1; break; }
        if (scan_dir_sector(scan_sec[0], 0)) break;
    }
    if (scan_bad) return;

    u32 w = fat32 ? 4 : 2, bad_mark = fat32 ? 0x0FFFFFF7 : 0xFFF7, nfree = 0;
    u32 rows = ((total_clusters + 2) * w + 511) / 512;
    u8 a[512], b[512];
    for (u32 row = 0; row < rows; row++) {
        if (!blockdev_read_sector(fat_start + row, a)) return;
        int dirty = 0;
        for (u8 f = 1; f < num_fats; f++) {
            if (!blockdev_read_sector(fat_start + (u32)f * fat_size_sectors + row, b)) return;
            if (memcmp(a, b, 512)) dirty = 1;
        }
        for (u32 k = 0; k < 512 / w; k++) {
            u32 c = row * (512 / w) + k;
            if (c < 2 || c >= total_clusters + 2) continue;
            u32 v = fat32 ? rd32(&a[k * 4]) & 0x0FFFFFFF : rd16(&a[k * 2]);
            u32 bit = c - 2;
            if (v && v != bad_mark && !(scan_live[bit / 8] & (1u << (bit % 8)))) {
                for (u32 i = 0; i < w; i++) if (i < 3 || !fat32) a[k * w + i] = 0;
                if (fat32) a[k * 4 + 3] &= 0xF0;
                v = 0; dirty = 1;
            }
            if (!v) nfree++;
        }
        if (dirty) {
            fsinfo_unknown();   /* a cut during this repair leaves the count unknown, never wrong */
            for (u8 f = 0; f < num_fats; f++) blockdev_write_sector(fat_start + (u32)f * fat_size_sectors + row, a);
        }
    }
    if (fat32 && fsinfo_lba >= 1 && fsinfo_lba < reserved_sectors && blockdev_read_sector(fsinfo_lba, a)
        && rd32(a) == 0x41615252 && rd32(&a[484]) == 0x61417272 && rd32(&a[488]) != nfree) {
        for (u32 i = 0; i < 4; i++) a[488 + i] = (u8)(nfree >> (8 * i));
        if (blockdev_write_sector(fsinfo_lba, a)) fsinfo_done = 0;   /* the first later change marks it unknown again */
    }
}

/* v29: fat's own functions already match struct vfs_ops's signatures
   exactly, no adapter shims needed, just point the table at them. */
unsigned int fat_cwd_get(void) { return current_dir_cluster; }
void fat_cwd_set(unsigned int c) { current_dir_cluster = c; }
static const struct vfs_ops fat_vfs_ops = {
    "fat", fat_read_file, fat_list, fat_delete, fat_chdir, fat_mkdir, fat_write_file, fat_replace_file, fat_cwd_get, fat_cwd_set
};

void fat_vfs_register(void) {
    vfs_register("fat", &fat_vfs_ops);
}
