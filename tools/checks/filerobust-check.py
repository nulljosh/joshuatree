#!/usr/bin/env python3
"""Regression coverage for docs/roadmap.md's 1.0.0 item "Nothing crashes:
bad input in every text field, long lines, empty files, missing disk, no
network." This is the file-side half: an EMPTY file, an OVERSIZED file, a
CORRUPT FAT16 image, and a FULL disk, each proven headless against the
real kernel and a real FAT16 image (dd + mkfs.vfat, the same tools/mkdisk.sh
every other FAT check uses), never a screen.

Verdicts come off two named serial markers this check adds to `cat` and
`write` in kernel/kernel.c (`cat <name>: n=<len>` / `cat <name>: not
found`, `write <name>: ok` / `write <name>: failed`), the same
"tests read a marker off the serial log" idiom files-roundtrip-check.sh
and ramfs-demo-check.sh already use, rather than scraping VGA text over
the QEMU monitor the way fatcyclehang-check.sh has to (that check predates
the marker; this one didn't need to repeat that route). A missing marker
after a generous, bounded wait is exactly what a hang looks like from the
outside, which is this script's proof that a corrupt read didn't wedge
the kernel, backed by a hard per-boot process timeout as a second net.

Four cases, two crafted images (built once, reused where the on-disk
state doesn't conflict):

  1. EMPTY (0 bytes): `write empty.txt` (no content) makes a real 0-byte
     file through the kernel's own write path, then `cat empty.txt` must
     read back exactly 0 bytes.
  2. OVERSIZED (20 KB, well past the shell's 4095-byte `cat` buffer and
     Notes' 4095-byte editor_buffer): crafted directly on the raw image
     (a real, non-corrupt, properly-chained cluster chain -- this is a
     legitimately large file, not a hostile one), `cat big.txt` must
     truncate cleanly to exactly 4095 bytes, never overflow that buffer.
  3. CORRUPT FAT16: one file whose directory entry claims a file_size far
     larger than the whole 16 MB volume AND whose one data cluster's own
     FAT entry loops back on itself (the fatcyclehang-check.sh bug was a
     *directory* cluster chain cycling; this is a *file's* data chain
     cycling with a lying size on top, a materially different path
     through fat_read_file, fair game per this check's own task scope),
     plus a root directory entry with garbage, non-ASCII name and
     attribute bytes sharing the same root directory. `cat loop.txt` must
     return some bounded length (not hang), and `ls` (which walks every
     root entry, including the garbage one) must return control to the
     shell without crashing or hanging -- proven by a known-good `cat`
     succeeding again right after it, in the same boot.
  4. FULL DISK: every cluster in the FAT marked in-use (no real free-space
     search needed to prove this -- alloc_cluster's search is over the
     exact same total_clusters this marks exhaustively). `write
     newfile.txt hello` must fail with an error, and the kernel must stay
     responsive: `write ok.txt` (0 bytes, needs no cluster) must still
     succeed right after.

Also fails, across every boot, if the serial log ever shows
"exception: ring-0" (idt.c's real ring-0-fault marker: a genuine kernel
crash, not a clean error return).
"""
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
KERNEL = os.environ.get("JT_KERNEL", "kernel.elf")

FAILURES = []


def report(ok, name, detail=""):
    if ok:
        print(f"PASS: {name}" + (f" ({detail})" if detail else ""))
    else:
        print(f"FAIL: {name}" + (f" -- {detail}" if detail else ""))
        FAILURES.append(name)
    return ok


# ---------------------------------------------------------------------------
# FAT16 image crafting (host side). Geometry is always read off the real
# boot sector, never assumed, the same discipline fatcyclehang-check.sh's
# own corruption script follows.
# ---------------------------------------------------------------------------

def read_bpb(path):
    with open(path, "rb") as f:
        boot = f.read(512)

    def rd16(o):
        return boot[o] | (boot[o + 1] << 8)

    bytes_per_sector = rd16(11)
    sectors_per_cluster = boot[13]
    reserved_sectors = rd16(14)
    num_fats = boot[16]
    root_entry_count = rd16(17)
    fat_size_sectors = rd16(22)
    assert bytes_per_sector == 512, f"unexpected sector size {bytes_per_sector}"

    fat_start = reserved_sectors
    root_dir_start = fat_start + num_fats * fat_size_sectors
    root_dir_sectors = (root_entry_count * 32 + 511) // 512
    data_start = root_dir_start + root_dir_sectors

    total_sectors_16 = rd16(19)
    total_sectors_32 = struct.unpack_from("<I", boot, 32)[0]
    total_sectors = total_sectors_16 or total_sectors_32
    total_clusters = (total_sectors - data_start) // sectors_per_cluster if total_sectors > data_start else 0
    # Mirrors drivers/fat.c's own fat_mount() clamp: a FAT16 table only has
    # room to index as many clusters as its own sectors can hold 2-byte
    # entries for, regardless of what total_sectors claims.
    max_from_fat = fat_size_sectors * (512 // 2)
    max_from_fat = max_from_fat - 2 if max_from_fat >= 2 else 0
    total_clusters = min(total_clusters, max_from_fat)

    return dict(
        sectors_per_cluster=sectors_per_cluster,
        num_fats=num_fats,
        root_entry_count=root_entry_count,
        fat_size_sectors=fat_size_sectors,
        fat_start=fat_start,
        root_dir_start=root_dir_start,
        root_dir_sectors=root_dir_sectors,
        data_start=data_start,
        total_clusters=total_clusters,
    )


class FatImage:
    """Raw sector-level access to a real FAT16 image, matching drivers/fat.c's
    own on-disk layout exactly (mirrors fatcyclehang-check.sh's corruption
    script, generalized into reusable pieces for building whole files
    instead of just flipping one FAT entry)."""

    def __init__(self, path):
        self.path = path
        self.f = open(path, "r+b")
        self.g = read_bpb(path)

    def close(self):
        self.f.close()

    def read_sector(self, lba):
        self.f.seek(lba * 512)
        return bytearray(self.f.read(512))

    def write_sector(self, lba, data):
        assert len(data) == 512
        self.f.seek(lba * 512)
        self.f.write(bytes(data))

    def cluster_to_lba(self, cluster):
        return self.g["data_start"] + (cluster - 2) * self.g["sectors_per_cluster"]

    def fat_entry_read(self, cluster):
        off = cluster * 2
        sec = self.read_sector(self.g["fat_start"] + off // 512)
        return sec[off % 512] | (sec[off % 512 + 1] << 8)

    def fat_entry_write(self, cluster, value):
        off = cluster * 2
        row = off // 512
        for k in range(self.g["num_fats"]):
            sec = self.read_sector(self.g["fat_start"] + k * self.g["fat_size_sectors"] + row)
            sec[off % 512] = value & 0xFF
            sec[off % 512 + 1] = (value >> 8) & 0xFF
            self.write_sector(self.g["fat_start"] + k * self.g["fat_size_sectors"] + row, sec)

    def find_free_clusters(self, n, avoid=frozenset()):
        found = []
        c = 2
        while len(found) < n and c < self.g["total_clusters"] + 2:
            if c not in avoid and self.fat_entry_read(c) == 0:
                found.append(c)
            c += 1
        if len(found) < n:
            raise RuntimeError(f"only found {len(found)} free clusters of {n} needed")
        return found

    def find_free_root_slots(self, n):
        slots = []
        for s in range(self.g["root_dir_sectors"]):
            lba = self.g["root_dir_start"] + s
            sec = self.read_sector(lba)
            for i in range(16):
                if sec[i * 32] in (0x00, 0xE5):
                    slots.append((lba, i))
                    if len(slots) == n:
                        return slots
        raise RuntimeError(f"only found {len(slots)} free root slots of {n} needed")

    def write_dir_entry(self, slot, name11, attr, cluster, size, reserved=b"\x00" * 8, wtime=0, wdate=0):
        lba, idx = slot
        sec = self.read_sector(lba)
        entry = struct.pack("<11sB8sHHHHI", name11, attr, reserved, 0, wtime, wdate, cluster & 0xFFFF, size)
        sec[idx * 32:(idx + 1) * 32] = entry
        self.write_sector(lba, sec)

    def mark_all_clusters_used(self):
        """The FULL-DISK case: every real cluster the volume has room to
        index (the same total_clusters fat_mount() clamps to) marked
        in-use, so alloc_cluster's free-cluster scan finds nothing no
        matter what it's asked to allocate."""
        for c in range(2, self.g["total_clusters"] + 2):
            self.fat_entry_write(c, 0xFFFF)


def to_fat_name(name):
    """Host mirror of drivers/fat.c's to_fat_name: uppercase 8.3, space-padded."""
    base, _, ext = name.partition(".")
    out = bytearray(b" " * 11)
    base_u = base.upper().encode("ascii")[:8]
    ext_u = ext.upper().encode("ascii")[:3]
    out[0:len(base_u)] = base_u
    out[8:8 + len(ext_u)] = ext_u
    return bytes(out)


def mkdisk(path):
    subprocess.run(["bash", str(ROOT / "tools/mkdisk.sh"), str(path)],
                    cwd=ROOT, check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def craft_big_and_loop_and_garbage(disk_path):
    """Adds BIG.TXT (a real, legitimate 20 KB file), LOOP.TXT (a directory
    entry that lies about its own size and whose one data cluster's FAT
    entry loops back on itself), and one root directory entry with garbage
    name/attribute bytes, onto an already-mounted, already-seeded disk
    image (empty.txt is expected to already exist on it, written by the
    kernel itself in an earlier boot)."""
    img = FatImage(disk_path)
    try:
        content = b"A" * 20480
        cluster_bytes = img.g["sectors_per_cluster"] * 512
        nclusters = (len(content) + cluster_bytes - 1) // cluster_bytes
        big_clusters = img.find_free_clusters(nclusters)
        off = 0
        for c in big_clusters:
            lba = img.cluster_to_lba(c)
            for s in range(img.g["sectors_per_cluster"]):
                chunk = content[off:off + 512]
                chunk = chunk + b"\x00" * (512 - len(chunk))
                img.write_sector(lba + s, chunk)
                off += 512
        for i in range(len(big_clusters) - 1):
            img.fat_entry_write(big_clusters[i], big_clusters[i + 1])
        img.fat_entry_write(big_clusters[-1], 0xFFFF)

        loop_cluster = img.find_free_clusters(1, avoid=frozenset(big_clusters))[0]
        lba = img.cluster_to_lba(loop_cluster)
        for s in range(img.g["sectors_per_cluster"]):
            img.write_sector(lba + s, b"L" * 512)
        img.fat_entry_write(loop_cluster, loop_cluster)  # the self-loop

        slots = img.find_free_root_slots(3)
        img.write_dir_entry(slots[0], to_fat_name("BIG.TXT"), 0x20, big_clusters[0], len(content))
        # file_size ~4.29 billion: far larger than this 16 MB volume could
        # ever hold, on top of the self-looping chain above.
        img.write_dir_entry(slots[1], to_fat_name("LOOP.TXT"), 0x20, loop_cluster, 0xFFFFFFF0)
        # Garbage root entry: name[0] deliberately not 0x00/0xE5 (both have
        # real meaning -- end-of-directory / deleted -- so using either
        # would stop find_entry_in/fat_list's scan rather than exercise
        # it), attr with neither ATTR_VOLUME_ID nor ATTR_DIRECTORY set (so
        # fat_list doesn't just skip it), and garbage everywhere else a
        # real corrupted or hostile image could plausibly hold.
        garbage_name = bytes([0xF0, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A])
        img.write_dir_entry(slots[2], garbage_name, 0x27, 0xDEAD, 0xCAFEBABE,
                             reserved=bytes([0x11] * 8), wtime=0xFFFF, wdate=0xFFFF)
    finally:
        img.close()


def craft_full_disk(disk_path):
    img = FatImage(disk_path)
    try:
        img.mark_all_clusters_used()
    finally:
        img.close()


# ---------------------------------------------------------------------------
# Driving the real kernel headlessly. Same shape as fatcyclehang-check.sh /
# shellregress-check.sh: -display none, -monitor stdio for sendkey/quit,
# -serial file:... for the verdicts. Esc first to leave the GUI desktop
# and reach the text shell (kmain calls gui_run() before ever reaching the
# shell loop).
# ---------------------------------------------------------------------------

_KEY_MAP = {" ": "spc", ".": "dot", "-": "minus", "/": "slash"}


def _send_keys(proc, line):
    for ch in line:
        proc.stdin.write(f"sendkey {_KEY_MAP.get(ch, ch)}\n")
    proc.stdin.write("sendkey ret\n")
    proc.stdin.flush()


def boot_and_run(disk_path, commands, workdir, boot_name, per_cmd_sleep=2, hang_timeout=25):
    """Boots `kernel.elf` against `disk_path`, types each string in
    `commands` into the shell (Enter after each), then quits. Returns the
    serial log text, or None if QEMU had to be killed after `hang_timeout`
    seconds past sending quit -- the hard, discriminating proof a corrupt
    read wedged the kernel rather than just running long."""
    serial_path = os.path.join(workdir, f"serial-{boot_name}.log")
    proc = subprocess.Popen(
        ["qemu-system-i386", "-kernel", KERNEL, "-display", "none", "-monitor", "stdio",
         "-serial", f"file:{serial_path}",
         "-drive", f"file={disk_path},format=raw,if=ide,index=0"],
        cwd=ROOT, stdin=subprocess.PIPE, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, text=True,
    )
    try:
        time.sleep(3)
        proc.stdin.write("sendkey esc\n")
        proc.stdin.flush()
        time.sleep(1)
        for cmd in commands:
            _send_keys(proc, cmd)
            time.sleep(per_cmd_sleep)
        proc.stdin.write("quit\n")
        proc.stdin.flush()
        try:
            proc.wait(timeout=hang_timeout)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait(timeout=10)
            return None
    finally:
        if proc.poll() is None:
            proc.kill()
            try:
                proc.wait(timeout=10)
            except subprocess.TimeoutExpired:
                pass

    try:
        return open(serial_path).read()
    except OSError:
        return ""


def check_no_crash(log, boot_name):
    if log is not None and "exception: ring-0" in log:
        line = next((l for l in log.splitlines() if "exception: ring-0" in l), "")
        report(False, f"{boot_name}: no ring-0 kernel crash", line)
        return False
    return True


def main():
    workdir = tempfile.mkdtemp(prefix="jt-filerobust-")
    try:
        if KERNEL == "kernel.elf":
            subprocess.run(["make", "-s", "kernel.elf"], cwd=ROOT, check=True)

        # ---- Cases 1-3: EMPTY, OVERSIZED, CORRUPT (shared disk image) ----
        robust_img = os.path.join(workdir, "robust.img")
        mkdisk(robust_img)

        # Phase A: create empty.txt for real, through the kernel's own
        # write path (arg with no trailing content -> strlen(content)==0),
        # not hand-forged on the host -- the same "create it for real, then
        # corrupt around it" split fatcyclehang-check.sh uses for its own
        # subdirectory.
        setup_log = boot_and_run(robust_img, ["write empty.txt"], workdir, "setup")
        check_no_crash(setup_log, "setup boot")
        if setup_log is None or "write empty.txt: ok" not in setup_log:
            report(False, "setup: write empty.txt", "kernel never confirmed the 0-byte write; can't proceed")
            return finish(workdir)

        # Phase B: craft the oversized file and the corrupted entries
        # directly on the same image.
        craft_big_and_loop_and_garbage(robust_img)

        # Phase C: read them all back in one boot.
        read_log = boot_and_run(
            robust_img,
            ["cat empty.txt", "cat big.txt", "cat loop.txt", "ls", "cat empty.txt"],
            workdir, "read", per_cmd_sleep=3, hang_timeout=30,
        )
        crash_free = check_no_crash(read_log, "read boot")

        if read_log is None:
            report(False, "empty file (0 bytes) reads cleanly via cat", "QEMU had to be killed (hang)")
            report(False, "oversized file truncates to the 4095-byte cat buffer", "QEMU had to be killed (hang)")
            report(False, "corrupt file (lying size + self-looping FAT chain) doesn't hang cat", "QEMU had to be killed (hang)")
            report(False, "garbage root directory entry doesn't crash/hang ls", "QEMU had to be killed (hang)")
        else:
            cats = [l for l in read_log.splitlines() if l.startswith("cat ")]
            empty_hits = [l for l in cats if l.startswith("cat empty.txt: ")]
            big_hits = [l for l in cats if l.startswith("cat big.txt: ")]
            loop_hits = [l for l in cats if l.startswith("cat loop.txt: ")]

            report(bool(empty_hits) and empty_hits[0] == "cat empty.txt: n=0",
                   "empty file (0 bytes) reads cleanly via cat",
                   empty_hits[0] if empty_hits else "no 'cat empty.txt' marker in serial log")

            report(bool(big_hits) and big_hits[0] == "cat big.txt: n=4095",
                   "oversized (20KB) file truncates to the 4095-byte cat buffer, no overflow",
                   big_hits[0] if big_hits else "no 'cat big.txt' marker in serial log")

            # Any bounded marker at all (a length, or "not found") proves
            # fat_read_file's bufsize clamp kept the self-looping chain and
            # the multi-gigabyte lying size from ever hanging the read; only
            # a boot that never got here (the read_log is None branch above)
            # would fail this.
            report(bool(loop_hits),
                   "corrupt file (file_size far exceeding the volume + a self-looping FAT chain) fails/reads cleanly, no hang",
                   loop_hits[0] if loop_hits else "no 'cat loop.txt' marker in serial log at all")

            # The real proof `ls` survived the garbage-attribute root entry:
            # a second, independent cat after it still produced a fresh,
            # correct marker -- the shell kept dispatching commands.
            report(len(empty_hits) >= 2 and empty_hits[-1] == "cat empty.txt: n=0",
                   "garbage-attribute root directory entry doesn't crash/hang ls; shell stays responsive after",
                   f"{len(empty_hits)} 'cat empty.txt' markers seen (need 2, before and after ls)")

        # ---- Case 4: FULL DISK ----
        full_img = os.path.join(workdir, "full.img")
        mkdisk(full_img)
        craft_full_disk(full_img)

        full_log = boot_and_run(full_img, ["write newfile.txt hello", "write ok.txt"], workdir, "full")
        check_no_crash(full_log, "full-disk boot")

        if full_log is None:
            report(False, "full disk: write fails cleanly", "QEMU had to be killed (hang)")
            report(False, "full disk: kernel stays responsive after a failed write", "QEMU had to be killed (hang)")
        else:
            report("write newfile.txt: failed" in full_log,
                   "full disk (no free clusters): write of a new file fails with an error",
                   "expected 'write newfile.txt: failed' marker not found" if "write newfile.txt: failed" not in full_log else "")
            report("write ok.txt: ok" in full_log,
                   "full disk: kernel stays responsive after a failed write (a real 0-byte write still succeeds)",
                   "expected 'write ok.txt: ok' marker not found" if "write ok.txt: ok" not in full_log else "")

        return finish(workdir)
    except Exception:
        shutil.rmtree(workdir, ignore_errors=True)
        raise


def finish(workdir):
    shutil.rmtree(workdir, ignore_errors=True)
    print()
    if FAILURES:
        print(f"FAIL: {len(FAILURES)} of the file-robustness checks failed:")
        for name in FAILURES:
            print(f"  - {name}")
        return 1
    print("PASS: empty files, oversized files, corrupt FAT16 images and a full disk all fail cleanly, no crash, no hang")
    return 0


if __name__ == "__main__":
    sys.exit(main())
