#include "vfs.h"
#include "../kernel/irqlock.h"

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

/* 1.9.23: every entry point is a cli critical section (kernel/irqlock.h).
   FAT keeps one cwd, one sector cache and one mounted state, ramfs one
   file table, and the desktop (task 0) can be preempted anywhere inside
   them; a ring-3 syscall entering through the gate must not find them
   half-updated. Nothing below waits, so holding IF off is bounded by the
   disk read itself. */
int vfs_read_file(const char *name, void *buf, unsigned int bufsize) {
    unsigned int f = irq_save(); int r = active < 0 ? -1 : backends[active]->read_file(name, buf, bufsize); irq_restore(f); return r;
}
void vfs_list(void (*cb)(const char *name, unsigned int size, int is_dir)) {
    unsigned int f = irq_save(); if (active >= 0) backends[active]->list(cb); irq_restore(f);
}
int vfs_delete(const char *name) {
    unsigned int f = irq_save(); int r = active < 0 ? 0 : backends[active]->delete_(name); irq_restore(f); return r;
}
int vfs_chdir(const char *name) {
    unsigned int f = irq_save(); int r = active < 0 ? 0 : backends[active]->chdir(name); irq_restore(f); return r;
}
int vfs_mkdir(const char *name) {
    unsigned int f = irq_save(); int r = active < 0 ? 0 : backends[active]->mkdir(name); irq_restore(f); return r;
}
int vfs_write_file(const char *name, const void *data, unsigned int len) {
    unsigned int f = irq_save(); int r = active < 0 ? 0 : backends[active]->write_file(name, data, len); irq_restore(f); return r;
}
int vfs_replace_file(const char *name, const void *data, unsigned int len) {
    unsigned int f = irq_save(); int r = active < 0 ? 0 : backends[active]->replace_file(name, data, len); irq_restore(f); return r;
}
/* 1.9.23: the cwd is one global cursor per backend. A syscall resolves its
   path from the root, but the desktop may be standing inside NOTES/ when
   the tick lands, so the syscall saves the cursor, goes to root, and puts
   it back before returning (syscall.c path_enter/path_leave). */
unsigned int vfs_cwd_get(void) { return active >= 0 && backends[active]->cwd_get ? backends[active]->cwd_get() : 0; }
void vfs_cwd_set(unsigned int c) { if (active >= 0 && backends[active]->cwd_set) backends[active]->cwd_set(c); }
