#!/usr/bin/env python3
"""FAT crash safety (drivers/fat.c; 4.13.0): pull the power at every sector write, never lose a file or corrupt the card.

Host only: the real fat.c under address and undefined-behaviour sanitizers over FAT16 and FAT32 images made by mkfs.fat. For
each of replace-with-more, replace-with-less, delete, mkdir and rmdir, the run is cut after the 0th, 1st, 2nd ... sector write
(every later write is dropped, as if the power went), then fsck.fat -n must report nothing worse than clusters used but
reachable from no entry or FAT copies that differ, the card is mounted again (which frees those clusters, syncs the copies and
fixes FSInfo's free count), fsck.fat -n must then find it fully clean, and the file must hold exactly the old content or exactly
the new one, never a mix; a folder is whole or absent. Then two mutations must FAIL this same check: the same fat.c with the directory entry written
right after the first cluster is allocated, before the data and the chain (the wrong order), and one with the mount scan off.
Skips (exit 0) where clang, mkfs.fat or fsck.fat is missing.
"""
import os, re, shutil, subprocess, sys, tempfile

root = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
drivers = os.path.join(root, "drivers")
if not (shutil.which("clang") and shutil.which("mkfs.fat") and shutil.which("fsck.fat")):
    print("SKIP: clang, mkfs.fat or fsck.fat not installed"); sys.exit(0)

HOST = r'''
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "fat.h"
void *kmalloc(unsigned int n) { return malloc(n); }   /* fat.c's scan buffers come from the heap (4.14.0) */
FILE *img; static unsigned limit = 0xFFFFFFFF, nwrites;
struct vfs_ops;
void vfs_register(const char *n, const struct vfs_ops *o) { (void)n; (void)o; }
int blockdev_read_sector(unsigned lba, void *b) { fseek(img, (long)lba * 512, SEEK_SET); return fread(b, 512, 1, img) == 1; }
int blockdev_write_sector(unsigned lba, const void *b) {   /* a write past the cut never reaches the card */
    if (nwrites++ >= limit) return 0;
    fseek(img, (long)lba * 512, SEEK_SET); return fwrite(b, 512, 1, img) == 1;
}
static char oldb[3000], newb[4000], smallb[100], rd[9000];
static void fill(void) { memset(oldb, 'o', sizeof oldb); memset(newb, 'n', sizeof newb); memset(smallb, 's', sizeof smallb); }
static int is(const char *name, const char *want, unsigned n) { int r = fat_read_file(name, rd, sizeof rd); return r == (int)n && !memcmp(rd, want, n); }
int main(int argc, char **argv) {
    fill(); img = fopen(argv[1], "r+b"); if (!img) return 2;
    if (!fat_mount()) { puts("MOUNT FAIL"); return 2; }
    const char *mode = argv[2], *op = argv[3];
    if (!strcmp(mode, "prep")) {
        if (!fat_replace_file("OLD.TXT", oldb, sizeof oldb) || !fat_replace_file("DEL.TXT", oldb, sizeof oldb) || !fat_mkdir("EMPTY")) return 3;
        return 0;
    }
    if (!strcmp(mode, "run")) {
        limit = (unsigned)atoi(argv[4]); nwrites = 0;
        if (!strcmp(op, "more")) fat_replace_file("OLD.TXT", newb, sizeof newb);
        else if (!strcmp(op, "less")) fat_replace_file("OLD.TXT", smallb, sizeof smallb);
        else if (!strcmp(op, "delete")) fat_delete("DEL.TXT");
        else if (!strcmp(op, "mkdir")) fat_mkdir("NEWD");
        else if (!strcmp(op, "rmdir")) fat_rmdir("EMPTY");
        printf("%u\n", nwrites); return 0;
    }
    /* verify: the card was mounted above, so the recovery scan has run */
    if (!strcmp(op, "more") || !strcmp(op, "less")) {
        int old = is("OLD.TXT", oldb, sizeof oldb), nw = !strcmp(op, "more") ? is("OLD.TXT", newb, sizeof newb) : is("OLD.TXT", smallb, sizeof smallb);
        puts(old ? "old" : nw ? "new" : "MIX");
    } else if (!strcmp(op, "delete")) puts(is("DEL.TXT", oldb, sizeof oldb) ? "old" : fat_read_file("DEL.TXT", rd, sizeof rd) < 0 ? "new" : "MIX");
    else if (!strcmp(op, "mkdir")) puts(fat_chdir("NEWD") && fat_chdir("..") ? "new" : "old");
    else if (!strcmp(op, "rmdir")) puts(fat_chdir("EMPTY") && fat_chdir("..") ? "old" : "new");
    /* the card must still take a write and the old file must still be intact after a repair */
    if (!fat_replace_file("AFTER.TXT", newb, sizeof newb) || !is("AFTER.TXT", newb, sizeof newb)) { puts("AFTER FAIL"); return 4; }
    return 0;
}
'''

# what a cut may leave behind that the mount scan then repairs: lost clusters and unequal FAT copies, nothing else
OK_FSCK = [r"^fsck\.fat ", r"^Free cluster summary", r"^Orphaned cluster", r"^Lost cluster", r"^Reclaim", r"differ", r"^\s+Auto-", r"^\s*$",
           r"^\s*Copy", r"^\s*\d+ (lost|orphaned)", r"^Perform changes", r"^\s*Use FAT", r"^\s*1\) ", r"^\s*2\) ", r"^\s*Which", r"^\*", r"^\S+: \d+ files",
           r"^\s*Using first FAT", r"^Leaving filesystem", r"^Starting check", r"^There are differences", r"lost clusters", r"^\s*Free cluster"]

def sh(*a, **k): return subprocess.run(list(a), capture_output=True, text=True, **k)

def build(t, name, fat_c):
    shim = os.path.join(t, "shim"); os.makedirs(shim, exist_ok=True)
    open(os.path.join(shim, "libc.h"), "w").write("#include <string.h>\n")
    src = os.path.join(t, name + ".c"); open(src, "w").write(HOST)
    exe = os.path.join(t, name)
    cc = sh("clang", "-std=gnu11", "-D_GNU_SOURCE", "-g", "-fsanitize=address,undefined", "-fno-sanitize-recover=undefined", "-w", "-I", shim, "-I", drivers, src, fat_c, "-o", exe)
    if cc.returncode: print(cc.stderr[-2000:]); sys.exit(1)
    return exe

def mutant(t, name, src):
    mdir = os.path.join(t, name + "-src"); os.makedirs(mdir)
    for h in ("fat.h", "blockdev.h", "vfs.h"): shutil.copy(os.path.join(drivers, h), mdir)
    open(os.path.join(mdir, "fat.c"), "w").write(src)
    return build(t, name, os.path.join(mdir, "fat.c"))

def sweep(t, exe, fatbits, ops):
    """Returns (cuts, failures, repairs) for one binary on one FAT type."""
    base = os.path.join(t, "base%d.img" % fatbits)
    with open(base, "wb") as f: f.truncate(4 * 1024 * 1024)
    assert sh("mkfs.fat", "-F", str(fatbits), "-s", "1", base).returncode == 0
    r = sh(exe, base, "prep", "-"); assert r.returncode == 0, r.stdout + r.stderr[-800:]
    cuts = repairs = 0; fails = []
    for op in ops:
        for cut in range(0, 400):
            work = os.path.join(t, "work.img"); shutil.copyfile(base, work)
            r = sh(exe, work, "run", op, str(cut)); n = int(r.stdout.split()[-1]) if r.returncode == 0 and r.stdout.split() else -1
            if n < 0: fails.append("FAT%d %s cut %d: run crashed: %s" % (fatbits, op, cut, r.stderr[-600:])); break
            cuts += 1
            pre = sh("fsck.fat", "-n", work)
            bad = [l for l in pre.stdout.splitlines() if not any(re.search(p, l) for p in OK_FSCK)]
            if bad: fails.append("FAT%d %s cut %d: fsck before repair: %s" % (fatbits, op, cut, " | ".join(bad)[:300]))
            if pre.returncode: repairs += 1
            v = sh(exe, work, "verify", op)
            state = v.stdout.split()[0] if v.stdout.split() else "?"
            if v.returncode or state not in ("old", "new"): fails.append("FAT%d %s cut %d: after mount %s %s" % (fatbits, op, cut, state, v.stderr[-300:]))
            post = sh("fsck.fat", "-n", work)
            if post.returncode: fails.append("FAT%d %s cut %d: fsck after repair: %s" % (fatbits, op, cut, post.stdout[-300:].replace("\n", " | ")))
            if n <= cut:   # the operation finished before the cut: every cut point has been tried
                if state != ("new"): fails.append("FAT%d %s uncut ended in state %s" % (fatbits, op, state))
                break
        else: fails.append("FAT%d %s never finished within 400 writes" % (fatbits, op))
        if fails and len(fails) > 12: break
    return cuts, fails, repairs

OPS = ["more", "less", "delete", "mkdir", "rmdir"]
ok = True
with tempfile.TemporaryDirectory(prefix="jt-fatcrash-") as t:
    exe = build(t, "real", os.path.join(drivers, "fat.c"))
    total = 0; rep = 0
    for bits in (16, 32):
        c, f, r = sweep(t, exe, bits, OPS); total += c; rep += r
        print("FAT%d: %d power cuts across replace (more, less), delete, mkdir, rmdir; %d left the card needing the mount repair" % (bits, c, r))
        for line in f: ok = False; print("FAIL", line)
    if ok: print("ok every cut: fsck.fat -n clean after the mount scan, the file old or new and never a mix (%d cuts, %d repaired)" % (total, rep))

    # mutation: the entry written before the data and the chain must be caught
    mut = open(os.path.join(drivers, "fat.c")).read()
    hook = "        prev_cluster = c;\n"
    assert mut.count(hook) == 1
    mut = mut.replace(hook, hook + """        if (c == first_cluster) { u8 ms[512]; blockdev_read_sector(slot_lba, ms); struct dir_entry *m = &((struct dir_entry *)ms)[slot_idx];
            to_fat_name(name, m->name); m->attr = 0; set_cluster(m, c); m->file_size = len; blockdev_write_sector(slot_lba, ms); }\n""")
    c, f, r = sweep(t, mutant(t, "mutant", mut), 16, ["more", "less"])
    if f: print("ok mutation (entry before data) fails the check: %d findings over %d cuts, first: %s" % (len(f), c, f[0][:140]))
    else: ok = False; print("FAIL mutation (entry before data) was NOT caught")

    # mutation: the mount scan switched off must be caught too, or the repair half of the claim is untested
    off = open(os.path.join(drivers, "fat.c")).read()
    hook = "    mounted = 1;\n    fat_recover();\n"
    assert off.count(hook) == 1
    c, f, r = sweep(t, mutant(t, "noscan", off.replace(hook, "    mounted = 1;\n")), 16, ["more", "delete", "mkdir"])
    if f: print("ok mutation (no mount scan) fails the check: %d findings over %d cuts, first: %s" % (len(f), c, f[0][:140]))
    else: ok = False; print("FAIL mutation (no mount scan) was NOT caught")
print("PASS: fat-crash" if ok else "FAIL: fat-crash")
sys.exit(0 if ok else 1)
