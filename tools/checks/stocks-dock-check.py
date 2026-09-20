#!/usr/bin/env python3
"""Headless proof that the Stocks app opens when clicked from the dock.
Models appclose-check.py's pattern: boots kernel.elf with -display none,
uses QMP absolute pointer to click the Stocks dock slot, pmemsaves the
framebuffer, and asserts that the Stocks window opened.

Stocks is now pinned as dock slot 9 (after Weather, before Trash).

Usage: tools/checks/stocks-dock-check.py   (from the repo root, after make kernel.elf)
"""
import json, os, socket, subprocess, sys, time
from PIL import Image

LOG = "/tmp/jt-stocks-dock-serial.log"
DUMP = "/tmp/jt-stocks-dock.raw"
FB = 0xfd000000; W, H = 1920, 1080
PORT = 4456
LOGICAL_W, LOGICAL_H, SCALE = 960, 540, 2
DOCK_ICON, DOCK_GAP, SLOT0_X = 37, 6, 247
PITCH = DOCK_ICON + DOCK_GAP
ICON_ROW_Y = 487
STOCKS_SLOT = 9              # after Weather (8), before Trash (10)
CLOSE_X, CLOSE_Y = 94, 56   # gui_launch_from_dock: red circle at (x+24, y+16) for x=70, y=40
CLOSE_RED = (0xFF, 0x5F, 0x57)

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

    def move(x, y):
        cmd({"execute": "input-send-event", "arguments": {"events": [
            {"type": "abs", "data": {"axis": "x", "value": int(x * 32768 / LOGICAL_W)}},
            {"type": "abs", "data": {"axis": "y", "value": int(y * 32768 / LOGICAL_H)}}]}})
    def click():
        cmd({"execute": "input-send-event", "arguments": {"events": [{"type": "btn", "data": {"down": True, "button": "left"}}]}})
        time.sleep(0.1)
        cmd({"execute": "input-send-event", "arguments": {"events": [{"type": "btn", "data": {"down": False, "button": "left"}}]}})
    def pixel(x, y):
        cmd({"execute": "pmemsave", "arguments": {"val": FB, "size": W * H * 4, "filename": DUMP}})
        img = Image.frombytes("RGBA", (W, H), open(DUMP, "rb").read(), "raw", "BGRA").convert("RGB")
        return img.getpixel((x * SCALE + 1, y * SCALE + 1))  # +1: inside the s x s block, never its seam
    def is_red(p): return max(abs(p[i] - CLOSE_RED[i]) for i in range(3)) <= 12

    f.readline()
    cmd({"execute": "qmp_capabilities"})
    time.sleep(5.0)  # desktop up

    centre = lambda slot: SLOT0_X + slot * PITCH + DOCK_ICON // 2

    # Click Stocks dock slot
    move(centre(STOCKS_SLOT), ICON_ROW_Y)
    time.sleep(0.3)
    click()
    time.sleep(1.0)  # app launch and render

    # Check for the close button (red circle)
    p = pixel(CLOSE_X, CLOSE_Y)
    if is_red(p):
        print(f"PASS: Stocks app opened, close button visible at ({CLOSE_X},{CLOSE_Y}): {p}")
        sys.exit(0)
    else:
        print(f"FAIL: Stocks app did not open, expected red close button at ({CLOSE_X},{CLOSE_Y}), got {p}")
        sys.exit(1)

finally:
    try: cmd({"execute": "quit"})
    except (ConnectionResetError, BrokenPipeError, OSError): pass
    try: q.wait(timeout=5)
    except subprocess.TimeoutExpired: q.kill()
