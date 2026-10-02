#include "ramfs.h"
#include "vfs.h"
#include "kheap.h"
#include "../kernel/exec.h" /* JT_USER_IMAGE_MAX: the file cap follows the program window */

#define RAMFS_MAX_FILES 20 /* 2026-10-01: was 8; the demo seeds a DOCS/ folder and its files on top of the ring-3 binaries, and a slot is only ~50 bytes */
#define RAMFS_NAME_LEN  32
#define RAMFS_FILE_SIZE JT_USER_IMAGE_MAX /* 2026-10-01: 128KB, the window grew; each file now kmallocs only what it holds (grown on a bigger write) so eight slots do not pin 1MB of heap. Earlier: 1.9.23: 28KB = JT_USER_IMAGE_MAX, 1.9.23: 28KB = JT_USER_IMAGE_MAX, Weather with its antialiased font atlas is 24KB and its .rodata was cut at 16384. Earlier:  1.9.3: ring-3 binaries are written here when no disk is mounted; Fieldbook is 6.3KB and was silently cut at 4096. 1.9.12: Calendar is 13.3KB and was cut at 8192 the same way (its .rodata never loaded) */

struct ramfs_file {
    char name[RAMFS_NAME_LEN];
    unsigned char *data; /* kmalloc'd to the write's size, regrown when a bigger one lands: static bss would eat the kernel/program-window gap */
    unsigned int len;
    unsigned int cap;
    int used;
    int is_dir;      /* seeded folders only (ramfs_seed_dir); a folder has no data */
    unsigned int parent; /* slot index + 1 of the containing folder, 0 = root */
};

static struct ramfs_file files[RAMFS_MAX_FILES];
static unsigned int cwd; /* slot index + 1 of the current folder, 0 = root (the vfs cwd cursor) */

static int streq(const char *a, const char *b) {
    while (*a && *b) { if (*a != *b) return 0; a++; b++; }
    return *a == *b;
}

static int find(const char *name) {
    for (int i = 0; i < RAMFS_MAX_FILES; i++) if (files[i].used && files[i].parent == cwd && streq(files[i].name, name)) return i;
    return -1;
}

static int ramfs_read_file(const char *name, void *buf, unsigned int bufsize) {
    int i = find(name);
    if (i < 0 || files[i].is_dir) return -1;
    unsigned int n = files[i].len < bufsize ? files[i].len : bufsize;
    for (unsigned int j = 0; j < n; j++) ((unsigned char *)buf)[j] = files[i].data[j];
    return (int)n;
}

static void ramfs_list(void (*cb)(const char *name, unsigned int size, int is_dir)) {
    for (int i = 0; i < RAMFS_MAX_FILES; i++) if (files[i].used && files[i].parent == cwd) cb(files[i].name, files[i].is_dir ? 0 : files[i].len, files[i].is_dir);
}

static int ramfs_delete(const char *name) {
    int i = find(name);
    if (i < 0 || files[i].is_dir) return 0; /* folders are seeded, never removed */
    files[i].used = 0;
    return 1;
}

/* Folders exist only when the kernel seeds them (ramfs_seed_dir), one level
   deep. chdir walks into one or back out with "..". mkdir from a program
   still honestly reports failure (callers like Samantha's notes fall back to
   the flat namespace on that), rather than quietly eating file slots. */
static int ramfs_chdir(const char *name) {
    if (name[0] == '.' && name[1] == '.' && !name[2]) {
        if (!cwd) return 0;
        cwd = files[cwd - 1].parent;
        return 1;
    }
    int i = find(name);
    if (i < 0 || !files[i].is_dir) return 0;
    cwd = (unsigned int)i + 1;
    return 1;
}
static int ramfs_mkdir(const char *name)  { (void)name; return 0; }
static unsigned int ramfs_cwd_get(void) { return cwd; }
static void ramfs_cwd_set(unsigned int c) { cwd = c <= RAMFS_MAX_FILES ? c : 0; }

int ramfs_seed_dir(const char *name) {
    if (cwd || find(name) >= 0) return 0; /* root level only, never twice */
    for (int j = 0; j < RAMFS_MAX_FILES; j++) if (!files[j].used) {
        int k = 0; while (name[k] && k < RAMFS_NAME_LEN - 1) { files[j].name[k] = name[k]; k++; } files[j].name[k] = 0;
        files[j].len = 0; files[j].is_dir = 1; files[j].parent = 0; files[j].used = 1;
        return 1;
    }
    return 0;
}

static int ramfs_write_common(const char *name, const void *data, unsigned int len, int replace) {
    int i = find(name);
    if (i >= 0 && files[i].is_dir) return 0;
    if (i >= 0 && !replace) return 0;
    if (i < 0) {
        for (int j = 0; j < RAMFS_MAX_FILES; j++) if (!files[j].used) { i = j; break; }
        if (i < 0) return 0; /* full */
    }
    unsigned int n = len < RAMFS_FILE_SIZE ? len : RAMFS_FILE_SIZE;
    if (files[i].data && files[i].cap < n) { kfree(files[i].data); files[i].data = 0; files[i].cap = 0; }
    if (!files[i].data) { files[i].data = (unsigned char *)kmalloc(n ? n : 1); files[i].cap = n; }
    if (!files[i].data) { files[i].cap = 0; return 0; }
    int k = 0; while (name[k] && k < RAMFS_NAME_LEN - 1) { files[i].name[k] = name[k]; k++; } files[i].name[k] = 0;
    for (unsigned int j = 0; j < n; j++) files[i].data[j] = ((const unsigned char *)data)[j];
    files[i].len = n;
    files[i].is_dir = 0;
    files[i].parent = cwd;
    files[i].used = 1;
    return 1;
}

static int ramfs_write_file(const char *name, const void *data, unsigned int len)   { return ramfs_write_common(name, data, len, 0); }
static int ramfs_replace_file(const char *name, const void *data, unsigned int len) { return ramfs_write_common(name, data, len, 1); }

static const struct vfs_ops ramfs_ops = {
    "ramfs", ramfs_read_file, ramfs_list, ramfs_delete, ramfs_chdir, ramfs_mkdir, ramfs_write_file, ramfs_replace_file, ramfs_cwd_get, ramfs_cwd_set
};

void ramfs_init(void) {
    for (int i = 0; i < RAMFS_MAX_FILES; i++) { files[i].used = 0; files[i].is_dir = 0; files[i].parent = 0; }
    cwd = 0;
    vfs_register("ramfs", &ramfs_ops);
}
