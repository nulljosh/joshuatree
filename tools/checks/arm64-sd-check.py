#!/usr/bin/env python3
"""ARM64 M4 SD card: the Pi image reads a FAT32 card through EMMC2, at the board's own address, and `ls` and `cat` work.

QEMU's raspi4b models EMMC2 (0xFE340000, the host the Pi 4's card slot is wired to) as an SDHCI host. `-drive if=sd`
does not reach it: that card goes to the GPIO-routed Arasan host at 0xFE300000, which on a real Pi is the Wi-Fi chip's.
So the card goes in with `-device sd-card,bus=/generic-sdhci/sd-bus`, the first SDHCI on the bus, which is EMMC2 in
QEMU 11 (if a later QEMU orders them the other way, the boot says `sd no card` and this check fails, it cannot pass on
the wrong host). The card is a sparse 4 GiB file this script writes itself, no mtools: a power of two (QEMU insists) and
over 2 GiB, so QEMU's card is SDHC and block addressed like Joshua's. Its MBR holds one 64 MiB FAT32 partition at block
2048 with one-sector clusters, so every file and the root directory are real FAT32 chains: config.txt (a copy of
tools/pi-config.txt), BIG.TXT (10 KB, its clusters interleaved with FILLER.BIN's), a root two clusters long (ZLAST.TXT
sits in the second), a long-name entry, a deleted entry, a volume label, a directory, and BAD.TXT whose first cluster
lies just past the partition's end. Nothing from a real card is ever read or printed.

Three boots. The sdtest build (`make -C arch/arm64 sdtest`) runs the Terminal's own commands at boot: `ls` must list
every file with its size (and no label, long-name or deleted entry), `cat config.txt` must print the file,
`cat BIG.TXT` exactly its first 4096 bytes, `cat BAD.TXT` must be refused at the partition's end, and a raw read past
the card's last block must be refused. The real kernel8.img with the card must mount it (`sd card sdhc 4096 MB`,
`sd fat32 partition at block 2048, 64 MB, read only`), and with no card must print `sd no card` and carry on to the
Wi-Fi step, the desktop already up. Skips (exit 0) without clang, ld.lld, qemu-system-aarch64 or its raspi4b model.
Usage: tools/checks/arm64-sd-check.py   (from the repo root)
"""
import os, re, shutil, struct, subprocess, sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from scratch import scratch_dir

root = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
arch = os.path.join(root, "arch/arm64")
if not all(shutil.which(t) for t in ("clang", "ld.lld", "qemu-system-aarch64")):
    print("SKIP: clang, ld.lld or qemu-system-aarch64 not installed"); sys.exit(0)
if "raspi4b" not in subprocess.run(["qemu-system-aarch64", "-machine", "help"], capture_output=True, text=True).stdout:
    print("SKIP: QEMU here has no raspi4b model (needs QEMU 9 or newer)"); sys.exit(0)
TMP = scratch_dir("sd")
fails = []

# ---- The card: MBR, one FAT32 partition, written sector by sector into a sparse file ----
CARD = 4 << 30                      # blocks 0 .. 8388607
PART, PSECT = 2048, 131072          # 64 MiB at block 2048
RES, NFATS, SPC = 32, 2, 1
FATSZ = -(-((PSECT - RES) // SPC + 2) * 4 // 512)       # sectors per FAT, rounded up
DATA = RES + NFATS * FATSZ                               # first data sector, inside the partition
CLUSTERS = (PSECT - DATA) // SPC                         # 128992: a real FAT32 needs 65525 or more
assert CLUSTERS >= 65525
BAD_CLUSTER = CLUSTERS + 2 + 10                          # just past the last data cluster
BAD_BLOCK = DATA + (BAD_CLUSTER - 2) * SPC               # the partition sector fat.c will ask for
assert BAD_BLOCK >= PSECT
CONFIG = open(os.path.join(root, "tools/pi-config.txt"), "rb").read()
BIG = b"".join(b"big %04d " % i + b"." * 22 + b"\n" for i in range(1, 321))   # 320 lines of 32 bytes: 10240 bytes
assert len(BIG) == 10240

fat = {0: 0x0FFFFFF8, 1: 0x0FFFFFFF}
clusters = {}                                            # cluster -> its 512 bytes
next_free = [3]                                          # cluster 2 is the root's first

def chain(cl):
    for a, b in zip(cl, cl[1:]): fat[a] = b
    fat[cl[-1]] = 0x0FFFFFFF

def alloc(n):
    out = list(range(next_free[0], next_free[0] + n)); next_free[0] += n; return out

def put(data, cl):
    for i, c in enumerate(cl): clusters[c] = data[i * 512:(i + 1) * 512].ljust(512, b"\0")
    chain(cl)

def entry(name83, attr, cluster, size):
    assert len(name83) == 11, name83
    return struct.pack("<11sBBBHHHHHHHI", name83, attr, 0, 0, 0, 0, 0, cluster >> 16, 0, 0, cluster & 0xFFFF, size)

def lfn(long, short83):
    s = 0
    for ch in short83: s = (((s & 1) << 7) + (s >> 1) + ch) & 0xFF
    u = long.encode("utf-16-le") + b"\0\0"
    u = u.ljust(-(-len(u) // 26) * 26, b"\xff")
    parts = [u[i:i + 26] for i in range(0, len(u), 26)]
    out = []
    for n, p in enumerate(parts, 1):
        o = n | (0x40 if n == len(parts) else 0)
        out.append(struct.pack("<B10sBBB12sH4s", o, p[:10], 0x0F, 0, s, p[10:22], 0, p[22:26]))
    return b"".join(reversed(out))

files = []                                               # (8.3 name as ls prints it, size)
root_dir = [entry(b"JTCHECK    ", 0x08, 0, 0)]           # the volume label: never listed
c = alloc(1); put(CONFIG, c); root_dir.append(entry(b"CONFIG  TXT", 0x20, c[0], len(CONFIG))); files.append(("CONFIG.TXT", len(CONFIG)))
dtb = b"\xd0\x0d\xfe\xed" + bytes(96)
c = alloc(1); put(dtb, c); root_dir.append(lfn("bcm2711-rpi-4-b.dtb", b"BCM271~1DTB") + entry(b"BCM271~1DTB", 0x20, c[0], len(dtb)))
files.append(("BCM271~1.DTB", len(dtb)))
d = alloc(1)
put(entry(b".          ", 0x10, d[0], 0) + entry(b"..         ", 0x10, 0, 0), d)
root_dir.append(entry(b"OVERLAYS   ", 0x10, d[0], 0))
both = alloc(40); big, filler = both[0::2], both[1::2]   # interleaved: BIG.TXT's chain skips a cluster every step
put(BIG, big); root_dir.append(entry(b"BIG     TXT", 0x20, big[0], len(BIG))); files.append(("BIG.TXT", len(BIG)))
put(bytes(range(256)) * 40, filler); root_dir.append(entry(b"FILLER  BIN", 0x20, filler[0], 10240)); files.append(("FILLER.BIN", 10240))
root_dir.append(entry(b"BAD     TXT", 0x20, BAD_CLUSTER, 1000)); files.append(("BAD.TXT", 1000))
root_dir.append(b"\xe5" + entry(b"GONE    TXT", 0x20, 0, 5)[1:])   # deleted: never listed
for i in range(1, 10):
    t = b"file %d\n" % i
    c = alloc(1); put(t, c); root_dir.append(entry(b"FILE%02d  TXT" % i, 0x20, c[0], len(t))); files.append(("FILE%02d.TXT" % i, len(t)))
t = b"the last entry, in the root's second cluster\n"
c = alloc(1); put(t, c); root_dir.append(entry(b"ZLAST   TXT", 0x20, c[0], len(t))); files.append(("ZLAST.TXT", len(t)))
rd = b"".join(root_dir)
assert 512 < len(rd) <= 1024, len(rd)                    # two root clusters
put(rd, [2] + alloc(1))

bpb = bytearray(512)
bpb[0:3] = b"\xeb\x58\x90"; bpb[3:11] = b"JTCHECK "
struct.pack_into("<HBHBHHBHHHII", bpb, 11, 512, SPC, RES, NFATS, 0, 0, 0xF8, 0, 63, 255, PART, PSECT)
struct.pack_into("<IHHIHH", bpb, 36, FATSZ, 0, 0, 2, 1, 6)
struct.pack_into("<BBBI11s8s", bpb, 64, 0x80, 0, 0x29, 0x12345678, b"JTCHECK    ", b"FAT32   ")
bpb[510:512] = b"\x55\xaa"
fsinfo = bytearray(512)
struct.pack_into("<I", fsinfo, 0, 0x41615252); struct.pack_into("<III", fsinfo, 484, 0x61417272, 0xFFFFFFFF, 0xFFFFFFFF)
struct.pack_into("<I", fsinfo, 508, 0xAA550000)
mbr = bytearray(512)
struct.pack_into("<B3sB3sII", mbr, 446, 0, b"\xfe\xff\xff", 0x0C, b"\xfe\xff\xff", PART, PSECT)
mbr[510:512] = b"\x55\xaa"
fatbytes = bytearray(next_free[0] * 4)
for k, v in fat.items(): struct.pack_into("<I", fatbytes, k * 4, v)

card = os.path.join(TMP, "card.img")
with open(card, "wb") as f:
    f.truncate(CARD)                                     # sparse: only the sectors below take space
    def at(sector, b): f.seek(sector * 512); f.write(b)
    at(0, mbr)
    at(PART, bpb); at(PART + 1, fsinfo); at(PART + 6, bpb); at(PART + 7, fsinfo)
    for n in range(NFATS): at(PART + RES + n * FATSZ, fatbytes)
    for k, b in clusters.items(): at(PART + DATA + (k - 2) * SPC, b)

# ---- Build and boot ----
def make(*targets):
    try: return subprocess.run(["make", "-C", arch, *targets], capture_output=True, timeout=900).returncode == 0
    except subprocess.TimeoutExpired: return False

def boot(img, with_card, done):
    log = os.path.join(TMP, "uart-%d.log" % time.monotonic_ns())
    cmd = ["qemu-system-aarch64", "-machine", "raspi4b", "-display", "none", "-serial", "file:" + log, "-kernel", img]
    if with_card: cmd += ["-drive", f"if=none,id=card,file={card},format=raw", "-device", "sd-card,drive=card,bus=/generic-sdhci/sd-bus"]
    q = subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
    out = ""
    try:
        for _ in range(600):                             # 60 s cap: a hung boot never prints its last line
            time.sleep(0.1)
            out = open(log, errors="replace").read() if os.path.exists(log) else ""
            if done in out or q.poll() is not None: break
        time.sleep(0.3)
        out = open(log, errors="replace").read() if os.path.exists(log) else out
    finally:
        q.kill()
        err = q.communicate()[1].decode(errors="replace")
    if q.returncode not in (None, 0, -9) and not out: fails.append(f"QEMU would not start: {err.strip()[-300:]}")
    return out

def in_order(name, out, want):
    at = -1
    for w in want:
        i = out.find(w, at + 1)
        if i < 0: fails.append(f"{name}: no {w!r} after position {at}; the log ends {out[-500:]!r}"); return False
        at = i
    return True

def section(out, start, end):
    i = out.find(start); j = out.find(end, i + 1) if i >= 0 else -1
    return out[i + len(start):j] if i >= 0 and j >= 0 else ""

subprocess.run(["make", "-C", arch, "clean"], capture_output=True)
try:
    if not make("pi") or not make("sdtest"):
        fails.append("arch/arm64 `make pi` or `make sdtest` does not build")
    else:
        img, test_img = os.path.join(arch, "kernel8.img"), os.path.join(arch, "sd-kernel8.img")

        # 1. The sdtest build: ls, cat, and the two reads that must be refused.
        out = boot(test_img, True, "sdtest done")
        if in_order("sdtest", out, ["sd card sdhc 4096 MB", "sd fat32 partition at block 2048, 64 MB, read only", "sdtest> ls",
                                    "sdtest> cat config.txt", "sdtest> cat BIG.TXT", "sdtest> cat BAD.TXT",
                                    f"sd refused block {BAD_BLOCK}, past the end of the partition", "sdtest> cat NOPE.TXT",
                                    "cat: no file NOPE.TXT", "sdtest> read past the card",
                                    f"sd refused block {CARD // 512}, past the end of the card", "sdtest done"]):
            ls = section(out, "sdtest> ls\n", "sdtest> cat config.txt").splitlines()
            listed = [l.split() for l in ls if l.strip()]
            want = [[n, str(s)] for n, s in files] + [["OVERLAYS/"]]
            for w in want:
                if w not in listed: fails.append(f"ls: no line {' '.join(w)!r}; it printed {ls!r}")
            extra = [l for l in listed if l not in want]
            if extra: fails.append(f"ls: printed what is not a file: {extra!r} (a volume label, long-name or deleted entry?)")
            if not fails: print(f"  ok: ls lists all {len(want)} root entries with their sizes, through both root clusters (ZLAST.TXT), and nothing else")
            cat = section(out, "sdtest> cat config.txt\n", "sdtest> cat BIG.TXT").splitlines()
            if cat != CONFIG.decode().splitlines(): fails.append(f"cat config.txt printed {cat!r}, not tools/pi-config.txt")
            else: print(f"  ok: cat config.txt prints tools/pi-config.txt line for line ({len(cat)} lines)")
            big = section(out, "sdtest> cat BIG.TXT\n", "sdtest> cat BAD.TXT").splitlines()
            want_big = BIG[:4096].decode().splitlines() + ["cat: the first 4096 bytes"]
            if big != want_big: fails.append(f"cat BIG.TXT: wanted its first 128 lines and the cut note, got {len(big)} lines, ending {big[-3:]!r}")
            else: print("  ok: cat BIG.TXT prints exactly its first 4096 bytes (8 clusters of an interleaved FAT32 chain), then says so")
            bad = section(out, "sdtest> cat BAD.TXT\n", "sdtest> cat NOPE.TXT")
            if bad.strip() != f"sd refused block {BAD_BLOCK}, past the end of the partition":
                fails.append(f"cat BAD.TXT should print only the refusal, got {bad!r}")
            else: print(f"  ok: a file whose cluster lies past the partition is refused at partition block {BAD_BLOCK}, nothing printed")
            print(f"  ok: a raw read of card block {CARD // 512} (one past the last) is refused")
        if "sd FAIL" in out: fails.append("sdtest: an `sd FAIL` line: " + next(l for l in out.splitlines() if "sd FAIL" in l))

        # 2. The real image with the card: it mounts, prints no test lines, and the boot carries on.
        out = boot(img, True, "wifi FAIL cmd5")
        if in_order("kernel8.img with a card", out, ["M1c fb ok", "sd card sdhc 4096 MB, base clock",
                                                     "sd fat32 partition at block 2048, 64 MB, read only", "wifi FAIL cmd5"]):
            if "sdtest" in out or "sd refused" in out or "sd FAIL" in out: fails.append("kernel8.img with a card printed a test, refusal or FAIL line")
            else: print("  ok: kernel8.img finds the card on EMMC2 (0xFE340000), mounts the FAT32 partition read only and boots on to Wi-Fi")

        # 3. The real image with no card: one plain line, then the boot carries on.
        out = boot(img, False, "wifi FAIL cmd5")
        if in_order("kernel8.img with no card", out, ["M1c fb ok", "sd no card", "wifi FAIL cmd5"]):
            if "sd FAIL" in out or "sd card" in out: fails.append("kernel8.img with no card printed more than `sd no card`")
            else: print("  ok: with no card, kernel8.img prints `sd no card` and boots on to Wi-Fi")
finally:
    subprocess.run(["make", "-C", arch, "clean"], capture_output=True)

for m in fails: print("FAIL: " + m)
if fails: sys.exit(1)
print("PASS: on QEMU's raspi4b the Pi image reads a FAT32 SDHC card through EMMC2 at 0xFE340000: ls, cat and both bounds hold, and no card is one line")
