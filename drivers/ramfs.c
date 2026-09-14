#include "ramfs.h"
#include "vfs.h"

#define RAMFS_MAX_FILES 8
#define RAMFS_NAME_LEN  32
#define RAMFS_FILE_SIZE 4096

struct ramfs_file {
    char name[RAMFS_NAME_LEN];
    unsigned char data[RAMFS_FILE_SIZE];
    unsigned int len;
    int used;
};

static struct ramfs_file files[RAMFS_MAX_FILES];

static int streq(const char *a, const char *b) {
    while (*a && *b) { if (*a != *b) return 0; a++; b++; }
    return *a == *b;
}

static int find(const char *name) {
    for (int i = 0; i < RAMFS_MAX_FILES; i++) if (files[i].used && streq(files[i].name, name)) return i;
    return -1;
}

static int ramfs_read_file(const char *name, void *buf, unsigned int bufsize) {
    int i = find(name);
    if (i < 0) return -1;
    unsigned int n = files[i].len < bufsize ? files[i].len : bufsize;
    for (unsigned int j = 0; j < n; j++) ((unsigned char *)buf)[j] = files[i].data[j];
    return (int)n;
}

static void ramfs_list(void (*cb)(const char *name, unsigned int size, int is_dir)) {
    for (int i = 0; i < RAMFS_MAX_FILES; i++) if (files[i].used) cb(files[i].name, files[i].len, 0);
}

static int ramfs_delete(const char *name) {
    int i = find(name);
    if (i < 0) return 0;
    files[i].used = 0;
    return 1;
}

/* Flat namespace on purpose (see ramfs.h): no subdirectories to move into
   or create, so both honestly report failure rather than pretend to
   support something that isn't there. */
static int ramfs_chdir(const char *name)  { (void)name; return 0; }
static int ramfs_mkdir(const char *name)  { (void)name; return 0; }

static int ramfs_write_common(const char *name, const void *data, unsigned int len, int replace) {
    int i = find(name);
    if (i >= 0 && !replace) return 0;
    if (i < 0) {
        for (int j = 0; j < RAMFS_MAX_FILES; j++) if (!files[j].used) { i = j; break; }
        if (i < 0) return 0; /* full */
    }
    unsigned int n = len < RAMFS_FILE_SIZE ? len : RAMFS_FILE_SIZE;
    int k = 0; while (name[k] && k < RAMFS_NAME_LEN - 1) { files[i].name[k] = name[k]; k++; } files[i].name[k] = 0;
    for (unsigned int j = 0; j < n; j++) files[i].data[j] = ((const unsigned char *)data)[j];
    files[i].len = n;
    files[i].used = 1;
    return 1;
}

static int ramfs_write_file(const char *name, const void *data, unsigned int len)   { return ramfs_write_common(name, data, len, 0); }
static int ramfs_replace_file(const char *name, const void *data, unsigned int len) { return ramfs_write_common(name, data, len, 1); }

static const struct vfs_ops ramfs_ops = {
    "ramfs", ramfs_read_file, ramfs_list, ramfs_delete, ramfs_chdir, ramfs_mkdir, ramfs_write_file, ramfs_replace_file
};

void ramfs_init(void) {
    for (int i = 0; i < RAMFS_MAX_FILES; i++) files[i].used = 0;
    vfs_register("ramfs", &ramfs_ops);
}
