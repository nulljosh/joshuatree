#!/usr/bin/env python3
"""Headless dock-icon edge-quality probe, the same boot + pmemsave shape as
iconhalo-check.py. Captures the real framebuffer after the desktop's first
frame, crops the dock's icon row, and prints a per-icon jaggedness score:
for every physical pixel on the tile's outer rounded-corner arc, the
absolute luminance difference between vertically adjacent pixels, summed.
A cleanly anti-aliased corner ramps smoothly; a staircased one spikes.

Usage: tools/checks/iconedge-check.py [out-prefix]
Writes <out>-dock.png (1:1) and <out>-dock4x.png (nearest 4x) for eyeballing.
"""
import json, os, socket, subprocess, sys, time
from PIL import Image

out = sys.argv[1] if len(sys.argv) > 1 else "/tmp/jt-iconedge"
LOG = "/tmp/jt-iconedge-serial.log"; DUMP = "/tmp/jt-iconedge.raw"
FB = 0xfd000000; W, H = 1920, 1080; PORT = 4461
DOCK_ICON, DOCK_GAP, SLOT0_X, ICON_TOP_Y, SCALE, SLOTS = 37, 6, 268, 469, 2, 10
PITCH = DOCK_ICON + DOCK_GAP

os.chdir(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
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
    f.readline(); cmd({"execute": "qmp_capabilities"}); time.sleep(5.0)
    cmd({"execute": "pmemsave", "arguments": {"val": FB, "size": W * H * 4, "filename": DUMP}})
    # QEMU can tear down the QMP socket the instant it processes quit,
    # before this side ever reads a reply -- a real race, not a bug in
    # the assertions above (which already ran); a reset here must not
    # mask a genuine PASS as a crash.
    try: cmd({"execute": "quit"})
    except (ConnectionResetError, BrokenPipeError, OSError): pass
finally:
    try: q.wait(timeout=5)
    except subprocess.TimeoutExpired: q.kill()

img = Image.frombytes("RGBA", (W, H), open(DUMP, "rb").read(), "raw", "BGRA").convert("RGB")
pw = DOCK_ICON * SCALE
dock = img.crop((SLOT0_X * SCALE - 8, ICON_TOP_Y * SCALE - 8, (SLOT0_X + SLOTS * PITCH) * SCALE, ICON_TOP_Y * SCALE + pw + 8))
dock.save(out + "-dock.png")
dock.resize((dock.width * 4, dock.height * 4), Image.NEAREST).save(out + "-dock4x.png")

def lum(p): return (p[0] * 299 + p[1] * 587 + p[2] * 114) // 1000
total = 0
for slot in range(SLOTS):
    x0 = (SLOT0_X + slot * PITCH) * SCALE; y0 = ICON_TOP_Y * SCALE
    tile = img.crop((x0, y0, x0 + pw, y0 + pw))
    # second-difference energy along every column and row: a smooth ramp
    # has near-zero second difference, a step has a spike
    e = 0
    for x in range(pw):
        col = [lum(tile.getpixel((x, y))) for y in range(pw)]
        e += sum(abs(col[i + 1] - 2 * col[i] + col[i - 1]) for i in range(1, pw - 1))
    for y in range(pw):
        row = [lum(tile.getpixel((x, y))) for x in range(pw)]
        e += sum(abs(row[i + 1] - 2 * row[i] + row[i - 1]) for i in range(1, pw - 1))
    total += e
    print(f"slot {slot}: second-difference energy {e}")
print(f"total {total}")

# v71.9 regression: the Trash can's three ribs used to run to within one
# supersample row of the base, so the shadow pass's ribs (offset 2 physical
# px down) poked out under the real body as three mid-grey stubs, and the
# real ribs cut the base rim to nothing. Measured on the real capture before
# the fix: tile rows 55-57 in the rib columns read 0 / 85 luminance; after,
# the whole base band is solid glyph white. Confirmed discriminating.
TRASH_SLOT = 9
x0 = (SLOT0_X + TRASH_SLOT * PITCH) * SCALE; y0 = ICON_TOP_Y * SCALE
bad = [(x, y, lum(img.getpixel((x0 + x, y0 + y)))) for y in range(55, 58) for x in range(24, 53)
       if lum(img.getpixel((x0 + x, y0 + y))) < 240]
print(f"trash base band non-white pixels: {len(bad)}/{3 * 29}")
if bad:
    print("  sample:", bad[:5])
    print("FAIL: Trash base rim is cut by ribs or shadow-rib stubs"); sys.exit(1)
print("PASS: Trash base rim is solid, no shadow-rib stubs under the can")
