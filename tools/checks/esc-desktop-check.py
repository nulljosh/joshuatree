#!/usr/bin/env python3
"""Regression check for the Esc-quits-the-desktop bug (v1.8.1).

Before this fix, Esc on a bare desktop (no window open) broke out of the
GUI to the text shell -- so a stray Esc, or three of them, silently
killed the desktop with no window ever having been focused. The fix
(kernel/kernel.c, gui_run) makes Esc on a bare desktop a no-op; only the
deliberate Ctrl+Alt+Backspace chord reaches the shell now. Esc still
closes the focused window when one is open, unchanged.

This proves the desktop survives: with nothing open, send Esc three
times, then click Mail on the dock and assert its window actually opens.
If the old bug were still there, the first Esc would already have quit
to the text shell, and the dock click that follows would land on nothing
QEMU ever draws again.

Same QMP absolute-pointer + pmemsave shape as appclose-check.py, kept
deliberately small: one boot, one assertion.

Usage: tools/checks/esc-desktop-check.py   (from the repo root, after make kernel.elf)
"""
import json, os, socket, subprocess, sys, time
from PIL import Image

LOG = "/tmp/jt-escdesktop-serial.log"
DUMP = "/tmp/jt-escdesktop.raw"
FB = 0xfd000000; W, H = 1920, 1080
PORT = 4452
LOGICAL_W, LOGICAL_H, SCALE = 960, 540, 2
DOCK_ICON, DOCK_GAP, SLOT0_X = 37, 6, 247
PITCH = DOCK_ICON + DOCK_GAP
ICON_ROW_Y = 487
CLOSE_X, CLOSE_Y = 94, 56          # gui_launch_from_dock: red circle at (x+24, y+16) for x=70, y=40
CLOSE_RED = (0xFF, 0x5F, 0x57)
MAIL_SLOT = 2                      # SLOTS = ["Apps", "Files", "Mail", ...] -- same order as appclose-check.py

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
        return img.getpixel((x * SCALE + 1, y * SCALE + 1))
    def is_red(p): return max(abs(p[i] - CLOSE_RED[i]) for i in range(3)) <= 12
    def window_open(): return is_red(pixel(CLOSE_X, CLOSE_Y))
    def keys(*qcodes):
        cmd({"execute": "send-key", "arguments": {"keys": [{"type": "qcode", "data": k} for k in qcodes]}})

    for _ in range(120):
        if pixel(480, 511) == (0xEF, 0xEB, 0xE4):
            break
        time.sleep(0.25)
    else:
        raise SystemExit("FAIL: desktop dock did not appear within 30 seconds")
    time.sleep(0.3)

    if window_open(): fails.append("desktop: red close button visible before anything was opened (sampling point is wrong)")

    # The regression itself: three bare Esc presses with no window open.
    for _ in range(3):
        keys("esc"); time.sleep(0.5)

    # If the old bug shipped, the first Esc already dropped to the text
    # shell and QEMU never draws the framebuffer again -- so the dock
    # pixel check below still stands as the real assertion, it just fails.
    centre = SLOT0_X + MAIL_SLOT * PITCH + DOCK_ICON // 2
    move(centre, ICON_ROW_Y); time.sleep(0.3)
    click()
    opened = False
    for _ in range(40):
        time.sleep(0.1)
        if window_open(): opened = True; break
    print(f"Mail open after 3x bare Esc: {'yes' if opened else 'NO'}")
    if not opened:
        fails.append("Esc on a bare desktop quit the GUI: Mail's dock click landed on nothing (desktop did not survive)")

    try: cmd({"execute": "quit"})
    except (ConnectionResetError, BrokenPipeError, OSError): pass
finally:
    try: q.wait(timeout=5)
    except subprocess.TimeoutExpired: q.kill()

if fails:
    for x in fails: print("FAIL:", x)
    sys.exit(1)
print("PASS: three bare Esc presses on an empty desktop did not quit the GUI; Mail still opens from the dock")
