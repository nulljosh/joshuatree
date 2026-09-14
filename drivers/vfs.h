#ifndef VFS_H
#define VFS_H
/* v29: a real VFS, one vnode-style ops table instead of every caller
   dialing fat_* directly. Deferred correctly at v4 (FAT was the only
   backend that existed, nothing to abstract over yet); real once a second
   backend (ramfs, below) exists to prove the interface actually abstracts
   something instead of being built for an imagined future caller. */
struct vfs_ops {
    const char *name;
    int  (*read_file)(const char *name, void *buf, unsigned int bufsize);
    void (*list)(void (*cb)(const char *name, unsigned int size, int is_dir));
    int  (*delete_)(const char *name);
    int  (*chdir)(const char *name);
    int  (*mkdir)(const char *name);
    int  (*write_file)(const char *name, const void *data, unsigned int len);
    int  (*replace_file)(const char *name, const void *data, unsigned int len);
};

/* Registers a backend as active. Both fat and ramfs call this once at
   boot with their own real ops table; vfs_switch() (the `fsuse` shell
   command) flips which one every vfs_* call below actually reaches. */
void vfs_register(const char *name, const struct vfs_ops *ops);
int  vfs_switch(const char *name); /* 1 on success, 0 if no backend has that name */
const char *vfs_current_name(void);

int  vfs_read_file(const char *name, void *buf, unsigned int bufsize);
void vfs_list(void (*cb)(const char *name, unsigned int size, int is_dir));
int  vfs_delete(const char *name);
int  vfs_chdir(const char *name);
int  vfs_mkdir(const char *name);
int  vfs_write_file(const char *name, const void *data, unsigned int len);
int  vfs_replace_file(const char *name, const void *data, unsigned int len);
#endif
