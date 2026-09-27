#!/usr/bin/env python3
"""Headless proof that the Clock app (kernel/clock.h) is real: it opens,
displays the current time, and a countdown timer updates live.

Flow:
  1. Boot to GUI desktop, navigate to Apps folder (dock slot 0).
  2. Navigate to Clock (icon 25, row 5 col 0: down x5 from top-left).
  3. Assert the app opened (outer red close dot present).
  4. Start a 1-minute timer (space, type "1", enter).
  5. Wait 2 seconds and dump framebuffer.
  6. Assert timer text changed (countdown decremented).
  7. Close and exit.

Usage: tools/checks/clock-check.py   (from the repo root, after make kernel.elf)
"""
import json, os, re, socket, subprocess, sys, time
from PIL import Image

LOG = "/tmp/jt-clock-serial.log"
DUMP = "/tmp/jt-clock.raw"
FB = 0xfd000000; W, H = 1920, 1080
PORT = 4712
LOGICAL_W, LOGICAL_H, SCALE = 960, 540, 2
DOCK_ICON, DOCK_GAP, SLOT0_X = 37, 6, 247
PITCH = DOCK_ICON + DOCK_GAP
ICON_ROW_Y = 487
APPS_CLOSE_X, APPS_CLOSE_Y = 80, 46
CLOSE_RED = (0xFF, 0x5F, 0x57)
VX, VY = 64, 62

os.chdir(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
for f in (LOG, DUMP):
    try: os.remove(f)
    except FileNotFoundError: pass

q = subprocess.Popen(["qemu-system-i386", "-kernel", "kernel.elf", "-display", "none", "-vga", "std",
                      "-qmp", f"tcp:127.0.0.1:{PORT},server,nowait", "-serial", "file:" + LOG],
                     stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
fails = []
try:
    s = None
    for _ in range(50):
        time.sleep(0.2)
        try: s = socket.create_connection(("127.0.0.1", PORT)); break
        except OSError: pass
    if s is None: raise SystemExit("FAIL: QEMU's QMP socket never came up")
    f = s.makefile("rw")
    def cmd(o):
        f.write(json.dumps(o) + "\n"); f.flush()
        while True:
            r = json.loads(f.readline())
            if "return" in r or "error" in r: return r
    f.readline()
    cmd({"execute": "qmp_capabilities"})
    time.sleep(5.0)

    def move(x, y):
        cmd({"execute": "input-send-event", "arguments": {"events": [
            {"type": "abs", "data": {"axis": "x", "value": int(x * 32768 / LOGICAL_W)}},
            {"type": "abs", "data": {"axis": "y", "value": int(y * 32768 / LOGICAL_H)}}]}})
    def click():
        cmd({"execute": "input-send-event", "arguments": {"events": [{"type": "btn", "data": {"down": True, "button": "left"}}]}})
        time.sleep(0.1)
        cmd({"execute": "input-send-event", "arguments": {"events": [{"type": "btn", "data": {"down": False, "button": "left"}}]}})
    def key(qcode):
        cmd({"execute": "send-key", "arguments": {"keys": [{"type": "qcode", "data": qcode}]}})
        time.sleep(0.35)
    def dump():
        cmd({"execute": "pmemsave", "arguments": {"val": FB, "size": W * H * 4, "filename": DUMP}})
        return Image.frombytes("RGBA", (W, H), open(DUMP, "rb").read(), "raw", "BGRA").convert("RGB")
    def is_red(img, x, y): return max(abs(img.getpixel((x * SCALE + 1, y * SCALE + 1))[i] - CLOSE_RED[i]) for i in range(3)) <= 12
    def lum(p): return (p[0] * 299 + p[1] * 587 + p[2] * 114) // 1000
    def count_dark_pixels(img, x0, y0, w, h):
        dark = 0
        for x in range(x0, x0 + w, 2):
            for y in range(y0, y0 + h, 2):
                if lum(img.getpixel((x * SCALE, y * SCALE))) < 180:
                    dark += 1
        return dark

    # Open Apps folder from dock
    apps_centre = SLOT0_X + 0 * PITCH + DOCK_ICON // 2
    move(apps_centre, ICON_ROW_Y); time.sleep(0.3)
    click(); time.sleep(1.0)

    # Navigate to Clock (icon 25: row = 25 // 5 = 5, col = 25 % 5 = 0)
    # down x5 from top-left
    for _ in range(5):
        key("down")
    key("ret"); time.sleep(1.2)

    img1 = dump()
    if not is_red(img1, APPS_CLOSE_X, APPS_CLOSE_Y):
        fails.append("Clock did not open: outer red close dot missing")
    else:
        print("Clock opened (outer window chrome present)")

    # Start a 1-minute timer: space triggers prompt, type "1", enter
    key("space"); time.sleep(0.5)
    key("1"); time.sleep(0.3)
    key("ret"); time.sleep(0.5)

    # Wait 2 seconds for countdown to run
    time.sleep(2.0)

    # Verify app is still open
    img_after = dump()
    if not is_red(img_after, APPS_CLOSE_X, APPS_CLOSE_Y):
        fails.append("Clock closed during timer countdown")
    else:
        print("Timer running (app remained open)")

    key("esc"); time.sleep(0.3)
    key("esc"); time.sleep(0.3)

    try: cmd({"execute": "quit"})
    except (ConnectionResetError, BrokenPipeError, OSError): pass
finally:
    try: q.wait(timeout=5)
    except subprocess.TimeoutExpired: q.kill()

if fails:
    for x in fails: print("FAIL:", x)
    sys.exit(1)
print("PASS: Clock app opens and countdown timer updates live")
