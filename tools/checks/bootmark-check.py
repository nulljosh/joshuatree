#!/usr/bin/env python3
"""Headless proof the boot splash draws the real engraved brand mark
(landing/mark-tree.svg via kernel/boot_mark.h), not the old stick-figure
tree primitive it replaced.

Same boot + pmemsave shape as bootlogo-check.py: boots kernel.elf with
-display none, pmemsaves the framebuffer right at the boot splash, then
downsamples the captured mark region and compares it against the source
SVG rasterized fresh (rsvg-convert, same path gen_boot_mark.py uses) --
a real image match, not a coordinate guess. Also asserts the captured
mark is NOT a match for the old stick-tree's known silhouette (few thin
limbs, mostly empty interior), so a regression back to the primitive
glyph fails this even if some other tree-shaped blob happens to land in
the same bbox.

Usage: tools/checks/bootmark-check.py   (from the repo root, after make kernel.elf)
"""
import json, os, socket, subprocess, sys, time
from PIL import Image

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..")
os.chdir(ROOT)

LOG = "/tmp/jt-bootmark-check-serial.log"
DUMP = "/tmp/jt-bootmark-check.raw"
FB = 0xfd000000; W, H = 1920, 1080
PORT = 4464

for f in (LOG, DUMP):
    try: os.remove(f)
    except FileNotFoundError: pass

q = subprocess.Popen(["qemu-system-i386", "-kernel", "kernel.elf", "-display", "none", "-vga", "std",
                      "-qmp", f"tcp:127.0.0.1:{PORT},server,nowait", "-serial", "file:" + LOG],
                     stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
try:
    time.sleep(1.0)
    s = socket.create_connection(("127.0.0.1", PORT)); f = s.makefile("rw")
    def cmd(o):
        f.write(json.dumps(o) + "\n"); f.flush()
        while True:
            r = json.loads(f.readline())
            if "return" in r or "error" in r: return r
    f.readline()
    cmd({"execute": "qmp_capabilities"})
    cmd({"execute": "pmemsave", "arguments": {"val": FB, "size": W * H * 4, "filename": DUMP}})
    try: cmd({"execute": "quit"})
    except (ConnectionResetError, BrokenPipeError, OSError): pass
finally:
    try: q.wait(timeout=5)
    except subprocess.TimeoutExpired: q.kill()

img = Image.frombytes("RGBA", (W, H), open(DUMP, "rb").read(), "raw", "BGRA").convert("RGB")
g = img.convert("L")
bb = g.point(lambda v: 255 if v > 40 else 0).getbbox()
if not bb or bb[2] - bb[0] < 60:
    print(f"FAIL: no boot mark found on the splash frame (bbox {bb})"); sys.exit(1)
# drop the progress-bar row the same way bootlogo-check.py does: the mark
# ends at the first fully-empty row below its top.
px = g.load()
for y in range(bb[1], bb[3]):
    if not any(px[x, y] > 40 for x in range(bb[0], bb[2])):
        ox, oy = bb[0], bb[1]
        c = g.crop((ox, oy, bb[2], y)).point(lambda v: 255 if v > 40 else 0).getbbox()
        bb = (ox + c[0], oy + c[1], ox + c[2], oy + c[3])
        break

crop = g.crop(bb)
cw, ch = crop.size

# Rasterize the source mark fresh, at the crop's own resolution, and
# compare -- proof the CAPTURED pixels are the mark, not just "some
# tree-ish blob in the right place".
ref_png = "/tmp/jt-bootmark-ref.png"
subprocess.run(["rsvg-convert", "-w", str(cw), "-h", str(ch), "landing/mark-tree.svg", "-o", ref_png], check=True)
# Use the alpha channel, not a grayscale of the flattened RGB: rsvg draws
# the mark as black fill on transparent, which flattens to black-on-WHITE
# (inverted brightness vs. the splash's white-ink-on-black), not a coverage
# map comparable to the captured crop.
ref = Image.open(ref_png).convert("RGBA").split()[3]

crop_small = crop.resize((64, int(64 * ch / cw)))
ref_small = ref.resize((64, int(64 * ch / cw)))
cd, rd = list(crop_small.getdata()), list(ref_small.getdata())
n = len(cd)
match = sum(1 for a, b in zip(cd, rd) if (a > 40) == (b > 40)) / n

# The old stick-tree primitive is nearly all thin limbs: coverage (bright
# pixels) over the whole crop bbox is low, well under a filled engraved
# mark's real interior fill. A regression back to it fails on real pixels,
# not a version string.
coverage = sum(1 for v in cd if v > 40) / n

print(f"mark bbox {bb} ({cw}x{ch}), silhouette match vs source SVG: {match:.1%}, fill coverage: {coverage:.1%}")
fail = 0
if match < 0.75:
    fail = 1; print("FAIL: captured splash mark does not match landing/mark-tree.svg -- old stick tree or something else is drawing")
if coverage < 0.15:
    fail = 1; print("FAIL: fill coverage reads like the old thin-limbed stick tree, not the engraved mark")
if not fail:
    print("PASS: boot splash draws the real engraved mark-tree.svg mark")
sys.exit(fail)
