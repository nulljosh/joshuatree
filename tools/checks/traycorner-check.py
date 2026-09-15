#!/usr/bin/env python3
"""Headless proof of the v79 tray-corner AA fix, same shape as
iconhalo-check.py/dockhover-check.py: boots kernel.elf with -display none,
waits for the desktop's first frame, pmemsaves the real framebuffer, and
asserts on actual pixels.

Real, confirmed bug (not a guess, real macro photo + real pixel capture,
see roadmap.md's v79 entry): the dock tray's own rounded corner
(gui_rounded_rect_on_wallpaper in kernel.c) drew its AA band with ONE
distance sample per PHYSICAL output pixel, thresholded into a 3-physical-
pixel linear ramp. That is a single coverage value per pixel, not a real
coverage fraction, so pixels right at the true circular boundary rendered
as either the tray's full color or the wallpaper's full color with no
blend at all, a hard binary staircase, confirmed with a real pmemsave
capture zoomed 10x: at physical pixel (549, 918), a point that sits
exactly on the arc's true boundary, the pre-fix code produced (21, 10, 6),
pure tray-shadow-corner color, zero blend. Every icon GLYPH avoids this
because gui_render_icon_cached renders into a real 6x oversampled buffer
(ICON_SS_SCALE) and box-downsamples; the tray itself never went through
that pipeline; it always drew straight to physical pixels, one sample
each. v79 fixes it in the same spirit without a full offscreen buffer for
the whole dock width: 4x4 = 16 subsamples per physical pixel, each tested
against the true circle, averaged into a real coverage fraction that
blends tray color against the actual wallpaper pixel behind it.

This checks that exact pixel. Confirmed discriminating: reverting v79's
subsample loop (back to the single-sample distance/band threshold) makes
this pixel come back as pure (21, 10, 6) again, FAIL; with the fix
restored it reads as a real intermediate blend, PASS.

Usage: tools/checks/traycorner-check.py   (from the repo root, after make kernel.elf)
"""
import json, os, socket, subprocess, sys, time
from PIL import Image

LOG = "/tmp/jt-traycorner-serial.log"
DUMP = "/tmp/jt-traycorner.raw"
FB = 0xfd000000; W, H = 1920, 1080
PORT = 4454

# Physical pixel on the tray's top-left corner arc, root-caused as
# described above: pure tray-shadow-corner color pre-fix, a real blended
# intermediate value post-fix.
PX, PY = 549, 918
DARK = (21, 10, 6)     # pre-fix: this pixel reads as pure corner color, zero coverage blend
CREAM = (239, 235, 228)  # the tray's own fill color, for sanity (this pixel should never be fully tray-colored either)

os.chdir(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
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
    time.sleep(5.0)
    cmd({"execute": "pmemsave", "arguments": {"val": FB, "size": W * H * 4, "filename": DUMP}})
    cmd({"execute": "quit"})
finally:
    try: q.wait(timeout=5)
    except subprocess.TimeoutExpired: q.kill()

img = Image.frombytes("RGBA", (W, H), open(DUMP, "rb").read(), "raw", "BGRA").convert("RGB")
p = img.getpixel((PX, PY))

d_dark = max(abs(p[i] - DARK[i]) for i in range(3))
d_cream = max(abs(p[i] - CREAM[i]) for i in range(3))

print(f"pixel ({PX},{PY}) = {p}  (dist to pure dark: {d_dark}, dist to pure tray-cream: {d_cream})")

fail = 0
if d_dark <= 15:
    print("FAIL: tray corner pixel is pure un-blended shadow color, the single-sample staircase is back")
    fail = 1
elif d_cream <= 15:
    print("FAIL: tray corner pixel is pure tray fill, unexpected (should be a genuine intermediate blend here)")
    fail = 1
else:
    print("PASS: tray corner pixel is a real intermediate blend, coverage AA is active")
sys.exit(fail)
