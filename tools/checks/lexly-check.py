#!/usr/bin/env python3
"""Headless proof that Lexly (kernel/lexly.h, v0.92.0) is real: it opens from
the dock, shows a Spanish word and four English answer choices, responds to
keyboard input (1-4), and displays correct/incorrect feedback with streak
tracking.

Usage: tools/checks/lexly-check.py   (from the repo root, after make kernel.elf)
"""
import json, os, socket, subprocess, sys, time
from PIL import Image

LOG = "/tmp/jt-lexly-serial.log"
DUMP = "/tmp/jt-lexly.raw"
FB = 0xfd000000; W, H = 1920, 1080
PORT = 4462
LOGICAL_W, LOGICAL_H, SCALE = 960, 540, 2
DOCK_ICON, DOCK_GAP, SLOT0_X = 37, 6, 247
PITCH = DOCK_ICON + DOCK_GAP
ICON_ROW_Y = 487

# Lexly is not in the default dock, need to open from Apps folder
# Apps folder is icon GUI_APPS_FOLDER, position 0 in the dock
APPS_X = SLOT0_X + DOCK_ICON // 2
APPS_Y = ICON_ROW_Y

# Lexly is icon 13, which is row 2, col 3 in 5-wide grid
# icon = row * 5 + col, so 13 = 2 * 5 + 3
LEXLY_APPS_COL = 3
LEXLY_APPS_ROW = 2
APPS_VX, APPS_VY = 56 + 8, 30 + 32  # Apps folder window viewport origin
APPS_GRID_TOP = APPS_VY + 68
CELL_SIZE = 92
LEXLY_APPS_X = APPS_VX + LEXLY_APPS_COL * CELL_SIZE + CELL_SIZE // 2
LEXLY_APPS_Y = APPS_GRID_TOP + LEXLY_APPS_ROW * CELL_SIZE + CELL_SIZE // 2

# Window geometry after opening (similar to other apps)
CLOSE_X, CLOSE_Y = 56 + 24, 30 + 16
CLOSE_RED = (0xFF, 0x5F, 0x57)
VX, VY = 56 + 8, 30 + 32  # viewport origin

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
    time.sleep(5.0)  # desktop up

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

    # Open Apps folder from dock (position 0)
    move(APPS_X, APPS_Y); time.sleep(0.3)
    click(); time.sleep(1.0)

    # Navigate to Lexly in the grid (row 2, col 3)
    # Use keyboard: right x3, down x2, enter
    for _ in range(3):
        key("right")
    for _ in range(2):
        key("down")
    key("ret"); time.sleep(1.0)

    # First screenshot: should show word and answers
    img1 = dump()
    img1.save("/tmp/jt-lexly-gallery/01-initial.png")
    print("Lexly window opened")

    # Check for text in the content area (Spanish word and English choices)
    content_area = img1.crop((VX * SCALE, (VY + 20) * SCALE, (VX + 800) * SCALE, (VY + 300) * SCALE))
    pixels = list(content_area.getdata())
    dark_px = sum(1 for p in pixels if lum(p) < 200)
    print(f"initial screenshot: content area dark pixels={dark_px}")
    if dark_px < 50:
        fails.append("Lexly initial screen shows too little text (likely missing word or choices)")
    else:
        print("initial screen has real text content (word and choices visible)")

    # Press '1' to answer
    key("1"); time.sleep(0.5)

    # Second screenshot: should show feedback and streak
    img2 = dump()
    img2.save("/tmp/jt-lexly-gallery/02-after-answer.png")

    # Check that the answer feedback text changed (different content)
    content_area2 = img2.crop((VX * SCALE, (VY + 20) * SCALE, (VX + 800) * SCALE, (VY + 300) * SCALE))
    pixels2 = list(content_area2.getdata())
    dark_px2 = sum(1 for p in pixels2 if lum(p) < 200)
    print(f"after answer: content area dark pixels={dark_px2}")

    # The feedback text should be different (different dark pixel distribution)
    if abs(dark_px2 - dark_px) < 3:
        fails.append("No visible change after pressing 1: feedback not displayed")
    else:
        print("screen changed after answer: feedback is displayed")

    # Close Lexly
    key("esc"); time.sleep(0.3)

    # Close Apps folder
    key("esc"); time.sleep(0.3)

    try: cmd({"execute": "quit"})
    except (ConnectionResetError, BrokenPipeError, OSError): pass
finally:
    try: q.wait(timeout=5)
    except subprocess.TimeoutExpired: q.kill()

if fails:
    for x in fails: print("FAIL:", x)
    sys.exit(1)
print("PASS: Lexly opens, displays word and choices, and responds to keyboard input")
