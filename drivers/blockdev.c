#include "blockdev.h"

#define MAX_BACKENDS 4
static const struct blockdev_ops *backends[MAX_BACKENDS];
static int n_backends = 0;
static int active = -1;

static int streq(const char *a, const char *b) {
    while (*a && *b) { if (*a != *b) return 0; a++; b++; }
    return *a == *b;
}

void blockdev_register(const char *name, const struct blockdev_ops *ops) {
    (void)name;
    if (n_backends >= MAX_BACKENDS) return;
    backends[n_backends] = ops;
    if (active < 0) active = n_backends; /* first registered wins by default, same convention as vfs.c */
    n_backends++;
}

int blockdev_switch(const char *name) {
    for (int i = 0; i < n_backends; i++) if (streq(backends[i]->name, name)) { active = i; return 1; }
    return 0;
}

const char *blockdev_current_name(void) {
    return active >= 0 ? backends[active]->name : "(none)";
}

int blockdev_read_sector(unsigned int lba, void *buf) {
    return active < 0 ? 0 : backends[active]->read_sector(lba, buf);
}
int blockdev_write_sector(unsigned int lba, const void *buf) {
    return active < 0 ? 0 : backends[active]->write_sector(lba, buf);
}
