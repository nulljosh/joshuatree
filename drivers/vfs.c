#include "vfs.h"

#define MAX_BACKENDS 4
static const struct vfs_ops *backends[MAX_BACKENDS];
static int n_backends = 0;
static int active = -1;

void vfs_register(const char *name, const struct vfs_ops *ops) {
    (void)name;
    if (n_backends >= MAX_BACKENDS) return;
    backends[n_backends] = ops;
    if (active < 0) active = n_backends; /* first registered backend wins by default */
    n_backends++;
}

static int streq(const char *a, const char *b) {
    while (*a && *b) { if (*a != *b) return 0; a++; b++; }
    return *a == *b;
}

int vfs_switch(const char *name) {
    for (int i = 0; i < n_backends; i++) {
        if (streq(backends[i]->name, name)) { active = i; return 1; }
    }
    return 0;
}

const char *vfs_current_name(void) {
    return active >= 0 ? backends[active]->name : "(none)";
}

int vfs_read_file(const char *name, void *buf, unsigned int bufsize) {
    return active < 0 ? -1 : backends[active]->read_file(name, buf, bufsize);
}
void vfs_list(void (*cb)(const char *name, unsigned int size, int is_dir)) {
    if (active >= 0) backends[active]->list(cb);
}
int vfs_delete(const char *name) {
    return active < 0 ? 0 : backends[active]->delete_(name);
}
int vfs_chdir(const char *name) {
    return active < 0 ? 0 : backends[active]->chdir(name);
}
int vfs_mkdir(const char *name) {
    return active < 0 ? 0 : backends[active]->mkdir(name);
}
int vfs_write_file(const char *name, const void *data, unsigned int len) {
    return active < 0 ? 0 : backends[active]->write_file(name, data, len);
}
int vfs_replace_file(const char *name, const void *data, unsigned int len) {
    return active < 0 ? 0 : backends[active]->replace_file(name, data, len);
}
