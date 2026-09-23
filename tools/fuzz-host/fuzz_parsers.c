/* Deterministic fuzz driver for drivers/http.c (status-line/header parsing),
   drivers/json.c (the string/number extractor weather/stocks/chat use) and
   drivers/fat.c (FAT16 boot sector, directory entries, cluster chains), run
   by tools/checks/parser-fuzz-check.sh under -fsanitize=address,undefined.
   Same shape as tools/fuzz-host/fuzz_decoders.c: every seed sample
   truncated at every length (or ~200 evenly spaced lengths for larger
   samples), ~2000 deterministic single/multi-byte mutations per sample
   (fixed PRNG seed, reproducible), and a handful of handcrafted nasties. A
   per-input alarm() kills the process if any single input hangs, so a
   stuck case fails loudly instead of hanging CI.

   drivers/net.c is NOT linked in: it owns the actual NIC/Ethernet/ARP/TCP
   state machine, which needs real hardware or QEMU to drive, nothing this
   harness can host-fuzz meaningfully. http.c only calls two functions out
   of it (dns_resolve, tcp_get_timeout); this file defines host stand-ins
   for exactly those two, the same "stub the kernel symbols the file pulls
   in" approach tools/png-host/kheap.h and tools/jpeg-host/kheap.h already
   use for kmalloc/kfree. The stub tcp_get_timeout hands back the fuzzed
   bytes as if they were the raw server reply, which is exactly the
   untrusted data http_body_only's status-line and header/body-boundary
   parsing has to survive.

   drivers/fat.c is fuzzed by registering a small in-memory "disk" backend
   (drivers/blockdev.c compiled in unchanged, same registration API
   drivers/ramdisk.c uses) whose read_sector serves straight out of the
   current fuzzed buffer and bounds-checks itself against that buffer's
   real size, so any lba a corrupted boot sector/FAT table sends fat.c
   chasing off into is safely rejected instead of ever being an
   out-of-bounds access in this harness's own code.

   This is a bytes-in, does-it-crash harness: it does not check output
   correctness. Any return code from these functions is fine; a crash,
   sanitizer report, or hang is not. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <signal.h>
#include <setjmp.h>

#include "http.h"
#include "json.h"
#include "fat.h"
#include "blockdev.h"
#include "vfs.h"

static sigjmp_buf g_jmp;
static volatile sig_atomic_t g_timed_out = 0;

static void on_alarm(int sig) {
    (void)sig;
    g_timed_out = 1;
    siglongjmp(g_jmp, 1);
}

/* ---- host stand-ins for the two net.c functions http.c calls ----------- */

int dns_resolve(const char *hostname, unsigned int dns_server_ip, unsigned int *ip_out) {
    (void)hostname; (void)dns_server_ip;
    *ip_out = 0x0A000202u;
    return 1;
}

/* fat_vfs_register (never called by this harness) references vfs_register;
   linked here only to satisfy that symbol, the same reasoning dns_resolve
   above gets stubbed for a call path this harness never actually takes. */
void vfs_register(const char *name, const struct vfs_ops *ops) { (void)name; (void)ops; }

static const unsigned char *g_http_fuzz_data;
static unsigned g_http_fuzz_len;

/* Stands in for a real TCP connection: hands back the current fuzzed
   buffer as if it were the raw bytes a hostile/broken server sent back,
   exactly what http_body_only's parsing (status line, \r\n\r\n split,
   body copy) has to survive without a real network round trip. */
int tcp_get_timeout(unsigned int dest_ip, unsigned short dest_port,
                    const void *request, unsigned int request_len,
                    void *response, unsigned int response_maxlen,
                    unsigned int reply_timeout_ticks) {
    (void)dest_ip; (void)dest_port; (void)request; (void)request_len; (void)reply_timeout_ticks;
    unsigned copy = g_http_fuzz_len < response_maxlen ? g_http_fuzz_len : response_maxlen;
    memcpy(response, g_http_fuzz_data, copy);
    return (int)copy;
}

/* xorshift32, fixed seed: deterministic across runs/platforms/CI. */
static uint32_t g_rng = 0xC0FFEE01u;
static uint32_t rnd(void) {
    uint32_t x = g_rng;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    return g_rng = x;
}

typedef int (*fuzz_fn)(const unsigned char *, unsigned);

static long g_cases = 0;
static int g_bad = 0;
static const char *g_regress_dir;

static void save_crash(const char *dir, const char *label, const unsigned char *data, unsigned len) {
    char path[512];
    snprintf(path, sizeof path, "%s/%s.bin", dir, label);
    FILE *f = fopen(path, "wb");
    if (f) { fwrite(data, 1, len, f); fclose(f); }
    fprintf(stderr, "CRASH/HANG saved to %s (%u bytes)\n", path, len);
}

static void try_one(fuzz_fn fn, const unsigned char *data, unsigned len, const char *label) {
    g_cases++;
    int r = fn(data, len);
    if (r == -1 && g_timed_out) {
        g_bad = 1;
        save_crash(g_regress_dir, label, data, len);
        g_timed_out = 0;
    }
}

static void fuzz_sample(fuzz_fn fn, const unsigned char *orig, unsigned n,
                        const char *name, const char *regress_dir) {
    unsigned char *buf = malloc(n ? n : 1);
    memcpy(buf, orig, n);

    /* (a) truncation at every length (small) or ~200 evenly spaced (large) */
    if (n <= 512) {
        for (unsigned len = 0; len <= n; len++) {
            char label[128];
            snprintf(label, sizeof label, "%s_trunc_%u", name, len);
            g_regress_dir = regress_dir;
            try_one(fn, buf, len, label);
        }
    } else {
        for (int i = 0; i <= 200; i++) {
            unsigned len = (unsigned)((uint64_t)i * n / 200);
            char label[128];
            snprintf(label, sizeof label, "%s_trunc_%u", name, len);
            g_regress_dir = regress_dir;
            try_one(fn, buf, len, label);
        }
    }

    /* (b) ~2000 deterministic mutations: mix of single-byte and multi-byte
       flips at pseudo-random (but fixed-seed) offsets/values. */
    if (n > 0) for (int i = 0; i < 2000; i++) {
        unsigned char *m = malloc(n);
        memcpy(m, orig, n);
        unsigned nmut = 1 + (rnd() % 4); /* 1..4 bytes touched */
        for (unsigned j = 0; j < nmut; j++) {
            unsigned off = rnd() % n;
            m[off] ^= (unsigned char)(rnd() & 0xFF);
        }
        char label[128];
        snprintf(label, sizeof label, "%s_mut_%d", name, i);
        g_regress_dir = regress_dir;
        try_one(fn, m, n, label);
        free(m);
    }

    free(buf);
}

/* ---- HTTP: fuzz http.c's response parsing (status line, header/body
   split) via the stubbed tcp_get_timeout above -------------------------- */

static int run_http_get(const unsigned char *data, unsigned len) {
    g_http_fuzz_data = data; g_http_fuzz_len = len;
    if (sigsetjmp(g_jmp, 1)) return -1;
    alarm(1);
    unsigned char body[512];
    int r = http_get_timeout("203.0.113.7", "/x", 80, body, sizeof body, 1);
    alarm(0);
    return r;
}

static int run_http_post(const unsigned char *data, unsigned len) {
    g_http_fuzz_data = data; g_http_fuzz_len = len;
    if (sigsetjmp(g_jmp, 1)) return -1;
    alarm(1);
    unsigned char resp[512];
    int r = http_post("203.0.113.7", "/x", 80, "{}", 2, resp, sizeof resp);
    alarm(0);
    return r;
}

/* ---- JSON: fuzz json.c's string/number extractor and escaper, always on
   a NUL-terminated buffer, the same convention every real caller in
   kernel.c/chat.h uses (http_get/http_post's response is always
   NUL-terminated by the caller right before it reaches json.c) --------- */

static int run_json(const unsigned char *data, unsigned len) {
    char *buf = malloc((size_t)len + 1);
    memcpy(buf, data, len);
    buf[len] = 0;
    if (sigsetjmp(g_jmp, 1)) { free(buf); return -1; }
    alarm(1);
    char out[256];
    int r = 0;
    r += (int)json_extract_string(buf, "content", out, sizeof out);
    r += (int)json_extract_string(buf, "city", out, sizeof out);
    r += (int)json_extract_string(buf, "response", out, sizeof out);
    r += (int)json_extract_number_text(buf, "lat", out, sizeof out);
    r += (int)json_extract_number_text(buf, "lon", out, sizeof out);
    char esc[256];
    r += (int)json_escape(buf, esc, sizeof esc);
    alarm(0);
    free(buf);
    return r;
}

/* ---- FAT16: fuzz fat.c against a mutated in-memory disk image, served
   through the real drivers/blockdev.c registration API -------------------
   */

#define FAT_SECTOR_SIZE 512
#define FAT_IMG_SECTORS 64
#define FAT_IMG_SIZE (FAT_IMG_SECTORS * FAT_SECTOR_SIZE)

static const unsigned char *g_disk_data;
static unsigned g_disk_len;

static int fuzzdisk_read_sector(unsigned int lba, void *buf) {
    unsigned long off = (unsigned long)lba * FAT_SECTOR_SIZE;
    if (off + FAT_SECTOR_SIZE > g_disk_len) { memset(buf, 0, FAT_SECTOR_SIZE); return 0; }
    memcpy(buf, g_disk_data + off, FAT_SECTOR_SIZE);
    return 1;
}
static int fuzzdisk_write_sector(unsigned int lba, const void *buf) {
    (void)lba; (void)buf;
    return 1; /* accept, no-op: writes aren't the hostile-input surface here */
}
static const struct blockdev_ops fuzzdisk_ops = { "fuzzdisk", fuzzdisk_read_sector, fuzzdisk_write_sector };

static void noop_list_cb(const char *name, unsigned int size, int is_dir) {
    (void)name; (void)size; (void)is_dir;
}

static int run_fat(const unsigned char *data, unsigned len) {
    g_disk_data = data; g_disk_len = len;
    if (sigsetjmp(g_jmp, 1)) return -1;
    alarm(1);
    int r = fat_mount();
    if (r) {
        fat_list(noop_list_cb);
        char buf[256];
        fat_read_file("TEST.TXT", buf, sizeof buf);
        fat_read_file("NOPE.TXT", buf, sizeof buf); /* not found: exercise the miss path too */
        if (fat_chdir("SUBDIR")) {
            fat_list(noop_list_cb);
            fat_chdir("..");
        }
        /* Also probes fat_write_file's alloc_cluster free-cluster search,
           which walks 2..total_clusters+2: total_clusters is derived from
           boot-sector fields a hostile image fully controls. */
        fat_write_file("NEW.TXT", "hi", 2);
    }
    alarm(0);
    return r;
}

/* Builds a small, valid FAT16 image: boot sector, one FAT sector, one root
   directory sector (a file "TEST.TXT" and a subdirectory "SUBDIR"), the
   file's one data cluster, and the subdirectory's one data cluster (its
   own "." and ".." entries). Geometry: bytes/sector=512, sectors/cluster=1,
   reserved=1, num_fats=1, root_entries=16 (1 sector), fat_size=1 sector ->
   fat_start=1, root_dir_start=2, data_start=3, cluster 2 -> lba 3, cluster
   3 -> lba 4. */
static unsigned char *build_fat_image(void) {
    unsigned char *img = calloc(1, FAT_IMG_SIZE);

    unsigned char *boot = img;
    boot[11] = 0x00; boot[12] = 0x02; /* bytes_per_sector = 512 */
    boot[13] = 1;                      /* sectors_per_cluster */
    boot[14] = 1; boot[15] = 0;        /* reserved_sectors */
    boot[16] = 1;                      /* num_fats */
    boot[17] = 16; boot[18] = 0;       /* root_entry_count */
    boot[19] = (unsigned char)(FAT_IMG_SECTORS & 0xFF);
    boot[20] = (unsigned char)((FAT_IMG_SECTORS >> 8) & 0xFF); /* total_sectors_16 */
    boot[22] = 1; boot[23] = 0;        /* fat_size_sectors */

    unsigned char *fat_sec = img + 1 * FAT_SECTOR_SIZE;
    fat_sec[0] = 0xF8; fat_sec[1] = 0xFF; /* cluster 0: media descriptor */
    fat_sec[2] = 0xFF; fat_sec[3] = 0xFF; /* cluster 1: reserved */
    fat_sec[4] = 0xFF; fat_sec[5] = 0xFF; /* cluster 2 (TEST.TXT): end of chain */
    fat_sec[6] = 0xFF; fat_sec[7] = 0xFF; /* cluster 3 (SUBDIR): end of chain */

    unsigned char *root = img + 2 * FAT_SECTOR_SIZE;
    memset(root + 0, ' ', 11);
    memcpy(root + 0, "TEST", 4);
    memcpy(root + 8, "TXT", 3);
    root[0 + 11] = 0x00;          /* attr: plain file */
    root[0 + 26] = 2; root[0 + 27] = 0; /* first_cluster_low = 2 */
    root[0 + 28] = 20;            /* file_size = 20 */

    memset(root + 32, ' ', 11);
    memcpy(root + 32, "SUBDIR", 6);
    root[32 + 11] = 0x10;         /* ATTR_DIRECTORY */
    root[32 + 26] = 3; root[32 + 27] = 0; /* first_cluster_low = 3 */

    unsigned char *c2 = img + 3 * FAT_SECTOR_SIZE;
    memcpy(c2, "hello fuzzer disk!!", 20);

    unsigned char *c3 = img + 4 * FAT_SECTOR_SIZE;
    memset(c3 + 0, ' ', 11); c3[0] = '.';
    c3[0 + 11] = 0x10; c3[0 + 26] = 3; c3[0 + 27] = 0;
    memset(c3 + 32, ' ', 11); c3[32] = '.'; c3[33] = '.';
    c3[32 + 11] = 0x10; c3[32 + 26] = 0; c3[32 + 27] = 0;

    return img;
}

/* ---- handcrafted nasties ------------------------------------------------ */

static void http_nasties(const char *regress_dir) {
    static const unsigned char *cases[] = {
        (const unsigned char *)"",
        (const unsigned char *)"H",
        (const unsigned char *)"HTTP",
        (const unsigned char *)"HTTP/1.1",
        (const unsigned char *)"HTTP/1.1 ",
        (const unsigned char *)"HTTP/1.1 2",
        (const unsigned char *)"HTTP/1.1 200",
        (const unsigned char *)"HTTP/1.1 200 OK",
        (const unsigned char *)"HTTP/1.1 200 OK\r\n",
        (const unsigned char *)"HTTP/1.1 200 OK\r\n\r",
        (const unsigned char *)"HTTP/1.1 200 OK\r\n\r\n",
        (const unsigned char *)"HTTP/1.1 abc OK\r\n\r\nbody",
        (const unsigned char *)"\r\n\r\n\r\n\r\n\r\n\r\n\r\n\r\n",
        (const unsigned char *)"HTTP/1.1 999 X\r\nA: 1\r\nB: 2\r\n\r\n",
    };
    for (unsigned i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        unsigned n = (unsigned)strlen((const char *)cases[i]);
        char label[64];
        snprintf(label, sizeof label, "http_nasty_%u", i);
        g_regress_dir = regress_dir;
        try_one(run_http_get, cases[i], n, label);
        snprintf(label, sizeof label, "http_post_nasty_%u", i);
        g_regress_dir = regress_dir;
        try_one(run_http_post, cases[i], n, label);
    }
}

static void json_nasties(const char *regress_dir) {
    static const unsigned char *cases[] = {
        (const unsigned char *)"",
        (const unsigned char *)"{",
        (const unsigned char *)"\"",
        (const unsigned char *)"\"content\":",
        (const unsigned char *)"\"content\":\"",
        (const unsigned char *)"\"content\":\"unterminated",
        (const unsigned char *)"\"content\":\"ends in backslash\\",
        (const unsigned char *)"\"content\":\"\\\\\\\\\\\\\\\\\"",
        (const unsigned char *)"\"lat\":",
        (const unsigned char *)"\"lat\":-",
        (const unsigned char *)"\"lat\":-.eE++--",
        (const unsigned char *)"\"lat\":\"not a number\"",
        (const unsigned char *)"\"latitude\":1,\"lat\":2",
        (const unsigned char *)"{\"content\":null}",
    };
    for (unsigned i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        unsigned n = (unsigned)strlen((const char *)cases[i]);
        char label[64];
        snprintf(label, sizeof label, "json_nasty_%u", i);
        g_regress_dir = regress_dir;
        try_one(run_json, cases[i], n, label);
    }
}

static void fat_nasties(const unsigned char *base, const char *regress_dir) {
    /* huge reserved_sectors: fat_start (and everything derived from it)
       moves far past the physical image */
    {
        unsigned char *img = malloc(FAT_IMG_SIZE); memcpy(img, base, FAT_IMG_SIZE);
        img[14] = 0xFF; img[15] = 0xFF;
        g_regress_dir = regress_dir;
        try_one(run_fat, img, FAT_IMG_SIZE, "fat_huge_reserved");
        free(img);
    }
    /* sectors_per_cluster maxed out (255): stresses cluster_to_lba's
       multiply */
    {
        unsigned char *img = malloc(FAT_IMG_SIZE); memcpy(img, base, FAT_IMG_SIZE);
        img[13] = 0xFF;
        g_regress_dir = regress_dir;
        try_one(run_fat, img, FAT_IMG_SIZE, "fat_huge_spc");
        free(img);
    }
    /* total_sectors_16 == 0 and total_sectors_32 maxed: total_clusters is
       computed from a 4-billion-sector claim with a FAT table that can
       back only ~254 real entries */
    {
        unsigned char *img = malloc(FAT_IMG_SIZE); memcpy(img, base, FAT_IMG_SIZE);
        img[19] = 0; img[20] = 0;
        img[32] = 0xFF; img[33] = 0xFF; img[34] = 0xFF; img[35] = 0xFF;
        g_regress_dir = regress_dir;
        try_one(run_fat, img, FAT_IMG_SIZE, "fat_huge_total_sectors_32");
        free(img);
    }
    /* Same 4-billion-cluster claim, but every byte of the image (not just
       the seed's own two hand-set FAT rows) is non-zero, so every cluster
       alloc_cluster can actually read -- in range or not, since
       fat_entry_read reports a failed blockdev_read_sector as 0xFFFF, not
       0 -- looks "occupied". Its free-cluster search has nothing bounding
       it but total_clusters itself, so this is the one nasty built to
       actually make that search run its full ~4 billion iterations
       instead of finding a false "free" (zero) entry in a couple of
       steps, the way base's mostly-zero-padded image let it. */
    {
        unsigned char *img = malloc(FAT_IMG_SIZE);
        memset(img, 0xAA, FAT_IMG_SIZE); /* every byte non-zero to start */
        img[11] = 0x00; img[12] = 0x02; /* bytes_per_sector = 512 */
        img[13] = 1;                     /* sectors_per_cluster */
        img[14] = 1; img[15] = 0;        /* reserved_sectors */
        img[16] = 1;                     /* num_fats */
        img[17] = 16; img[18] = 0;       /* root_entry_count */
        img[19] = 0; img[20] = 0;        /* total_sectors_16 = 0: forces the 32-bit field */
        img[22] = 1; img[23] = 0;        /* fat_size_sectors */
        img[32] = 0xFF; img[33] = 0xFF; img[34] = 0xFF; img[35] = 0xFF; /* total_sectors_32, huge */
        /* write_file's OWN free-directory-slot search ("NEW.TXT" needs
           somewhere to go) needs one real 0x00 marker byte; only that one
           byte is cleared, name[1] of the same entry stays 0xAA so the
           2-byte pair fat_entry_read sees when it later walks over this
           exact same offset (cluster and directory data share the same
           backing sectors here, nothing stops a corrupted geometry from
           making the FAT table's row math land anywhere) still reads
           non-zero, not "free". */
        img[2 * FAT_SECTOR_SIZE + 2 * 32] = 0x00;
        /* Every readable cluster -- in range (now uniformly 0xAA, so every
           2-byte pair reads non-zero) or out of it (a failed
           blockdev_read_sector, which fat_entry_read reports as 0xFFFF,
           also non-zero) -- looks "occupied", so alloc_cluster's
           free-cluster search has nothing bounding it but total_clusters
           itself and has to run its full ~4 billion iterations. */
        g_regress_dir = regress_dir;
        try_one(run_fat, img, FAT_IMG_SIZE, "fat_alloc_cluster_dos");
        free(img);
    }
    /* num_fats maxed: root_dir_start/data_start pushed out */
    {
        unsigned char *img = malloc(FAT_IMG_SIZE); memcpy(img, base, FAT_IMG_SIZE);
        img[16] = 0xFF;
        g_regress_dir = regress_dir;
        try_one(run_fat, img, FAT_IMG_SIZE, "fat_huge_numfats");
        free(img);
    }
    /* SUBDIR's first_cluster_low set to 1 (a reserved, never-valid cluster
       number): fat_chdir stores it unchecked, the next dir_get_sector call
       on it must still come back safe, not misread memory */
    {
        unsigned char *img = malloc(FAT_IMG_SIZE); memcpy(img, base, FAT_IMG_SIZE);
        img[32 + 26] = 1; img[32 + 27] = 0;
        g_regress_dir = regress_dir;
        try_one(run_fat, img, FAT_IMG_SIZE, "fat_bad_subdir_cluster");
        free(img);
    }
    /* directory entry name[0] never hits 0x00 or 0xE5 (no end-of-directory
       marker anywhere): every sector in the chain looks "still in use" */
    {
        unsigned char *img = malloc(FAT_IMG_SIZE); memcpy(img, base, FAT_IMG_SIZE);
        for (unsigned off = 2 * FAT_SECTOR_SIZE; off < FAT_IMG_SIZE; off += 32) {
            if (img[off] == 0x00) img[off] = 0x41; /* 'A': looks like a real, if garbage, entry */
        }
        g_regress_dir = regress_dir;
        try_one(run_fat, img, FAT_IMG_SIZE, "fat_no_dir_terminator");
        free(img);
    }
}

int main(void) {
    signal(SIGALRM, on_alarm);
    blockdev_register("fuzzdisk", &fuzzdisk_ops);

    const char *regress_dir = "tools/checks/parser-fuzz-regress";
    /* directory is created by the shell wrapper before running us */

    static const unsigned char http_seed1[] =
        "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: 27\r\n\r\n"
        "{\"lat\":49.0,\"lon\":-122.6}\n";
    static const unsigned char http_seed2[] =
        "HTTP/1.0 403 Forbidden\r\nServer: nginx\r\nConnection: close\r\n\r\n<html>blocked</html>";
    fuzz_sample(run_http_get, http_seed1, sizeof(http_seed1) - 1, "http_get_json", regress_dir);
    fuzz_sample(run_http_get, http_seed2, sizeof(http_seed2) - 1, "http_get_403", regress_dir);
    fuzz_sample(run_http_post, http_seed1, sizeof(http_seed1) - 1, "http_post_json", regress_dir);
    http_nasties(regress_dir);

    static const unsigned char json_seed1[] =
        "{\"status\":\"success\",\"latitude\":\"trap\",\"longitude\":1,"
        "\"city\":\"Langley\",\"lat\":49.0983,\"lon\":-122.6498,\"zip\":\"V3A\"}";
    static const unsigned char json_seed2[] =
        "{\"message\":{\"role\":\"assistant\",\"content\":\"Hello, \\\"world\\\"!\\nLine two.\\t\\\\done\"},\"done\":true}";
    static const unsigned char json_seed3[] =
        "{\"current\":{\"temperature_2m\":-3.4,\"weathercode\":71,\"windspeed_10m\":12.5},"
        "\"hourly\":{\"time\":[\"2026-09-22T00:00\",\"2026-09-22T01:00\"],\"temperature_2m\":[-3.4,-3.1]}}";
    fuzz_sample(run_json, json_seed1, sizeof(json_seed1) - 1, "json_geo", regress_dir);
    fuzz_sample(run_json, json_seed2, sizeof(json_seed2) - 1, "json_chat", regress_dir);
    fuzz_sample(run_json, json_seed3, sizeof(json_seed3) - 1, "json_weather", regress_dir);
    json_nasties(regress_dir);

    unsigned char *fat_base = build_fat_image();
    fuzz_sample(run_fat, fat_base, FAT_IMG_SIZE, "fat16_basic", regress_dir);
    fat_nasties(fat_base, regress_dir);
    free(fat_base);

    fprintf(stderr, "parser-fuzz: %ld cases run\n", g_cases);
    puts(g_bad ? "PARSER FUZZ: FAIL (hang detected, see saved regress inputs)" : "PARSER FUZZ: PASS");
    return g_bad;
}
