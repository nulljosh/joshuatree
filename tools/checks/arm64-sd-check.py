#!/usr/bin/env python3
"""ARM64 M4 SD card: the Pi image reads and writes a FAT32 card through EMMC2, at the board's own address.

QEMU's raspi4b models EMMC2 (0xFE340000, the host the Pi 4's card slot is wired to) as an SDHCI host. `-drive if=sd`
does not reach it: that card goes to the GPIO-routed Arasan host at 0xFE300000, which on a real Pi is the Wi-Fi chip's.
So the card goes in with `-device sd-card,bus=/generic-sdhci/sd-bus`, the first SDHCI on the bus, which is EMMC2 in
QEMU 11 (if a later QEMU orders them the other way, the boot says `sd no card` and this check fails, it cannot pass on
the wrong host). The card is a sparse 4 GiB file this script writes itself: a power of two (QEMU insists) and over
2 GiB, so QEMU's card is SDHC and block addressed like Joshua's. Its MBR holds one 64 MiB FAT32 partition at block
2048 with one-sector clusters, so every file and the root directory are real FAT32 chains: config.txt (a copy of
tools/pi-config.txt), stand-ins for the other boot files, BIG.TXT (10 KB, its clusters interleaved with FILLER.BIN's),
a root two clusters long (ZLAST.TXT sits in the second), a long-name entry, a deleted entry, a volume label, a
directory, and on the read card BAD.TXT, whose first cluster lies just past the partition's end. Nothing from a real
card is ever read or printed.

Reads, three boots. The sdtest build (`make -C arch/arm64 sdtest`) runs the Terminal's own commands at boot: `ls` must
list every file with its size (and no label, long-name or deleted entry), `cat config.txt` must print the file,
`cat BIG.TXT` exactly its first 4096 bytes, `cat BAD.TXT` must be refused at the partition's end, and a raw read past
the card's last block must be refused. The real kernel8.img with the card must mount it, and with no card must print
`sd no card` and carry on to the Wi-Fi step, the desktop already up.

Writes, two boots on one clean card (no BAD.TXT; the first free FAT entries carry a top nibble, which FAT32 says
belongs to the volume). sdw-kernel8.img (`make sdwtest`) writes JT/NOTE.TXT, replaces it, writes and deletes
JT/GONE.TXT, makes JT/LOGS and writes a two-cluster JT/CHAIN.TXT; `write config.txt`, `write kernel8.img`, a path
and `rm start4.elf` must be refused on the UART; a write past the card and one to the boot sector must be refused;
and a CMD25 of eight blocks at the card's end must read back. Then, from the host: mtools sees the files with the
right bytes, the FAT pass below (and `fsck.fat -n` where dosfstools is installed) finds the file system clean, the
boot files and boot sectors hash the same as before, and the eight blocks are on the image. sdr-kernel8.img then boots
the same card, as after a reboot: `cat JT/NOTE.TXT` must still print the replaced line, then it deletes NOTE.TXT and
CHAIN.TXT, and the host checks again that the file system is clean, every cluster they held is free and the top
nibbles are still there. Skips (exit 0) without clang, ld.lld, qemu-system-aarch64 or its raspi4b model; the host
side of the write half needs mtools.
Usage: tools/checks/arm64-sd-check.py   (from the repo root)
"""
import hashlib, os, re, shutil, struct, subprocess, sys, time
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
BOOT = {"KERNEL8.IMG": bytes(range(256)) * 6 + b"kernel", "START4.ELF": b"\x7fELF" + bytes(700),
        "BOOTCODE.BIN": b"bootcode" * 70, "FIXUP4.DAT": b"fixup" * 20}   # stand-ins for the files the firmware boots from
NIBBLE = 0xA0000000                                      # the write card's first free FAT entries carry this top nibble


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


def build_card(path, with_bad):
    """Write the card; return the (name as ls prints it, size) of every root file and the first free cluster. The
    write card (no BAD.TXT) also has PAD.BIN, whose chain fills every cluster up to 65544, so each cluster a write
    takes needs the high half of its number (first_cluster_high) in its directory entry."""
    fat = {0: 0x0FFFFFF8, 1: 0x0FFFFFFF}
    clusters = {}                                        # cluster -> its 512 bytes
    next_free = [3]                                      # cluster 2 is the root's first

    def alloc(n):
        out = list(range(next_free[0], next_free[0] + n)); next_free[0] += n; return out

    def link(cl):
        for a, b in zip(cl, cl[1:]): fat[a] = b
        fat[cl[-1]] = 0x0FFFFFFF

    def put(data, cl):
        for i, c in enumerate(cl): clusters[c] = data[i * 512:(i + 1) * 512].ljust(512, b"\0")
        link(cl)

    files = []
    root_dir = [entry(b"JTCHECK    ", 0x08, 0, 0)]       # the volume label: never listed
    c = alloc(1); put(CONFIG, c); root_dir.append(entry(b"CONFIG  TXT", 0x20, c[0], len(CONFIG))); files.append(("CONFIG.TXT", len(CONFIG)))
    for name, data in BOOT.items():
        base, ext = name.split(".")
        c = alloc(-(-len(data) // 512)); put(data, c)
        root_dir.append(entry(base.ljust(8).encode() + ext.encode(), 0x20, c[0], len(data))); files.append((name, len(data)))
    dtb = b"\xd0\x0d\xfe\xed" + bytes(96)
    c = alloc(1); put(dtb, c); root_dir.append(lfn("bcm2711-rpi-4-b.dtb", b"BCM271~1DTB") + entry(b"BCM271~1DTB", 0x20, c[0], len(dtb)))
    files.append(("BCM271~1.DTB", len(dtb)))
    d = alloc(1)
    put(entry(b".          ", 0x10, d[0], 0) + entry(b"..         ", 0x10, 0, 0), d)
    root_dir.append(entry(b"OVERLAYS   ", 0x10, d[0], 0))
    both = alloc(40); big, filler = both[0::2], both[1::2]   # interleaved: BIG.TXT's chain skips a cluster every step
    put(BIG, big); root_dir.append(entry(b"BIG     TXT", 0x20, big[0], len(BIG))); files.append(("BIG.TXT", len(BIG)))
    put(bytes(range(256)) * 40, filler); root_dir.append(entry(b"FILLER  BIN", 0x20, filler[0], 10240)); files.append(("FILLER.BIN", 10240))
    if with_bad: root_dir.append(entry(b"BAD     TXT", 0x20, BAD_CLUSTER, 1000)); files.append(("BAD.TXT", 1000))
    root_dir.append(b"\xe5" + entry(b"GONE    TXT", 0x20, 0, 5)[1:])   # deleted: never listed
    pad = [] if with_bad else alloc(65544 - next_free[0])   # its bytes are the image's zeros, never written here
    for i in range(1, 10):
        t = b"file %d\n" % i
        c = alloc(1); put(t, c); root_dir.append(entry(b"FILE%02d  TXT" % i, 0x20, c[0], len(t))); files.append(("FILE%02d.TXT" % i, len(t)))
    t = b"the last entry, in the root's second cluster\n"
    c = alloc(1); put(t, c); root_dir.append(entry(b"ZLAST   TXT", 0x20, c[0], len(t))); files.append(("ZLAST.TXT", len(t)))
    if pad: link(pad); root_dir.append(entry(b"PAD     BIN", 0x20, pad[0], len(pad) * 512)); files.append(("PAD.BIN", len(pad) * 512))
    rd = b"".join(root_dir)
    assert 512 < len(rd) <= 1024 - 64, len(rd)           # two root clusters, with room left for JT
    put(rd, [2] + alloc(1))
    if not with_bad:
        for k in range(next_free[0], next_free[0] + 32): fat[k] = NIBBLE   # free, but the volume's top bits are set

    bpb = bytearray(512)
    bpb[0:3] = b"\xeb\x58\x90"; bpb[3:11] = b"JTCHECK "
    struct.pack_into("<HBHBHHBHHHII", bpb, 11, 512, SPC, RES, NFATS, 0, 0, 0xF8, 0, 63, 255, PART, PSECT)
    struct.pack_into("<IHHIHH", bpb, 36, FATSZ, 0, 0, 2, 1, 6)
    struct.pack_into("<BBBI11s8s", bpb, 64, 0x80, 0, 0x29, 0x12345678, b"JTCHECK    ", b"FAT32   ")
    bpb[510:512] = b"\x55\xaa"
    fsinfo = bytearray(512)                              # a true free count and hint, as mkfs.fat leaves them
    struct.pack_into("<I", fsinfo, 0, 0x41615252)
    struct.pack_into("<III", fsinfo, 484, 0x61417272, CLUSTERS - (next_free[0] - 2), next_free[0])
    struct.pack_into("<I", fsinfo, 508, 0xAA550000)
    mbr = bytearray(512)
    struct.pack_into("<B3sB3sII", mbr, 446, 0, b"\xfe\xff\xff", 0x0C, b"\xfe\xff\xff", PART, PSECT)
    mbr[510:512] = b"\x55\xaa"
    fatbytes = bytearray((max(fat) + 1) * 4)
    for k, v in fat.items(): struct.pack_into("<I", fatbytes, k * 4, v)
    with open(path, "wb") as f:
        f.truncate(CARD)                                 # sparse: only the sectors below take space
        def at(sector, b): f.seek(sector * 512); f.write(b)
        at(0, mbr)
        at(PART, bpb); at(PART + 1, fsinfo); at(PART + 6, bpb); at(PART + 7, fsinfo)
        for n in range(NFATS): at(PART + RES + n * FATSZ, fatbytes)
        for k, b in clusters.items(): at(PART + DATA + (k - 2) * SPC, b)
    return files, next_free[0]


card = os.path.join(TMP, "card.img")
files, _ = build_card(card, True)
wcard = os.path.join(TMP, "wcard.img")
_, FIRST_FREE = build_card(wcard, False)

# ---- The host's view of the write card ----
def psector(img, n, count=1):
    with open(img, "rb") as f: f.seek((PART + n) * 512); return f.read(512 * count)


def fat_pass(img, label):
    """A FAT32 consistency pass over the card, like fsck's: every chain in range, no loops, no cluster owned twice, no
    lost cluster, sizes that match their chains, `.` and `..` right, both FAT copies the same, FSInfo true or unknown.
    Returns {cluster: raw FAT entry} for the nibble check, or None after recording a failure."""
    raw = psector(img, RES, FATSZ)
    if raw != psector(img, RES + FATSZ, FATSZ): fails.append(f"{label}: the two FAT copies differ"); return None
    ent = struct.unpack("<%dI" % (CLUSTERS + 2), raw[:(CLUSTERS + 2) * 4])
    nxt = [e & 0x0FFFFFFF for e in ent]
    owner, bad = {}, []

    def chain(c, who):
        out = []
        while 2 <= c < 0x0FFFFFF8:
            if c >= CLUSTERS + 2: bad.append(f"{who}: cluster {c} past the end"); break
            if c in owner: bad.append(f"{who}: cluster {c} is also {owner[c]}'s"); break
            owner[c] = who; out.append(c); c = nxt[c]
        return out

    def walk(first, path, parent):
        cl = chain(first, path or "/")
        data = b"".join(psector(img, DATA + c - 2) for c in cl)
        for i in range(0, len(data), 32):
            e = data[i:i + 32]
            if e[0] == 0: break
            if e[0] == 0xE5 or e[11] == 0x0F or e[11] & 0x08: continue
            name = e[:8].rstrip().decode() + ("." + e[8:11].rstrip().decode() if e[8:11].strip() else "")
            c = struct.unpack_from("<H", e, 26)[0] | struct.unpack_from("<H", e, 20)[0] << 16
            size = struct.unpack_from("<I", e, 28)[0]
            if name == ".":
                if c != first: bad.append(f"{path}/.: cluster {c}, not {first}")
                continue
            if name == "..":
                if c != parent: bad.append(f"{path}/..: cluster {c}, not {parent}")
                continue
            if e[11] & 0x10: walk(c, path + "/" + name, 0 if not path else first)
            else:
                n = len(chain(c, path + "/" + name))
                if n != -(-size // 512): bad.append(f"{path}/{name}: {size} bytes in {n} clusters")
    walk(2, "", 0)
    lost = [c for c in range(2, CLUSTERS + 2) if nxt[c] and c not in owner]
    if lost: bad.append(f"{len(lost)} lost clusters, from {lost[:5]}")
    free = sum(1 for c in range(2, CLUSTERS + 2) if not nxt[c])
    fs = psector(img, 1)
    count = struct.unpack_from("<I", fs, 488)[0]
    if struct.unpack_from("<I", fs, 0)[0] != 0x41615252 or count not in (0xFFFFFFFF, free):
        bad.append(f"FSInfo says {count:#x} free, the FAT {free}")
    if ent[0] != 0x0FFFFFF8 or ent[1] != 0x0FFFFFFF: bad.append("FAT entries 0 and 1 changed")
    for b in bad: fails.append(f"{label}: {b}")
    return None if bad else ent


def fsck(img, label):
    """dosfstools' fsck.fat -n over the partition, where it is installed (CI has it)."""
    exe = shutil.which("fsck.fat") or shutil.which("dosfsck")
    if not exe: return "no fsck.fat here"
    part = os.path.join(TMP, "part.img")
    with open(part, "wb") as f: f.write(psector(img, 0, PSECT))
    r = subprocess.run([exe, "-n", part], capture_output=True, text=True)
    os.remove(part)
    if r.returncode != 0: fails.append(f"{label}: fsck.fat -n exited {r.returncode}: {(r.stdout + r.stderr).strip()[-400:]!r}"); return ""
    return "fsck.fat -n clean"


MT = dict(os.environ, MTOOLS_SKIP_CHECK="1")
def mt(*args):
    return subprocess.run([args[0], "-i", f"{wcard}@@{PART * 512}", *args[1:]], capture_output=True, env=MT)


def boot_hashes():
    """Every boot file's bytes through mtools, the long-name dtb and the overlays folder too, and the boot sectors."""
    out = {}
    for name in ["CONFIG.TXT", *BOOT, "bcm2711-rpi-4-b.dtb"]:
        r = mt("mtype", "::" + name)
        out[name] = hashlib.sha256(r.stdout).hexdigest() if r.returncode == 0 else f"mtype failed: {r.stderr!r}"
    out["OVERLAYS/"] = mt("mdir", "-b", "::OVERLAYS").stdout
    out["boot sectors"] = hashlib.sha256(psector(wcard, 0) + psector(wcard, 2, RES - 2)).hexdigest()   # all but FSInfo
    return out


# ---- Build and boot ----
def make(*targets):
    try: return subprocess.run(["make", "-C", arch, *targets], capture_output=True, timeout=900).returncode == 0
    except subprocess.TimeoutExpired: return False


def boot(img, sd, done):
    log = os.path.join(TMP, "uart-%d.log" % time.monotonic_ns())
    cmd = ["qemu-system-aarch64", "-machine", "raspi4b", "-display", "none", "-serial", "null", "-serial", "file:" + log, "-kernel", img]   # serial 0 is the PL011, the Bluetooth radio's; the console is the mini UART, serial 1
    if sd: cmd += ["-drive", f"if=none,id=card,file={sd},format=raw", "-device", "sd-card,drive=card,bus=/generic-sdhci/sd-bus"]
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


MOUNT = "sd fat32 partition at block 2048, 64 MB, writes go in JT/"
subprocess.run(["make", "-C", arch, "clean"], capture_output=True)
try:
    if not make("pi") or not make("sdtest") or not make("sdwtest"):
        fails.append("arch/arm64 `make pi`, `make sdtest` or `make sdwtest` does not build")
    else:
        img, test_img = os.path.join(arch, "kernel8.img"), os.path.join(arch, "sd-kernel8.img")

        # 1. The sdtest build: ls, cat, and the two reads that must be refused.
        out = boot(test_img, card, "sdtest done")
        if in_order("sdtest", out, ["sd card sdhc 4096 MB", MOUNT, "sdtest> ls",
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
        out = boot(img, card, "wifi FAIL cmd5")
        if in_order("kernel8.img with a card", out, ["M1c fb ok", "sd card sdhc 4096 MB, base clock", MOUNT, "wifi FAIL cmd5"]):
            if "sdtest" in out or "sd refused" in out or "sd FAIL" in out: fails.append("kernel8.img with a card printed a test, refusal or FAIL line")
            else: print("  ok: kernel8.img finds the card on EMMC2 (0xFE340000), mounts the FAT32 partition and boots on to Wi-Fi")

        # 3. The real image with no card: one plain line, then the boot carries on.
        out = boot(img, None, "wifi FAIL cmd5")
        if in_order("kernel8.img with no card", out, ["M1c fb ok", "sd no card", "wifi FAIL cmd5"]):
            if "sd FAIL" in out or "sd card" in out: fails.append("kernel8.img with no card printed more than `sd no card`")
            else: print("  ok: with no card, kernel8.img prints `sd no card` and boots on to Wi-Fi")

        # 4. Writes on the clean card, then the host's look at it.
        if not shutil.which("mtype"):
            print("  SKIP: the write half needs mtools (mtype, mdir) to look at the card from the host")
        else:
            before = boot_hashes()
            if fat_pass(wcard, "the write card as built") is None or "failed" in str(before): fails.append(f"the write card is not clean before any write: {before!r}")
            out = boot(os.path.join(arch, "sdw-kernel8.img"), wcard, "sdtest done")
            boot_name = "the firmware boots from it"
            refusals = [("write config.txt hello", "config.txt", boot_name), ("write kernel8.img x", "kernel8.img", boot_name),
                        ("write ../config.txt x", "../config.txt", "not an 8.3 name"), ("rm start4.elf", "start4.elf", boot_name),
                        ("mkdir overlays", "overlays", boot_name)]
            if in_order("sdwtest", out, [MOUNT, "sdtest> cat JT/NOTE.TXT", "cat: no file JT/NOTE.TXT",
                                         "wrote JT/NOTE.TXT, 15 bytes", "wrote JT/NOTE.TXT, 21 bytes", "wrote JT/GONE.TXT, 4 bytes",
                                         "removed JT/GONE.TXT", "made JT/LOGS/", "wrote JT/CHAIN.TXT, 1021 bytes",
                                         *[f"sd refused {n}, {why}" for _, n, why in refusals],
                                         "sdtest> ls JT", "sdtest> cat JT/NOTE.TXT", "it survives a reboot", "cat: no file JT/GONE.TXT",
                                         f"sd refused write to block {CARD // 512}, past the end of the card",
                                         "sd refused write to block 0, a boot sector", "sdtest multi-block write ok", "sdtest done"]):
                ls = [l.split() for l in section(out, "sdtest> ls JT\n", "sdtest> cat JT/NOTE.TXT").splitlines() if l.strip()]
                if sorted(ls) != sorted([["NOTE.TXT", "21"], ["CHAIN.TXT", "1021"], ["LOGS/"]]): fails.append(f"ls JT printed {ls!r}")
                else: print("  ok: on the card, write makes JT/ and a file, replaces it, rm deletes one, mkdir makes JT/LOGS, ls JT lists them")
                for (line, n, why), nxt in zip(refusals, [r[0] for r in refusals[1:]] + ["ls JT"]):
                    said = section(out, f"sdtest> {line}\n", f"sdtest> {nxt}\n").strip()
                    if said != f"sd refused {n}, {why}": fails.append(f"{line}: should print only the refusal, printed {said!r}")
                print("  ok: config.txt, kernel8.img, start4.elf, overlays and a path are refused on the UART; so are a write past the card and one to the boot sector")
            if "sd FAIL" in out: fails.append("sdwtest: an `sd FAIL` line: " + next(l for l in out.splitlines() if "sd FAIL" in l))
            note, chain = mt("mtype", "::JT/NOTE.TXT").stdout, mt("mtype", "::JT/CHAIN.TXT").stdout
            listing = mt("mdir", "-b", "::JT").stdout.decode(errors="replace").split()
            if note != b"it survives a reboot\n" or chain != b"0123456789" * 102 + b"\n" or sorted(listing) != sorted(["::/JT/NOTE.TXT", "::/JT/CHAIN.TXT", "::/JT/LOGS/"]):
                fails.append(f"mtools: JT/ holds {listing!r}, NOTE.TXT is {note!r}, CHAIN.TXT {len(chain)} bytes")
            else: print(f"  ok: mtools on the host finds JT/NOTE.TXT (the replaced line), the two-cluster JT/CHAIN.TXT and JT/LOGS/, and no GONE.TXT, all in clusters from {FIRST_FREE} up, so first_cluster_high is right")
            ent = fat_pass(wcard, "after sdwtest")
            if ent is not None:
                used = [c for c in range(FIRST_FREE, FIRST_FREE + 32) if ent[c] & 0x0FFFFFFF]
                if not used or any(ent[c] >> 28 != NIBBLE >> 28 for c in range(FIRST_FREE, FIRST_FREE + 32)):
                    fails.append(f"after sdwtest: the top nibble of FAT entries {FIRST_FREE}.. is gone, or none of them was used")
                else: print(f"  ok: the FAT pass finds it clean ({fsck(wcard, 'after sdwtest')}), and the {len(used)} clusters it took kept their top nibble")
            if boot_hashes() != before: fails.append("after sdwtest: a boot file or boot sector changed")
            else: print("  ok: config.txt, kernel8.img, start4.elf, bootcode.bin, fixup4.dat, the dtb, overlays and the boot sectors hash the same")
            with open(wcard, "rb") as f: f.seek(CARD - 4096); tail = f.read()
            if tail != bytes((i * 7 + (i >> 9)) & 0xFF for i in range(4096)): fails.append("the CMD25 blocks are not at the card's end")
            else: print("  ok: a CMD25 of eight blocks (Auto CMD12, busy wait) lands at the card's end, byte for byte")

            # 5. The same card after a reboot: the file is still there, then it goes.
            out = boot(os.path.join(arch, "sdr-kernel8.img"), wcard, "sdtest done")
            if in_order("sdrtest", out, [MOUNT, "sdtest> cat JT/NOTE.TXT", "it survives a reboot", "removed JT/NOTE.TXT",
                                         "removed JT/CHAIN.TXT", "cat: no file JT/NOTE.TXT", "sdtest> ls JT", "LOGS/", "sdtest done"]):
                ls = [l.split() for l in section(out, "sdtest> ls JT\n", "sdtest done").splitlines() if l.strip()]
                if ls != [["LOGS/"]]: fails.append(f"after the reboot, ls JT printed {ls!r}")
                else: print("  ok: after a reboot on the same card, cat JT/NOTE.TXT still prints it; rm deletes it and CHAIN.TXT")
            if "sd FAIL" in out: fails.append("sdrtest: an `sd FAIL` line: " + next(l for l in out.splitlines() if "sd FAIL" in l))
            ent = fat_pass(wcard, "after the reboot")
            if ent is not None:
                held = [c for c in range(FIRST_FREE, FIRST_FREE + 32) if ent[c] & 0x0FFFFFFF]
                if len(held) != 2 or any(ent[c] >> 28 != NIBBLE >> 28 for c in range(FIRST_FREE, FIRST_FREE + 32)):
                    fails.append(f"after the reboot: {len(held)} of those clusters still taken (JT and LOGS should be the only two), or a top nibble gone")
                else: print(f"  ok: clean again ({fsck(wcard, 'after the reboot')}), every cluster the files held is free, only JT and LOGS remain")
            if boot_hashes() != before: fails.append("after the reboot: a boot file or boot sector changed")
finally:
    subprocess.run(["make", "-C", arch, "clean"], capture_output=True)

for m in fails: print("FAIL: " + m)
if fails: sys.exit(1)
print("PASS: on QEMU's raspi4b the Pi reads and writes a FAT32 SDHC card through EMMC2 at 0xFE340000: files in JT/ survive a reboot, the boot files are never touched, and the card stays clean")
