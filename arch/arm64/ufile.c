/* M3d: the file calls of user/jtsys.h for the one EL0 app, with the i386 semantics docs/ARCHITECTURE.md gives them (the
   same errnos, the 8 KB open() limit, 255 bytes a read or write, five descriptors, 3 to 7). They run over drivers/fat.c on
   the partition emmc.c mounted: the Pi's SD card, or the virtio disk on QEMU's virt machine. An app's paths are relative
   to the card's JT/ folder, its root: Notes asks for "NOTES/NOTES/N0000001.TXT" and gets JT/NOTES/NOTES/N0000001.TXT. No
   leading slash, no "." or ".." parts, at most 63 bytes and 8 parts, so nothing outside JT/ is reachable, the boot files
   least of all. Every user pointer goes through main.c's span checks before a byte moves, one byte at a time for a path.

   A descriptor is a whole-file buffer, as on i386: open loads the file (8192 bytes at most, a bigger one is -EINVAL), read
   and write work on the buffer at the descriptor's offset, and close writes it back with fat_replace_file and reads it
   back to compare (-EIO when they differ). The kernel's heap never frees, so the five buffers are static. An app that
   exits or faults has its descriptors closed for it, the flush included (main.c app_end).

   fat.c has one current directory for the whole kernel. A call walks from the root into JT/ and down the path, then puts
   the cursor back where it found it (fat_cwd_set), so the Terminal's ls and cat still start at the root. That is only
   safe because one EL0 app runs at a time and is only ever parked at a yield, never inside a call: two apps, or an app
   preempted mid-call, would need fat.c's state per caller first. */
#include "emmc.h"
#include "accounts.h"
#include "home.h"
#include "../../drivers/fat.h"

int user_in_range(unsigned long a, unsigned long n);   /* main.c: memory the app may have the kernel read */
int user_writable(unsigned long a, unsigned long n);   /* main.c: memory the kernel may store into for it (never code) */

#define EIO     5
#define EACCES 13
#define EBADF   9
#define EAGAIN 11
#define EFAULT 14
#define EINVAL 22
#define EISDIR 21
#define EMFILE 24
#define EFBIG  27
#define ENOSPC 28
#define ESPIPE 29
#define ENOENT  2
#define ENOSYS 38
#define O_ACCMODE 0x003
#define O_WRONLY  0x001
#define O_CREAT   0x040
#define O_TRUNC   0x200
#define O_APPEND  0x400
#define IO_MAX 255                 /* bytes a read or write moves per call */
#define OPEN_MAX_FILE 8192
#define PATH_MAX 63                /* bytes of path, the NUL not counted */
#define PATH_DEPTH 8
#define FIRST_FD 3
#define NFDS 12                    /* 4.0: several apps at once, each with a few */
#define READDIR_MAX 64
#define DIRENT_NAME 32             /* struct jt_dirent { char name[32]; unsigned size, is_dir; }: 40 bytes, no pointers, the same on LP64 */
#define READFILE_CAP_MAX (6u << 20)

unsigned ufile_owner;   /* 4.0: the app (slot + 1) whose calls these are, set by main.c before each run; 0 is the boot self-test */
struct ufd { unsigned owner; int used, dirty; unsigned size, pos, flags; char home[HOME_DIR_MAX + 1]; char name[PATH_MAX + 1]; char data[OPEN_MAX_FILE + 1]; };
static struct ufd fds[NFDS];
static char back[OPEN_MAX_FILE + 1];   /* close's read-back */

static long copy_path(unsigned long a, char *out) {   /* -EFAULT off the app's memory, -EINVAL with no NUL in 64 bytes */
    for (unsigned i = 0; i <= PATH_MAX; i++) {
        if (!user_in_range(a + i, 1)) return -EFAULT;
        out[i] = ((const char *)a)[i];
        if (!out[i]) return 0;
    }
    return -EINVAL;
}
static unsigned saved_cwd;
static void path_leave(void) { fat_cwd_set(saved_cwd); }
/* Split `path` in place and stand in the folder that holds its last `keep` parts (1: a file or folder to open, make or
   remove; 0: a folder to list), starting from JT/ (made first when `make`), or from the signed-in person's JT/HOME/<name>/
   (`home`, made too) when there are accounts. *leaf is what is left. On an error nothing has
   moved. */
static const char *cur_home(char *buf) { return home_mine(buf) ? buf : 0; }   /* 4.11.0: the signed-in person's folder name, 0 with no accounts */
static long path_enter(char *path, int keep, char **leaf, int make, const char *home) {
    char *comp[PATH_DEPTH + 1];
    int n = 0;
    *leaf = path;
    if (path[0] == '/') return -EINVAL;
    if (path[0]) {
        for (char *p = path;;) {
            if (n > PATH_DEPTH) return -EINVAL;
            comp[n++] = p;
            char *q = p;
            while (*q && *q != '/') q++;
            long len = q - p;
            if (len == 0 || (len == 1 && p[0] == '.') || (len == 2 && p[0] == '.' && p[1] == '.')) return -EINVAL;
            if (!*q) break;
            *q = 0;
            p = q + 1;
        }
    }
    if (n < keep || n - keep > PATH_DEPTH) return -EINVAL;
    if (n == 1 && accounts_protected(comp[0])) return -EACCES;   /* 4.10.0: JT/USERS.TXT and JT/SUDOERS.TXT belong to the account commands, not to an app */
    if (!sd_ready()) return -ENOENT;   /* no card: nothing is there */
    saved_cwd = fat_cwd_get();
    fat_cwd_set(0);
    if (!sd_enter_jt(make)) { path_leave(); return -ENOENT; }
    if (home && !home_in(home, make)) { path_leave(); return -ENOENT; }   /* 4.11.0: with accounts the app's root is JT/HOME/<name>/, never the shared JT/ */
    for (int i = 0; i < n - keep; i++) if (!fat_chdir(comp[i])) { path_leave(); return -ENOENT; }
    *leaf = n ? comp[n - 1] : path;
    return 0;
}
static struct ufd *fd_of(unsigned long fd) { return fd >= FIRST_FD && fd < FIRST_FD + NFDS && fds[fd - FIRST_FD].used && fds[fd - FIRST_FD].owner == ufile_owner ? &fds[fd - FIRST_FD] : 0; }   /* an app's own descriptors only */

static long u_open(unsigned long upath, unsigned long flags) {
    if (flags & ~(unsigned long)(O_ACCMODE | O_CREAT | O_TRUNC | O_APPEND)) return -EINVAL;
    unsigned acc = (unsigned)(flags & O_ACCMODE);
    if (acc == O_ACCMODE) return -EINVAL;
    if (!acc && (flags & (O_CREAT | O_TRUNC | O_APPEND))) return -EINVAL;   /* nothing is written back from a read-only fd */
    char name[PATH_MAX + 1], walk[PATH_MAX + 1], *leaf;
    long err = copy_path(upath, name);
    if (err) return err;
    int slot = -1;
    for (int i = 0; i < NFDS; i++) if (!fds[i].used) { slot = i; break; }
    if (slot < 0) return -EMFILE;
    struct ufd *f = &fds[slot];
    char hb[HOME_DIR_MAX + 1];
    const char *home = cur_home(hb);
    for (int i = 0; i <= PATH_MAX; i++) { walk[i] = name[i]; if (!name[i]) break; }
    if ((err = path_enter(walk, 1, &leaf, (flags & O_CREAT) != 0, home))) return err;
    int n = fat_read_file(leaf, f->data, OPEN_MAX_FILE + 1);
    if (n > OPEN_MAX_FILE) { path_leave(); return -EINVAL; }   /* bigger than one open holds: refused, never cut */
    int exists = n > 0;                                        /* empty and missing read the same, as on i386 */
    if (!exists && !(flags & O_CREAT)) { path_leave(); return -ENOENT; }
    if (!exists && !fat_replace_file(leaf, "", 0)) { path_leave(); return -ENOSPC; }   /* O_CREAT lands now */
    path_leave();
    /* O_TRUNC of a file that exists lands at close with the new bytes, in one fat_replace_file, so a power cut
       before close leaves the old file, never an empty one (4.13.0) */
    f->used = 1; f->dirty = exists && (flags & O_TRUNC); f->owner = ufile_owner; f->flags = (unsigned)flags;
    f->size = exists && !(flags & O_TRUNC) ? (unsigned)n : 0;
    f->pos = flags & O_APPEND ? f->size : 0;
    for (int i = 0; i <= PATH_MAX; i++) { f->name[i] = name[i]; if (!name[i]) break; }
    for (int i = 0; i <= HOME_DIR_MAX; i++) f->home[i] = home ? home[i] : 0;   /* the flush goes where the open went, whoever is signed in by then */
    return FIRST_FD + slot;
}
static long u_read(unsigned long fd, unsigned long buf, unsigned long len) {
    if (len > IO_MAX) len = IO_MAX;
    if (!len) return 0;
    if (!user_writable(buf, len)) return -EFAULT;   /* the buffer before the descriptor, as i386 */
    if (fd == 0) return -EAGAIN;                    /* no keyboard stream on ARM: nothing is ever queued */
    struct ufd *f = fd_of(fd);
    if (!f || (f->flags & O_ACCMODE) == O_WRONLY) return -EBADF;
    unsigned n = f->size - f->pos < len ? f->size - f->pos : (unsigned)len;
    for (unsigned i = 0; i < n; i++) ((char *)buf)[i] = f->data[f->pos + i];
    f->pos += n;
    return n;
}
static long u_write(unsigned long fd, unsigned long buf, unsigned long len) {
    if (len > IO_MAX) len = IO_MAX;
    struct ufd *f = fd_of(fd);
    if (!f || !(f->flags & O_ACCMODE)) return -EBADF;
    if (f->flags & O_APPEND) f->pos = f->size;
    if (!len) return 0;
    if (f->pos + len > OPEN_MAX_FILE) return -EFBIG;   /* nothing written, never a short write */
    if (!user_in_range(buf, len)) return -EFAULT;
    for (unsigned i = 0; i < len; i++) f->data[f->pos + i] = ((const char *)buf)[i];
    f->pos += (unsigned)len;
    if (f->pos > f->size) f->size = f->pos;
    f->dirty = 1;
    return (long)len;
}
static long u_lseek(unsigned long fd, int off, unsigned long whence) {
    if (fd < FIRST_FD) return -ESPIPE;   /* 0, 1 and 2 are streams */
    struct ufd *f = fd_of(fd);
    if (!f) return -EBADF;
    long base = whence == 0 ? 0 : whence == 1 ? (long)f->pos : whence == 2 ? (long)f->size : -1;
    if (base < 0) return -EINVAL;
    long np = base + off;
    if (np < 0 || np > (long)f->size) return -EINVAL;   /* past the end would be a hole of bytes nobody wrote */
    f->pos = (unsigned)np;
    return np;
}
static long flush(struct ufd *f) {   /* the buffer back to the card, then read back and compared */
    if (!f->dirty) return 0;
    char walk[PATH_MAX + 1], *leaf;
    for (int i = 0; i <= PATH_MAX; i++) { walk[i] = f->name[i]; if (!f->name[i]) break; }
    if (path_enter(walk, 1, &leaf, 0, f->home[0] ? f->home : 0)) return -EIO;   /* its folder went away since open */
    int ok = fat_replace_file(leaf, f->data, f->size), n = ok ? fat_read_file(leaf, back, sizeof back) : -1;
    path_leave();
    if (n < 0 || (unsigned)n != f->size) return -EIO;
    for (unsigned i = 0; i < f->size; i++) if (back[i] != f->data[i]) return -EIO;
    f->dirty = 0;
    return 0;
}
static long u_close(unsigned long fd) {   /* the descriptor goes even when the flush fails */
    struct ufd *f = fd_of(fd);
    if (!f) return -EBADF;
    long err = flush(f);
    f->used = 0;
    return err;
}
static unsigned char stage[READDIR_MAX][DIRENT_NAME + 8];
static unsigned dir_total;
static void collect(const char *name, unsigned size, int is_dir) {
    unsigned i = dir_total++, k = 0;
    if (i >= READDIR_MAX) return;   /* counted, not stored: the answer says the listing was cut */
    unsigned char *d = stage[i];
    for (; name[k] && k < DIRENT_NAME - 1; k++) d[k] = (unsigned char)name[k];
    for (; k < DIRENT_NAME; k++) d[k] = 0;
    if (is_dir) size = 0;
    for (int b = 0; b < 4; b++) { d[DIRENT_NAME + b] = (unsigned char)(size >> 8 * b); d[DIRENT_NAME + 4 + b] = b ? 0 : is_dir != 0; }
}
static long u_readdir(unsigned long upath, unsigned long out, unsigned long max) {
    char walk[PATH_MAX + 1], *leaf;
    long err = copy_path(upath, walk);
    if (err) return err;
    if (max > READDIR_MAX) max = READDIR_MAX;
    if (max && !user_writable(out, max * (DIRENT_NAME + 8))) return -EFAULT;   /* the whole array, before anything runs */
    if (walk[0] == '.' && !walk[1]) walk[0] = 0;
    char hb[HOME_DIR_MAX + 1];
    if ((err = path_enter(walk, 0, &leaf, 0, cur_home(hb)))) return err;
    dir_total = 0;
    fat_list(collect);
    path_leave();
    unsigned n = dir_total < max ? dir_total : (unsigned)max;
    for (unsigned i = 0; i < n; i++) for (unsigned b = 0; b < DIRENT_NAME + 8; b++) ((unsigned char *)out)[i * (DIRENT_NAME + 8) + b] = stage[i][b];
    return dir_total;
}
static long u_mkdir(unsigned long upath) {
    char walk[PATH_MAX + 1], *leaf;
    long err = copy_path(upath, walk);
    if (err) return err;
    char hb[HOME_DIR_MAX + 1];
    if ((err = path_enter(walk, 1, &leaf, 1, cur_home(hb)))) return err;
    int ok = fat_mkdir(leaf);
    path_leave();
    return ok ? 0 : -ENOSPC;   /* a taken name, a full card or a full folder: fat.c answers only yes or no */
}
static long u_unlink(unsigned long upath) {
    char walk[PATH_MAX + 1], *leaf;
    long err = copy_path(upath, walk);
    if (err) return err;
    char hb[HOME_DIR_MAX + 1];
    if ((err = path_enter(walk, 1, &leaf, 0, cur_home(hb)))) return err;
    if (fat_chdir(leaf)) { path_leave(); return -EISDIR; }   /* a folder: never removed through unlink */
    int ok = fat_delete(leaf);
    path_leave();
    return ok ? 0 : -ENOENT;
}
static long u_readfile(unsigned long upath, unsigned long buf, unsigned long cap) {
    if (!cap || cap > READFILE_CAP_MAX) return -EINVAL;
    char walk[PATH_MAX + 1], *leaf;
    long err = copy_path(upath, walk);
    if (err) return err;
    if (!user_writable(buf, cap)) return -EFAULT;
    char hb[HOME_DIR_MAX + 1];
    if ((err = path_enter(walk, 1, &leaf, 0, cur_home(hb)))) return err;
    int n = fat_read_file(leaf, (void *)buf, (unsigned)cap);
    path_leave();
    return n > 0 ? n : -ENOENT;
}

/* SYS_READAT (403, 6.1.0): len bytes of a file from byte off, into a user buffer, through fat_read_at; Movies streams
   clips bigger than READFILE's 6 MB with it. The argument block and the whole buffer are checked before anything runs. */
struct readat_args { unsigned long buf; unsigned int off, len; };
static long u_readat(unsigned long upath, unsigned long uargs) {
    if (!user_writable(uargs, sizeof(struct readat_args))) return -EFAULT;
    struct readat_args a = *(const struct readat_args *)uargs;
    if (!a.len || a.len > READFILE_CAP_MAX) return -EINVAL;
    char walk[PATH_MAX + 1], *leaf;
    long err = copy_path(upath, walk);
    if (err) return err;
    if (!user_writable(a.buf, a.len)) return -EFAULT;
    char hb[HOME_DIR_MAX + 1];
    if ((err = path_enter(walk, 1, &leaf, 0, cur_home(hb)))) return err;
    int n = fat_read_at(leaf, a.off, (void *)a.buf, a.len);
    path_leave();
    return n < 0 ? -ENOENT : n;
}

/* main.c's exc_el0_sync: the file calls, by user/jtsys.h's numbers; -ENOSYS for anything else. */
long ufile_syscall(unsigned long n, unsigned long a0, unsigned long a1, unsigned long a2) {
    switch (n) {
    case 3:   return u_read(a0, a1, a2);
    case 4:   return u_write(a0, a1, a2);
    case 5:   return u_open(a0, a1);
    case 6:   return u_close(a0);
    case 19:  return u_lseek(a0, (int)a1, a2);
    case 388: return u_readdir(a0, a1, a2);
    case 389: return u_mkdir(a0);
    case 390: return u_unlink(a0);
    case 401: return u_readfile(a0, a1, a2);
    case 403: return u_readat(a0, a1);
    }
    return -ENOSYS;
}
/* An app ended (exit, fault or its red dot): every descriptor it left open is closed, written back first. */
void ufile_release(void) {
    for (int i = 0; i < NFDS; i++) if (fds[i].used && fds[i].owner == ufile_owner) u_close(FIRST_FD + (unsigned long)i);
}
/* Power (4.16.0): every descriptor with unwritten bytes goes to the card before a restart or shutdown; returns how many were
   written, -1 when one failed. The descriptors stay open (the machine is about to go), so nothing here frees anything. */
int ufile_sync(void) {
    int n = 0, bad = 0;
    for (int i = 0; i < NFDS; i++) if (fds[i].used && fds[i].dirty) { if (flush(&fds[i])) bad = 1; else n++; }
    return bad ? -1 : n;
}
/* The power check's build (main.c POWER_FILE_TEST) calls this: a file an app wrote and has not closed, as Notes is mid-save. */
void ufile_selftest_dirty(const char *name, const char *text) {
    struct ufd *f = &fds[NFDS - 1];
    unsigned n = 0, k = 0;
    while (text[n] && n < 64) { f->data[n] = text[n]; n++; }
    for (; name[k] && k < PATH_MAX; k++) f->name[k] = name[k];
    f->name[k] = 0; f->home[0] = 0;
    f->used = f->dirty = 1; f->owner = 0; f->flags = O_WRONLY; f->size = f->pos = n;
}
/* Folders an app expects before it runs, made inside JT/ (JT/ too) when missing: Notes gets NOTES/NOTES, its default
   folder, as i386's notes_check_support makes it. Without NOTES/ it would fall back to loose files in its root. */
void ufile_mkdirs(const char *path) {
    char walk[PATH_MAX + 1], *leaf;
    unsigned i = 0;
    for (; path[i] && i < PATH_MAX; i++) walk[i] = path[i];
    walk[i] = 0;
    char hb[HOME_DIR_MAX + 1];
    const char *home = cur_home(hb);
    if (path_enter(walk, 0, &leaf, 1, home) == 0) { path_leave(); return; }   /* all there already */
    for (unsigned k = 0; k <= i; k++) {   /* one more part at a time */
        if (path[k] != '/' && path[k]) continue;
        for (unsigned j = 0; j < k; j++) walk[j] = path[j];
        walk[k] = 0;
        if (path_enter(walk, 1, &leaf, 1, home)) return;
        fat_mkdir(leaf);   /* fails harmlessly when it is there */
        path_leave();
    }
}
