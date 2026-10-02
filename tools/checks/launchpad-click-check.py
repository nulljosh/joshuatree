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
wait for the grid to render, click squarely on a fleet-app tile (grid index 8,
row 1 col 3).

Since gate 5 the tile does not launch the app inside the folder any more: the
folder closes and the app opens as its own compositor window (a ring-3 window
task). So the assertions are the kernel's serial markers, in order:
  - "appsgridrepaint" (the folder grid was up when the click landed),
  - "ring3app: launching <APP>.BIN at ring 3 as a window",
  - "syscall: window opened for ring-3 task",
  - the app's own open marker "<app>: ring-3 window" written from ring 3.
Before the 0.62-era hit-test fix a tile click only dismissed the folder, so
none of the launch lines ever appeared.

Usage: tools/checks/launchpad-click-check.py   (from repo root, after make kernel.elf)
"""
import json, os, socket, subprocess, sys, time
from PIL import Image
from freeport import free_port

LOG = "/tmp/jt-launchpad-serial.log"
DUMP = "/tmp/jt-launchpad.raw"
FB = 0xfd000000; W, H = 1920, 1080
PORT = free_port()
LOGICAL_W, LOGICAL_H, SCALE = 960, 540, 2
DOCK_ICON, DOCK_GAP, SLOT0_X = 37, 6, 247
PITCH = DOCK_ICON + DOCK_GAP
ICON_ROW_Y = 487
APPS_FOLDER_SLOT = 0  # GUI_DOCK_DEFAULT[0] == GUI_APPS_FOLDER
# Grid index 8 (row 1 col 3, whichever fleet app sits there) tile centre in the apps-folder
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
    click()
    deadline = time.time() + 15
    while time.time() < deadline:
        time.sleep(0.25)
        if "window opened for ring-3 task" in open(LOG, errors="replace").read(): break
    time.sleep(1.0)  # let the app's own marker land after the window opens
    # QEMU can tear down the QMP socket the instant it processes quit,
    # before this side ever reads a reply -- a real race, not a bug in
    # the assertions above (which already ran); a reset here must not
    # mask a genuine PASS as a crash.
    try: cmd({"execute": "quit"})
    except (ConnectionResetError, BrokenPipeError, OSError): pass
finally:
    try: q.wait(timeout=5)
    except subprocess.TimeoutExpired: q.kill()

log = open(LOG, errors="replace").read()
import re
m = re.search(r"ring3app: launching (\w+)\.BIN at ring 3 as a window", log)
ok = True
if "appsgridrepaint" not in log:
    print("FAIL: the Apps folder grid never rendered, so the click did not test a tile"); ok = False
if not m:
    print("FAIL: no 'ring3app: launching <APP>.BIN at ring 3 as a window' after the tile click (the click just closed the folder)"); ok = False
else:
    app = m.group(1).lower()
    print("tile click launched:", app)
    if "window opened for ring-3 task" not in log[log.index(m.group(0)):]:
        print("FAIL: 'window opened for ring-3 task' missing after the launch line"); ok = False
    if (app + ": ring-3 window") not in log:
        print(f"FAIL: the app's own open marker '{app}: ring-3 window' never appeared"); ok = False
if ok:
    print("PASS: the launchpad tile click closed the folder and opened the app as a compositor window")
    sys.exit(0)
sys.exit(1)
