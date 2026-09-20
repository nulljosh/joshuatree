#!/usr/bin/env python3
"""Regression test for the v86/0.71.0 "launchpad apps don't open" bug:
real root cause, confirmed by reading kernel/kernel.c's gui_launch_apps,
was that EVERY click reaching the Apps-folder grid screen was treated as
"close the folder", with no hit test at all against the tile cells drawn
right above it (unlike the dock, whose tiles are hit-tested by
gui_dock_hit_test). Clicking a fleet app tile therefore only ever
dismissed the launchpad; only the keyboard path (arrows+Enter, or digits
'1'-'9' for the first 9 of 22 apps) could actually launch anything.

Reproduces headlessly with the same QMP absolute-pointer pattern as
tools/checks/dockhover-check.py: open the dock's Apps-folder tile (dock slot 0),
wait for the grid to render, click squarely on a fleet-app tile (Curbfind,
grid index 8), then check a real, unambiguous pixel: the outer window's
red traffic-light dot at (80,46) in 960x540 logical space (gui_launch_
from_dock draws it once for the whole windowed app session, whether the
Apps folder or something launched from inside it is currently showing).

  - Before the fix: the click just closes the folder -> back to the full
    desktop -> that pixel is desktop/menubar colour, not the red dot.
  - After the fix: the click launches Curbfind inside the SAME outer
    window -> the red dot is still exactly there.

Proven discriminating below (temporarily reverting the kernel.c hit-test
block makes this fail, restoring it makes it pass again).

Usage: tools/checks/launchpad-click-check.py   (from repo root, after make kernel.elf)
"""
import json, os, socket, subprocess, sys, time
from PIL import Image

LOG = "/tmp/jt-launchpad-serial.log"
DUMP = "/tmp/jt-launchpad.raw"
FB = 0xfd000000; W, H = 1920, 1080
PORT = 4452
LOGICAL_W, LOGICAL_H, SCALE = 960, 540, 2
DOCK_ICON, DOCK_GAP, SLOT0_X = 37, 6, 247
PITCH = DOCK_ICON + DOCK_GAP
ICON_ROW_Y = 487
APPS_FOLDER_SLOT = 0  # GUI_DOCK_DEFAULT[0] == GUI_APPS_FOLDER
RED_DOT = (56 + 24, 30 + 16)  # gui_launch_from_dock's fixed traffic-light red circle, logical screen coords
# Curbfind (grid index 8, row 1 col 3) tile centre in the apps-folder
# viewport, converted to full-screen logical coords by hand from
# gui_launch_apps's own layout math (window x=56,y=30,w=848,h=490 ->
# viewport origin 64,62,832,450; x0=(832-750)/2=41, y0=95; cell_w=150,
# cell_h=108, tile=60; row=1,col=3 -> cx=566,cy=203; a point comfortably
# inside that cell's hit box).
CURBFIND_CLICK = (64 + 566, 62 + 240)

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
    time.sleep(5.0)  # desktop up

    def move(x, y):
        cmd({"execute": "input-send-event", "arguments": {"events": [
            {"type": "abs", "data": {"axis": "x", "value": int(x * 32768 / LOGICAL_W)}},
            {"type": "abs", "data": {"axis": "y", "value": int(y * 32768 / LOGICAL_H)}}]}})
    def click():
        cmd({"execute": "input-send-event", "arguments": {"events": [
            {"type": "btn", "data": {"down": True, "button": "left"}}]}})
        time.sleep(0.12)
        cmd({"execute": "input-send-event", "arguments": {"events": [
            {"type": "btn", "data": {"down": False, "button": "left"}}]}})
    def dump(path):
        cmd({"execute": "pmemsave", "arguments": {"val": FB, "size": W * H * 4, "filename": path}})
    centre = lambda slot: SLOT0_X + slot * PITCH + DOCK_ICON // 2

    # Open the Apps folder from the dock.
    move(centre(APPS_FOLDER_SLOT), ICON_ROW_Y); time.sleep(0.4)
    click(); time.sleep(1.0)
    # Click squarely on the Curbfind tile.
    move(*CURBFIND_CLICK); time.sleep(0.3)
    click(); time.sleep(0.8)
    dump(DUMP)
    # QEMU can tear down the QMP socket the instant it processes quit,
    # before this side ever reads a reply -- a real race, not a bug in
    # the assertions above (which already ran); a reset here must not
    # mask a genuine PASS as a crash.
    try: cmd({"execute": "quit"})
    except (ConnectionResetError, BrokenPipeError, OSError): pass
finally:
    try: q.wait(timeout=5)
    except subprocess.TimeoutExpired: q.kill()

def load(path):
    return Image.frombytes("RGBA", (W, H), open(path, "rb").read(), "raw", "BGRA").convert("RGB")

img = load(DUMP)
# Sample a small box around the red dot rather than one pixel: the cursor
# sprite (moved away from the dock after the tile click, but still
# somewhere on screen) or antialiasing can land exactly on a single
# sample point. Any pixel in the box being solidly red is enough.
cx, cy = RED_DOT[0] * SCALE, RED_DOT[1] * SCALE
box = [img.getpixel((x, y)) for y in range(cy - 6, cy + 6) for x in range(cx - 6, cx + 6)]
best = max(box, key=lambda p: p[0] - p[1] - p[2])
print("reddest pixel near the red-dot position after clicking a launchpad tile:", best)
is_red = best[0] > 200 and best[1] < 140 and best[2] < 140
if is_red:
    print("PASS: outer window chrome still present -> the launchpad tile click launched the app instead of just closing the folder")
    sys.exit(0)
else:
    print("FAIL: red traffic-light dot is gone -> the click closed the whole window instead of opening the tapped app (launchpad clicks don't open apps)")
    sys.exit(1)
