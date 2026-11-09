#!/usr/bin/env python3
"""A disk kept from an older kernel gets this kernel's app (13.6.0, kernel/ring3app.c ring3app_seed).

Boots with a fresh FAT16 image and open=keyrate, which seeds KEYRATE.BIN. Boots again unchanged: no replace line. Then flips bytes of
the file on the host, as an older kernel's copy would differ, and boots a third time: the serial says it replaced the stale copy
and the bytes on disk are this kernel's again. Discriminating: without the compare the changed bytes stay and no line appears.
Skips (exit 0) when mkfs.vfat is missing.
"""
import os, shutil, struct, subprocess, sys, tempfile, time

ROOT = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
os.chdir(ROOT)
if not (shutil.which("mkfs.vfat") or shutil.which("mkfs.fat")):
    print("SKIP: mkfs.vfat not installed"); sys.exit(0)
subprocess.run(["make", "-s", "kernel.elf"], check=True)
tmp = tempfile.mkdtemp()
disk, log = os.path.join(tmp, "disk.img"), os.path.join(tmp, "serial")
subprocess.run(["./tools/mkdisk.sh", disk], check=True, stdout=subprocess.DEVNULL)

def boot():
    open(log, "w").close()
    q = subprocess.Popen(["qemu-system-i386", "-kernel", "kernel.elf", "-append", "open=keyrate", "-display", "none", "-vga", "std",
                          "-drive", f"file={disk},format=raw,if=ide,index=0", "-serial", f"file:{log}"],
                         stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    out = ""
    for _ in range(600):
        time.sleep(0.1)
        out = open(log, errors="replace").read()
        if "at ring 3" in out: break
    time.sleep(3)
    q.kill(); q.wait()
    return open(log, errors="replace").read()

def locate():
    """(bytes of KEYRATE.BIN, its file offset of cluster one) by walking the FAT16 root directory and cluster chain."""
    d = open(disk, "rb").read()
    bps, spc, res, nfat, nroot, _, _, spf = struct.unpack_from("<HBHBHHBH", d, 11)
    fat, root = res * bps, (res + nfat * spf) * bps
    data = root + nroot * 32
    for i in range(nroot):
        e = d[root + i * 32:root + (i + 1) * 32]
        if e[:11] == b"KEYRATE BIN":
            c, size, out, first = struct.unpack_from("<H", e, 26)[0], struct.unpack_from("<I", e, 28)[0], b"", None
            while 2 <= c < 0xFFF8:
                at = data + (c - 2) * spc * bps
                first = first if first is not None else at
                out += d[at:at + spc * bps]
                c = struct.unpack_from("<H", d, fat + c * 2)[0]
            return out[:size], first
    return None

fails = []
first = boot()
if "at ring 3" not in first: sys.exit(f"FAIL: Keyrate never launched: {first[-400:]!r}")
at = locate()
if not at: sys.exit("FAIL: KEYRATE.BIN was not seeded on the disk")
good, off = at
if "replacing stale" in boot(): fails.append("an unchanged copy was replaced")
with open(disk, "r+b") as f:
    f.seek(off + 64); f.write(bytes(b ^ 0xFF for b in good[64:96]))
third = boot()
if "replacing stale KEYRATE.BIN" not in third: fails.append(f"the changed copy was not replaced: {third[-400:]!r}")
at = locate()
if not at or at[0] != good: fails.append(f"the bytes on disk are not this kernel's after the replace: {len(at[0]) if at else None} against {len(good)}")
shutil.rmtree(tmp, ignore_errors=True)
if fails:
    for x in fails: print("FAIL:", x)
    sys.exit(1)
print("PASS: a changed KEYRATE.BIN is replaced at launch and an unchanged one is left alone")
