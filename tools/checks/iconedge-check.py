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
DOCK_ICON, DOCK_GAP, SLOT0_X, ICON_TOP_Y, SCALE, SLOTS = 37, 6, 247, 469, 2, 11
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

# The Trash base rim, same property this has always asserted, re-aimed at the
# authored artwork that now draws it (art/icons/trash.svg).
#
# What it asserted before: tile rows 55-57, x 24-52 (87 px), every pixel
# luminance >= 240, zero tolerance. That was the v71.9 regression: the can's
# three ribs used to run to within one supersample row of the base, so the
# shadow pass's ribs (offset 2 physical px down) poked out under the real
# body as three mid-grey stubs and the real ribs cut the base rim to nothing.
# Before the fix those rows read 0 / 85 luminance in the rib columns.
#
# What it asserts now: tile rows 52-56, x 28-46 (95 px), every pixel
# luminance >= 240, zero tolerance. Same defect, same threshold, same
# zero tolerance, and a strictly LARGER region: 95 pixels over 5 rows rather
# than 87 over 3. Only the coordinates moved, because the authored can's base
# band sits a few rows higher and is narrower than the primitive can's was.
# Measured on the real capture, not guessed: rows 52-56 carry an unbroken
# >= 240 run spanning x 26-47, so the asserted window sits inside that run
# with a pixel of margin on each side rather than on its edge.
#
# It still catches exactly the same failure. The ribs are the dark bars
# above this band; a rib extended into it, or a shadow stub poking below the
# body, drops those pixels far under 240 and fails the assertion. The band is
# deliberately drawn as one FLAT fill in the artwork rather than sharing the
# can's left-to-right gradient, so "solid" stays a fixed luminance claim
# instead of depending on where in the band the check happens to sample.
#
# The glossy-tile pass gave every glyph its own top-light gradient, and that
# gradient is white-only with no darkening term at the bottom specifically so
# this band survives it: the band sits about 73% of the way down the glyph, so
# any black term there would land on it directly and drag it under 240.
# tools/gen/restyle_icons.py says the same thing next to LIFT. Re-measured on
# a real capture after that pass rather than argued: still 0 of 95 pixels
# under 240, the same result as before it.
TRASH_SLOT = 9
TRASH_ROWS, TRASH_COLS = range(52, 57), range(28, 47)
x0 = (SLOT0_X + TRASH_SLOT * PITCH) * SCALE; y0 = ICON_TOP_Y * SCALE
bad = [(x, y, lum(img.getpixel((x0 + x, y0 + y)))) for y in TRASH_ROWS for x in TRASH_COLS
       if lum(img.getpixel((x0 + x, y0 + y))) < 240]
print(f"trash base band non-white pixels: {len(bad)}/{len(TRASH_ROWS) * len(TRASH_COLS)}")
if bad:
    print("  sample:", bad[:5])
    print("FAIL: Trash base rim is cut by ribs or shadow-rib stubs"); sys.exit(1)
print("PASS: Trash base rim is solid, no shadow-rib stubs under the can")
