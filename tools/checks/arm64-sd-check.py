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
`sd no card` and carry on to the Wi-Fi step; the boot splash is up for the SD step and the first `FAIL` line (the Wi-Fi power-on) ends it,
so the desktop paints (`M1c fb ok`) and the splash never stays up on a missing or failing card.

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
import hashlib, os, re, shutil, subprocess, sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from scratch import scratch_dir
from sdcard import CARD, PART, RES, BAD_BLOCK, CONFIG, BIG, BOOT, NIBBLE, build_card, psector, fat_pass as _fat_pass, fsck as _fsck   # the card itself

root = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
arch = os.path.join(root, "arch/arm64")
if not all(shutil.which(t) for t in ("clang", "ld.lld", "qemu-system-aarch64")):
    print("SKIP: clang, ld.lld or qemu-system-aarch64 not installed"); sys.exit(0)
if "raspi4b" not in subprocess.run(["qemu-system-aarch64", "-machine", "help"], capture_output=True, text=True).stdout:
    print("SKIP: QEMU here has no raspi4b model (needs QEMU 9 or newer)"); sys.exit(0)
TMP = scratch_dir("sd")
fails = []

card = os.path.join(TMP, "card.img")
files, _ = build_card(card, True)
wcard = os.path.join(TMP, "wcard.img")
_, FIRST_FREE = build_card(wcard, False)

# ---- The host's view of the write card ----
def fat_pass(img, label): return _fat_pass(img, label, fails)


def fsck(img, label): return _fsck(img, label, fails, TMP)


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

        # 2. The real image with the card: it mounts, prints no test lines, and the boot carries on (the splash is up for the SD step; the first FAIL line, Wi-Fi's, ends it and the desktop paints).
        out = boot(img, card, "wifi FAIL cmd5")
        if in_order("kernel8.img with a card", out, ["splash up", "sd card sdhc 4096 MB, base clock", MOUNT, "M1c fb ok", "wifi FAIL cmd5"]):
            if "sdtest" in out or "sd refused" in out or "sd FAIL" in out: fails.append("kernel8.img with a card printed a test, refusal or FAIL line")
            else: print("  ok: kernel8.img finds the card on EMMC2 (0xFE340000), mounts the FAT32 partition and boots on to Wi-Fi")

        # 3. The real image with no card: one plain line, then the boot carries on and the splash does not stay up (the desktop paints, `M1c fb ok`).
        out = boot(img, None, "wifi FAIL cmd5")
        if in_order("kernel8.img with no card", out, ["splash up", "sd no card", "M1c fb ok", "wifi FAIL cmd5"]):
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
