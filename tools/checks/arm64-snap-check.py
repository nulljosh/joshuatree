#!/usr/bin/env python3
"""ARM64 home snapshots (arch/arm64/snap.c, drivers/fat.c fat_rmdir; 4.12.0).

Host only: the real snap.c and the real fat.c (under address and undefined-behaviour sanitizers) over a FAT16 image made by
mkfs.fat, with ann's home folder in JT/HOME/ann. A snapshot copies files and nested folders into ~/SNAP/<n>/ and never copies
SNAP into the next one; a fourth snapshot drops the oldest; a file over 32 KB is skipped and counted; a snapshot with no _DONE
is ignored by the list and cleaned by the next; restore puts back changed and deleted files and removes the ones made since;
an unknown id is refused. fat_rmdir refuses a folder that holds something. fsck.fat -n finds the card clean at the end (no
cluster lost by any of the deletes). Skips (exit 0) when clang or mkfs.fat is missing.
"""
import os, shutil, subprocess, sys, tempfile

root = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
arch = os.path.join(root, "arch", "arm64")
if not (shutil.which("clang") and shutil.which("mkfs.fat")):
    print("SKIP: clang or mkfs.fat not installed"); sys.exit(0)

HOST = r'''
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "snap.h"
#include "home.h"
#include "../../drivers/fat.h"
#include "../../drivers/blockdev.h"
FILE *img;
struct vfs_ops;
void vfs_register(const char *n, const struct vfs_ops *o) { (void)n; (void)o; }
int blockdev_read_sector(unsigned lba, void *b) { fseek(img, (long)lba * 512, SEEK_SET); return fread(b, 512, 1, img) == 1; }
int blockdev_write_sector(unsigned lba, const void *b) { fseek(img, (long)lba * 512, SEEK_SET); return fwrite(b, 512, 1, img) == 1; }
char current_user[32] = "ann";
int sd_ready(void) { return 1; }
int sd_enter_jt(int make) { fat_cwd_set(0); if (!fat_chdir("JT")) { if (!make || !fat_mkdir("JT") || !fat_chdir("JT")) return 0; } return 1; }
int home_mine(char out[9]) { strcpy(out, "ann"); return 1; }
int home_make(const char *u) { (void)u; sd_enter_jt(1); if (!fat_chdir("HOME") && (!fat_mkdir("HOME") || !fat_chdir("HOME"))) return 0; if (!fat_chdir("ann") && (!fat_mkdir("ann") || !fat_chdir("ann"))) return 0; fat_cwd_set(0); return 1; }
int home_in(const char *d, int make) { (void)make; return fat_chdir("HOME") && fat_chdir(d); }

static void go(const char *path) {   /* the cursor to a folder from the root, made on the way */
    char p[100]; strcpy(p, path); fat_cwd_set(0);
    for (char *t = strtok(p, "/"); t; t = strtok(0, "/")) if (!fat_chdir(t)) { assert(fat_mkdir(t)); assert(fat_chdir(t)); }
}
static void put(const char *dir, const char *name, const char *body) { go(dir); assert(fat_replace_file(name, body, (unsigned)strlen(body))); fat_cwd_set(0); }
static char *get(const char *dir, const char *name) {   /* the file's text, or 0 */
    static char b[40000]; go(dir); int n = fat_read_file(name, b, sizeof b - 1); fat_cwd_set(0);
    if (n < 0) return 0; b[n] = 0; return b;
}
static int has_dir(const char *path) { char p[100]; strcpy(p, path); fat_cwd_set(0); for (char *t = strtok(p, "/"); t; t = strtok(0, "/")) if (!fat_chdir(t)) { fat_cwd_set(0); return 0; } fat_cwd_set(0); return 1; }
static unsigned seen; static unsigned seen_id[8]; static char seen_info[8][24];
static void cb(unsigned id, const char *info) { if (seen < 8) { seen_id[seen] = id; strcpy(seen_info[seen], info); seen++; } }
#define H "JT/HOME/ann"

int main(int argc, char **argv) {
    (void)argc; img = fopen(argv[1], "r+b"); assert(img);
    extern int fat_mount(void); assert(fat_mount());
    unsigned id, files, skipped;
    assert(snap_take(&id, &files, &skipped) == 1 && id == 1 && files == 0);   /* an empty home is a snapshot too */
    assert(snap_list(cb) == 1 && seen == 1);
    put(H, "A.TXT", "alpha"); put(H, "B.TXT", "bravo"); put(H "/DOCS", "C.TXT", "charlie"); put(H "/DOCS/DEEP", "D.TXT", "delta");
    static char big[40000]; memset(big, 'x', sizeof big - 1); big[sizeof big - 1] = 0; put(H, "BIG.DAT", big);
    assert(snap_take(&id, &files, &skipped) == 1 && id == 2 && files == 4 && skipped == 1);
    assert(!strcmp(get(H "/SNAP/2", "A.TXT"), "alpha") && !strcmp(get(H "/SNAP/2/DOCS/DEEP", "D.TXT"), "delta") && !get(H "/SNAP/2", "BIG.DAT"));
    assert(!has_dir(H "/SNAP/2/SNAP"));
    seen = 0; assert(snap_list(cb) == 2 && seen_id[0] == 1 && seen_id[1] == 2 && !strncmp(seen_info[1], "4 files", 7));
    puts("ok take: nested files copied, the big file skipped and counted, SNAP never copied into itself, listed oldest first");

    go(H "/SNAP/2"); assert(fat_delete("_DONE")); fat_cwd_set(0);            /* snapshot 2 now looks cut short */
    seen = 0; assert(snap_list(cb) == 1 && seen_id[0] == 1);
    assert(snap_take(&id, &files, &skipped) == 1 && id == 3);                  /* it is cleaned, and the number after it is used */
    assert(!has_dir(H "/SNAP/2") && has_dir(H "/SNAP/3"));
    puts("ok a snapshot without _DONE is not listed and the next take removes it");

    assert(snap_take(&id, &files, &skipped) == 1 && id == 4);
    assert(snap_take(&id, &files, &skipped) == 1 && id == 5);
    seen = 0; assert(snap_list(cb) == 3 && seen_id[0] == 3 && seen_id[2] == 5 && !has_dir(H "/SNAP/1"));
    puts("ok only the newest three are kept");

    put(H, "A.TXT", "changed"); put(H, "NEW.TXT", "new"); put(H "/NEWDIR", "E.TXT", "e"); put(H "/DOCS", "X.TXT", "x");
    go(H); assert(fat_delete("B.TXT")); fat_cwd_set(0);
    assert(snap_restore(9, &files, &skipped) == -3);
    assert(snap_restore(5, &files, &skipped) == 1 && files == 4);
    assert(!strcmp(get(H, "A.TXT"), "alpha") && !strcmp(get(H, "B.TXT"), "bravo") && !get(H, "NEW.TXT") && !has_dir(H "/NEWDIR") && !get(H "/DOCS", "X.TXT"));
    assert(!strcmp(get(H "/DOCS/DEEP", "D.TXT"), "delta") && !strcmp(get(H, "BIG.DAT"), big));   /* a file too big to be in any snapshot is left alone */
    seen = 0; assert(snap_list(cb) == 3);
    puts("ok restore: changed and deleted files back, files and folders made since removed, the snapshots themselves kept");

    go(H "/DOCS"); assert(!fat_rmdir("DEEP")); assert(fat_cwd_get()); fat_cwd_set(0);
    puts("PASS: snap.c");
    return 0;
}
'''
fails = []
with tempfile.TemporaryDirectory(prefix="jt-snap-") as t:
    img = os.path.join(t, "card.img")
    with open(img, "wb") as f: f.truncate(16 * 1024 * 1024)
    subprocess.run(["mkfs.fat", "-F", "16", img], capture_output=True, check=True)
    shim = os.path.join(t, "shim"); os.makedirs(shim)
    open(os.path.join(shim, "libc.h"), "w").write("#include <string.h>\n")
    src = os.path.join(t, "t.c"); open(src, "w").write(HOST)
    exe = os.path.join(t, "t")
    cc = subprocess.run(["clang", "-std=gnu11", "-D_GNU_SOURCE", "-g", "-fsanitize=address,undefined", "-w", "-I", shim, "-I", arch, "-I", os.path.join(root, "drivers"),
                         src, os.path.join(arch, "snap.c"), os.path.join(root, "drivers", "fat.c"), os.path.join(root, "tools", "fuzz-host", "kmalloc_host.c"), "-o", exe], capture_output=True, text=True)
    if cc.returncode: print(cc.stderr[-2000:]); sys.exit(1)
    r = subprocess.run([exe, img], capture_output=True, text=True, timeout=600)
    print(r.stdout + r.stderr[-1500:])
    ok = r.returncode == 0 and "PASS: snap.c" in r.stdout
    fsck = subprocess.run(["fsck.fat", "-n", img], capture_output=True, text=True) if shutil.which("fsck.fat") else None
    if fsck and fsck.returncode: ok = False; print("fsck.fat:", fsck.stdout[-600:])
    elif fsck: print("ok fsck.fat -n: the card is clean, no cluster lost")
    sys.exit(0 if ok else 1)
