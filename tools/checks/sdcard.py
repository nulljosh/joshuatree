"""The check card for the ARM SD and file checks: a sparse 4 GiB SDHC image with an MBR and one 64 MiB FAT32 partition
at block 2048 with one-sector clusters, written sector by sector here, and the host's look back at it (a FAT32
consistency pass like fsck's, and fsck.fat -n where dosfstools is installed). arm64-sd-check.py describes the card;
arm64-notes-check.py boots Notes on the same one, and arm64-m3c-check.py hands it to QEMU's virt machine as a virtio
disk. Nothing from a real card is ever read or printed.
"""
import os, shutil, struct, subprocess

root = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))

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


def psector(img, n, count=1):
    with open(img, "rb") as f: f.seek((PART + n) * 512); return f.read(512 * count)


def fat_pass(img, label, fails):
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


def fsck(img, label, fails, tmp):
    """dosfstools' fsck.fat -n over the partition, where it is installed (CI has it)."""
    exe = shutil.which("fsck.fat") or shutil.which("dosfsck")
    if not exe: return "no fsck.fat here"
    part = os.path.join(tmp, "part.img")
    with open(part, "wb") as f: f.write(psector(img, 0, PSECT))
    r = subprocess.run([exe, "-n", part], capture_output=True, text=True)
    os.remove(part)
    if r.returncode != 0: fails.append(f"{label}: fsck.fat -n exited {r.returncode}: {(r.stdout + r.stderr).strip()[-400:]!r}"); return ""
    return "fsck.fat -n clean"
