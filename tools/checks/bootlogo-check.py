#!/usr/bin/env python3
"""Headless proof the menu bar logo (gui_draw_logo, kernel.c ~3261) draws with
clean anti-aliasing and no false interior seams. Same boot + pmemsave shape
as traycorner-check.py/iconedge-check.py: boots kernel.elf with -display
none, waits for the desktop to appear (after the boot splash), pmemsaves the
framebuffer at the menu bar region, and asserts on real pixels.

Real, confirmed bug (not a guess): gui_draw_capsule's physical-pixel
coverage-AA path (v83) blended every partial-coverage edge pixel toward
the caller's flat `into` background color, regardless of what was already
drawn there. The logo's crown is built from several overlapping capsules
that share joints (trunk top, each branch split), so wherever a later
capsule's own edge band crossed ground an earlier capsule had already
painted solid white, that pixel got faded toward black anyway, punching a
visible dark hairline crack through what should read as solid fill.

This checks that the menu bar's logo region has no dark seam cracks, plus
a sanity check that real AA (intermediate gray tones, not flat binary edges)
still exists on the logo's outer silhouette, so a fix that makes everything
either full white or a flat non-white also fails this.

Usage: tools/checks/bootlogo-check.py   (from the repo root, after make kernel.elf)
"""
import json, os, socket, subprocess, sys, time
from PIL import Image

LOG = "/tmp/jt-bootlogo-check-serial.log"
DUMP = "/tmp/jt-bootlogo-check.raw"
FB = 0xfd000000; W, H = 1920, 1080
PORT = 4463

# Menu bar height and logo position. gui_draw_logo is called at (16, GUI_MENUBAR_H / 2 + 2)
GUI_MENUBAR_H = 26

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
    f.readline()
    cmd({"execute": "qmp_capabilities"})

    # Wait for the desktop to appear. The boot splash holds for ~1s, then
    # the desktop appears. We want to capture after the desktop is up and
    # the menu bar is drawn, so wait ~1.5s total from boot.
    time.sleep(0.5)  # We already waited 1.0s before connecting

    # Pmemsave the entire framebuffer
    cmd({"execute": "pmemsave", "arguments": {"val": FB, "size": W * H * 4, "filename": DUMP}})
    try: cmd({"execute": "quit"})
    except (ConnectionResetError, BrokenPipeError, OSError): pass
finally:
    try: q.wait(timeout=5)
    except subprocess.TimeoutExpired: q.kill()

img = Image.frombytes("RGBA", (W, H), open(DUMP, "rb").read(), "raw", "BGRA").convert("RGB")

# Crop to the menu bar region (top GUI_MENUBAR_H rows)
menubar_img = img.crop((0, 0, W, GUI_MENUBAR_H))
g = menubar_img.convert("L")

# Find the bounding box of non-empty pixels in the menu bar
bb = g.point(lambda v: 255 if v > 40 else 0).getbbox()
fail = 0
if not bb or bb[2] - bb[0] < 10:
    print(f"FAIL: no logo found in the menu bar (bbox {bb})"); sys.exit(1)

px = g.load()

# Analyze the logo region for seams and anti-aliasing
solid = mid = holes = doubled = edges = 0
for y in range(bb[1], bb[3]):
    for x in range(bb[0], bb[2]):
        v = px[x, y]
        if v >= 225: solid += 1
        elif v > 30: mid += 1
        # a dark pixel boxed in by white two pixels away on all four sides is a seam crack
        if v < 128 and all(px[x + dx, y + dy] >= 225 for dx, dy in ((2, 0), (-2, 0), (0, 2), (0, -2))): holes += 1

for y in range(bb[1] & ~1, bb[3] - 1, 2):
    for x in range(bb[0] & ~1, bb[2] - 1, 2):
        blk = (px[x, y], px[x + 1, y], px[x, y + 1], px[x + 1, y + 1])
        if min(blk) < 225 and max(blk) > 30:          # an edge block
            edges += 1
            if len(set(blk)) == 1: doubled += 1

print(f"menu bar logo bbox {bb}: solid {solid}, antialiased {mid}, seam holes {holes}, edge blocks {edges}, pixel-doubled {doubled}")
if holes:
    fail = 1; print("FAIL: dark cracks inside the logo, capsule joints are blending toward a flat backdrop again")
if mid * 20 < solid:
    fail = 1; print("FAIL: almost no intermediate tones, the logo edge is not antialiased")
if edges and doubled * 4 > edges:
    fail = 1; print("FAIL: edges are 2x2 identical blocks, the logo is being pixel-doubled from logical resolution")
if not fail: print("PASS: menu bar logo is solid, antialiased and drawn at physical resolution")
sys.exit(fail)
