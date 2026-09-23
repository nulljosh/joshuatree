#!/usr/bin/env python3
"""Headless proof that the shadow under the dock tray follows the photo.

gui_draw_dock_tray used to darken one colour per row (gui_wallpaper_color,
the photo's centre column), so on the photo wallpaper it drew a flat striped
bar under the tray. The oracle: along each shadow row the real photo varies,
so no single colour may cover most of the row. Before the fix every row was
one colour across its whole width (100%); after, the most common colour
covers a few percent.

Usage: tools/checks/dockband-check.py   (from the repo root, after make kernel.elf)
"""
import json, os, re, shutil, socket, subprocess, sys, time
from PIL import Image

LOG = "/tmp/jt-dockband-serial.log"
DUMP = "/tmp/jt-dockband.raw"
FB = 0xfd000000; W, H = 1920, 1080
PORT = 4491


os.chdir(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
for f in (LOG, DUMP):
    try: os.remove(f)
    except FileNotFoundError: pass

q = subprocess.Popen(["qemu-system-i386", "-name", "jt-dockband", "-kernel", "kernel.elf",
                      "-display", "none", "-vga", "std",
                      "-qmp", "tcp:127.0.0.1:%d,server,nowait" % PORT, "-serial", "file:" + LOG],
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
    # Wait for the dock tray itself to be on screen, not for a frame count:
    # the first presented frame can be the boot splash, and a desktop that is
    # up and then sits still never presents again.
    for _ in range(300):
        cmd({"execute": "pmemsave", "arguments": {"val": FB, "size": W * H * 4, "filename": DUMP}})
        probe = Image.frombytes("RGBA", (W, H), open(DUMP, "rb").read(), "raw", "BGRA")
        if any(probe.getpixel((960, y))[:3] == (239, 235, 228) for y in range(900, H, 4)): break
        time.sleep(0.1)
    else:
        print("FAIL: the dock never appeared")
        sys.exit(1)
    time.sleep(0.5)
    cmd({"execute": "pmemsave", "arguments": {"val": FB, "size": W * H * 4, "filename": DUMP}})
    try: cmd({"execute": "quit"})
    except (ConnectionResetError, BrokenPipeError, OSError): pass
finally:
    try: q.wait(timeout=5)
    except subprocess.TimeoutExpired: q.kill()

img = Image.frombytes("RGBA", (W, H), open(DUMP, "rb").read(), "raw", "BGRA").convert("RGB")


TRAY = (239, 235, 228)
X = 960
y = max(y for y in range(700, H) if img.getpixel((X, y)) == TRAY) + 1  # first row under the tray
fail = 0
for row in range(y, y + 16):
    px = [img.getpixel((x, row)) for x in range(560, 1360)]
    top = max(px.count(c) for c in set(px)) / len(px)
    print("row %d: most common colour covers %.0f%%" % (row, top * 100))
    if top > 0.5: fail = 1
print("FAIL: the shadow under the dock is flat colour bands, not the photo darkened" if fail
      else "PASS: the shadow under the dock darkens the real photo, no flat bands")
sys.exit(fail)
