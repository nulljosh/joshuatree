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
seam = img.getpixel((SEAM_PX, SEAM_PY))
edge = img.getpixel((EDGE_PX, EDGE_PY))

print(f"seam pixel ({SEAM_PX},{SEAM_PY}) = {seam}")
print(f"edge pixel ({EDGE_PX},{EDGE_PY}) = {edge}")

fail = 0
d_notch = max(abs(seam[i] - SOLID_NOTCH[i]) for i in range(3))
if d_notch <= 12:
    print("FAIL: trunk-top seam pixel reads as the pre-fix partial-black notch, capsule joints are blending toward a flat backdrop again")
    fail = 1
elif sum(seam) < 650:
    print(f"FAIL: trunk-top seam pixel is not solid white ({seam}), some other seam/coverage regression")
    fail = 1
else:
    print("PASS: trunk-top seam pixel is solid, no false interior notch")

edge_lum = sum(edge) / 3
if edge_lum < 20 or edge_lum > 235:
    print(f"FAIL: crown outer-edge pixel has no real AA blend ({edge}), coverage sampling looks disabled or the geometry moved")
    fail = 1
else:
    print("PASS: crown outer edge shows a real intermediate AA blend")

sys.exit(fail)
