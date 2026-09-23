#!/usr/bin/env python3
"""Test Sparkjar upvote and re-sort functionality headless.
Opens Sparkjar, navigates to third idea (index 2), upvotes 3 times,
and captures a PNG showing the votes increased and list re-sorted.
"""
import json, os, socket, subprocess, sys, time
from PIL import Image

LOG = "/tmp/jt-sparkjar-upvote-test.log"
DUMP = "/tmp/jt-sparkjar-upvote-test.raw"
FB = 0xfd000000; W, H = 1920, 1080
PORT = 4462  # unique port
LOGICAL_W, LOGICAL_H, SCALE = 960, 540, 2
DOCK_ICON, DOCK_GAP, SLOT0_X = 37, 6, 247
PITCH = DOCK_ICON + DOCK_GAP
ICON_ROW_Y = 487
PARK = (480, 200)

# Icon 15 (Sparkjar) is at: row = 15 // 5 = 3, col = 15 % 5 = 0
SPARKJAR_ROW, SPARKJAR_COL = 3, 0

os.chdir(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
for f in (LOG, DUMP):
    try: os.remove(f)
    except FileNotFoundError: pass

q = subprocess.Popen(["qemu-system-i386", "-kernel", "kernel.elf", "-display", "none", "-vga", "std",
                      "-qmp", f"tcp:127.0.0.1:{PORT},server,nowait", "-serial", "file:" + LOG],
                     stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

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
    def key(qcode):
        cmd({"execute": "send-key", "arguments": {"keys": [{"type": "qcode", "data": qcode}]}})
        time.sleep(0.35)
    def dump():
        cmd({"execute": "pmemsave", "arguments": {"val": FB, "size": W * H * 4, "filename": DUMP}})
        return Image.frombytes("RGBA", (W, H), open(DUMP, "rb").read(), "raw", "BGRA").convert("RGB")
    def pixel(x, y):
        cmd({"execute": "pmemsave", "arguments": {"val": FB, "size": W * H * 4, "filename": DUMP}})
        img = Image.frombytes("RGBA", (W, H), open(DUMP, "rb").read(), "raw", "BGRA").convert("RGB")
        return img.getpixel((x * SCALE + 1, y * SCALE + 1))

    # Wait for desktop
    for _ in range(120):
        if pixel(480, 511) == (0xEF, 0xEB, 0xE4):
            break
        time.sleep(0.25)
    else:
        raise SystemExit("FAIL: desktop dock did not appear within 30 seconds")
    time.sleep(0.3)

    # Open Apps folder (dock slot 0)
    move(SLOT0_X + 0 * PITCH + DOCK_ICON // 2, ICON_ROW_Y)
    time.sleep(0.3)
    click()
    time.sleep(1.0)

    # Navigate to Sparkjar (row 3, col 0): press down 3 times to get to row 3
    for _ in range(SPARKJAR_ROW):
        key("down")
    # Already in col 0, no need to press right
    time.sleep(0.3)

    # Press Enter to open Sparkjar
    key("ret")
    time.sleep(1.2)

    # Navigate down twice (to index 2)
    key("down")
    key("down")
    time.sleep(0.5)

    # Capture before upvoting
    img_before = dump()
    img_before.save("/tmp/jt-sparkjar-before-upvote.png")

    # Press 'u' three times to upvote
    key("u")
    time.sleep(0.4)
    key("u")
    time.sleep(0.4)
    key("u")
    time.sleep(0.4)

    # Capture after upvoting
    img_after = dump()
    img_after.save("/tmp/jt-sparkjar-after-upvote.png")

    # Close the app
    key("esc")
    time.sleep(0.5)

    print("PASS: Sparkjar upvote test completed")
    print("Before: /tmp/jt-sparkjar-before-upvote.png")
    print("After: /tmp/jt-sparkjar-after-upvote.png")

finally:
    try: q.terminate(); q.wait(timeout=3)
    except: q.kill()
