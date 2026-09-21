#!/usr/bin/env python3
"""Headless proof the boot-splash logo (gui_draw_logo, kernel.c) draws with
clean anti-aliasing and no false interior seams, same boot + pmemsave shape
as traycorner-check.py/iconedge-check.py: boots kernel.elf with -display
none, pmemsaves the framebuffer right at the boot splash (before the ~1s
hold hands off to the desktop), and asserts on real pixels.

Real, confirmed bug (not a guess): gui_draw_capsule's physical-pixel
coverage-AA path (v83) blended every partial-coverage edge pixel toward
the caller's flat `into` background color, regardless of what was already
drawn there. The boot logo's crown is built from several overlapping
capsules that share joints (trunk top, each branch split), so wherever a
later capsule's own edge band crossed ground an earlier capsule had
already painted solid white, that pixel got faded toward black anyway,
punching a visible dark hairline crack through what should read as solid
fill -- confirmed with a real pmemsave capture zoomed 4x/8x, and by sampling
actual pixels: (960, 438), deep inside the trunk-top/branch-split overlap,
read (48, 48, 48) before the fix (a partial-black notch inside solid
geometry) and (255, 255, 255) after (real solid fill, joints blend toward
whatever is already there instead of a flat backdrop).

This checks that exact pixel, plus a same-region sanity check that real
AA (an intermediate gray, not a flat binary edge) still exists on the
logo's true outer silhouette, so a fix that makes everything either full
white or a flat non-white also fails this.

Usage: tools/checks/bootlogo-check.py   (from the repo root, after make kernel.elf)
"""
import json, os, socket, subprocess, sys, time
from PIL import Image

LOG = "/tmp/jt-bootlogo-check-serial.log"
DUMP = "/tmp/jt-bootlogo-check.raw"
FB = 0xfd000000; W, H = 1920, 1080
PORT = 4463

# Deep inside the trunk-top / branch-split capsule overlap: solid white
# once the seam bug is fixed, a partial-black notch when it regresses.
SEAM_PX, SEAM_PY = 960, 438
SOLID_NOTCH = (48, 48, 48)   # pre-fix: pure notch color at this exact pixel

# A point on the crown's real outer diagonal edge (left branch, upper
# silhouette), expected to be a genuine intermediate AA blend, never pure
# black or pure white, so this check can't be satisfied by just painting
# everything solid.
EDGE_PX, EDGE_PY = 899, 460

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
    # No extra hold: pmemsave right after the QMP handshake lands inside
    # the boot splash's logo-only window (the splash holds the logo alone
    # for ~0.6s before a progress bar even appears, and connecting +
    # capabilities negotiation alone already eats a chunk of that).
    cmd({"execute": "pmemsave", "arguments": {"val": FB, "size": W * H * 4, "filename": DUMP}})
    try: cmd({"execute": "quit"})
    except (ConnectionResetError, BrokenPipeError, OSError): pass
finally:
    try: q.wait(timeout=5)
    except subprocess.TimeoutExpired: q.kill()

img = Image.frombytes("RGBA", (W, H), open(DUMP, "rb").read(), "raw", "BGRA").convert("RGB")
# Geometry-free on purpose: the logo has been redrawn more than once, and a
# check pinned to two pixel coordinates broke every time without catching anything.
g = img.convert("L")
bb = g.point(lambda v: 255 if v > 40 else 0).getbbox()
fail = 0
if not bb or bb[2] - bb[0] < 60:
    print(f"FAIL: no boot logo found on the splash frame (bbox {bb})"); sys.exit(1)
px = g.load()
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
print(f"logo bbox {bb}: solid {solid}, antialiased {mid}, seam holes {holes}, edge blocks {edges}, pixel-doubled {doubled}")
if holes:
    fail = 1; print("FAIL: dark cracks inside the logo, capsule joints are blending toward a flat backdrop again")
if mid * 20 < solid:
    fail = 1; print("FAIL: almost no intermediate tones, the logo edge is not antialiased")
if edges and doubled * 4 > edges:
    fail = 1; print("FAIL: edges are 2x2 identical blocks, the logo is being pixel-doubled from logical resolution")
if not fail: print("PASS: boot logo is solid, antialiased and drawn at physical resolution")
sys.exit(fail)
