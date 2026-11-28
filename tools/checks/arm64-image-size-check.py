#!/usr/bin/env python3
"""The Pi's kernel8.img must stay small enough for a Pi's `update` to fetch (13.27.3).

browser.c reads a reply into at most RAW_MAX (4 MiB, headers and all), so the release job refuses to publish a kernel8.img
of 4128768 bytes or more and the Pi's `update` has nothing to install. 13.27.0's painted icons and wallpaper took it to
4.33 MB and no PR check noticed: only the release job went red, after the merge. This builds the real image and fails
over the same limit, read from .github/workflows/release.yml so the two cannot drift.

Discriminating: store park_wall.h's pixels raw again (park_px, 518 KB) and the image is 4.39 MB, over the limit.
It also proves the stored pictures unpack: park_wall.h and splash_mark.h each carry an Adler-32 of the unpacked bytes.
Usage: python3 tools/checks/arm64-image-size-check.py"""
import os, re, subprocess, sys, zlib

ROOT = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
ARCH = os.path.join(ROOT, "arch", "arm64")
wf = open(os.path.join(ROOT, ".github", "workflows", "release.yml")).read()
m = re.search(r'if \[ "\$size" -ge (\d+) \]', wf)
if not m: sys.exit("FAIL: release.yml no longer names the kernel8.img size limit")
LIMIT = int(m.group(1))

bad = []
for hdr, name, total, sumdef in (("park_wall.h", "park_z", 960 * 540, "PARK_Z_SUM"), ("splash_mark.h", "splash_mark_z", 400 * 400, "SPLASH_MARK_Z_SUM")):
    src = open(os.path.join(ARCH, hdr)).read()
    if name + "[" not in src: bad.append(f"{hdr}: no {name}, the picture is stored raw"); continue
    body = src[src.index(name + "["):]
    z = bytes(int(t) for t in re.findall(r"\d+", body[body.index("{"):body.index("}")]))
    try: raw = zlib.decompress(z, -15)
    except zlib.error as e: bad.append(f"{hdr}: {name} does not inflate ({e})"); continue
    want = int(re.search(r"#define %s 0x([0-9A-Fa-f]{8})u" % sumdef, src).group(1), 16)
    if len(raw) != total: bad.append(f"{hdr}: {name} unpacks to {len(raw)} bytes, want {total}")
    elif zlib.adler32(raw) != want: bad.append(f"{hdr}: {name} unpacks to bytes whose Adler-32 is not {sumdef}")
    else: print(f"  ok: {hdr}: {len(z)} bytes stored, {total} unpacked, checksum matches")

subprocess.run(["make", "-s", "-C", ARCH, "clean"], capture_output=True)
r = subprocess.run(["make", "-s", "-C", ARCH, "kernel8.img"], capture_output=True, text=True)
if r.returncode: sys.exit("FAIL: kernel8.img did not build:\n" + r.stderr[-2000:])
size = os.path.getsize(os.path.join(ARCH, "kernel8.img"))
subprocess.run(["make", "-s", "-C", ARCH, "clean"], capture_output=True)
if size >= LIMIT: bad.append(f"kernel8.img is {size} bytes, over the {LIMIT} a Pi's update can fetch (release.yml, browser.c RAW_MAX)")
else: print(f"  ok: kernel8.img is {size} bytes, {LIMIT - size} under the {LIMIT} a Pi's update can fetch")
if bad:
    for b in bad: print("FAIL:", b)
    sys.exit(1)
print("PASS: the Pi image fits a Pi's update, and its stored pictures unpack to what they claim")
