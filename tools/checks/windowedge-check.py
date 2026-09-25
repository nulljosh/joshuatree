#!/usr/bin/env python3
"""Headless proof of the v83 window-edge/corner-seam fix (gui_rounded_rect_on_
wallpaper, kernel.c), same shape as traycorner-check.py.

Real, confirmed bug (QA screenshot + macro crops, not a guess): the straight
top edge of a window's chrome got its own linear band-width AA blend (a
fixed weight per row, identical at every column), while the rounded corner
right next to it used a real per-pixel circle-coverage test. The two were
computed independently and never reconciled at the column where one hands
off to the other (px==pr), so the top edge read as a flat ~100%-background
blend at row 0 right up until the last column before the corner box, then
the corner's circle test jumped straight to ~50% coverage on the very next
column -- a hard, visible step/notch, plus the corner arc beyond it reading
as stair-stepped rather than smoothly graded, exactly Joshua's report
("window edges still pixely").

v83 fixes it by unifying both into ONE coverage test: the straight edge is
now the same circle test with its x-offset pinned to 0 (the same value the
corner test evaluates to exactly at the seam column), computed once per row
and reused across the whole flat run. There is no longer a second formula
to disagree with the first.

This check boots kernel.elf headless, opens the real Chat app window (dock
slot 7, same click as chatapp-check.py), pmemsaves the framebuffer, and
checks the window's actual top-right corner in physical pixels:
  (a) no step-pattern in the top-edge row as it approaches the corner --
      the row's blend value must move monotonically/smoothly, not jump by a
      large delta at the corner boundary column;
  (b) the corner arc itself has real intermediate (blended) pixels along
      its curve, not a pure binary staircase of full-color/full-background.

Discriminating: reverting to the old per-region formula (git stash this
file's kernel.c hunk) reintroduces both a large single-column jump in (a)
and, less reliably, can still pass (b) since the arc itself was already
subsampled -- (a) is the one that catches this exact regression.

Usage: tools/checks/windowedge-check.py   (from the repo root, after make kernel.elf)
"""
import json, os, socket, subprocess, sys, time
from PIL import Image

LOG = "/tmp/jt-windowedge-serial.log"; DUMP = "/tmp/jt-windowedge.raw"
FB = 0xfd000000; W, H = 1920, 1080; PORT = 4492
LOGICAL_W, LOGICAL_H = 960, 540
DOCK_ICON, DOCK_GAP, SLOT0_X = 37, 6, 247; PITCH = DOCK_ICON + DOCK_GAP; ICON_ROW_Y = 487
CLOSE_X, CLOSE_Y = 94, 56; CLOSE_RED = (0xFF, 0x5F, 0x57)

# Chat opens at logical x=70,y=40,w=820,h=385, scale=2 -> physical
# x0=140,y0=80,x1=1780,y1=850, r=18 logical -> pr=36 physical.
X0, Y0, X1 = 140, 80, 1780
PR = 36
CREAM = (245, 240, 235)

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
    if s is None: raise SystemExit("FAIL: no QMP")
    f = s.makefile("rw")
    def cmd(o):
        f.write(json.dumps(o) + "\n"); f.flush()
        while True:
            r = json.loads(f.readline())
            if "return" in r or "error" in r: return r
    f.readline(); cmd({"execute": "qmp_capabilities"})
    def move(x, y):
        cmd({"execute": "input-send-event", "arguments": {"events": [
            {"type": "abs", "data": {"axis": "x", "value": int(x * 32768 / LOGICAL_W)}},
            {"type": "abs", "data": {"axis": "y", "value": int(y * 32768 / LOGICAL_H)}}]}})
    def click():
        cmd({"execute": "input-send-event", "arguments": {"events": [{"type": "btn", "data": {"down": True, "button": "left"}}]}})
        time.sleep(0.1)
        cmd({"execute": "input-send-event", "arguments": {"events": [{"type": "btn", "data": {"down": False, "button": "left"}}]}})
    def dump():
        cmd({"execute": "pmemsave", "arguments": {"val": FB, "size": W * H * 4, "filename": DUMP}})
        return Image.frombytes("RGBA", (W, H), open(DUMP, "rb").read(), "raw", "BGRA").convert("RGB")
    def pixel(img, x, y): return img.getpixel((x, y))
    def logical_pixel(img, x, y): return img.getpixel((x * 2 + 1, y * 2 + 1))
    def is_red(p): return max(abs(p[i] - CLOSE_RED[i]) for i in range(3)) <= 12

    time.sleep(6.0)
    move(SLOT0_X + 7 * PITCH + DOCK_ICON // 2, ICON_ROW_Y); time.sleep(0.3); click()
    img = None
    for _ in range(40):
        time.sleep(0.1)
        img = dump()
        if is_red(logical_pixel(img, CLOSE_X, CLOSE_Y)): break
    else: raise SystemExit("FAIL: Chat window never opened from dock slot 7")
    time.sleep(0.5)
    img = dump()

    def gray(p): return (p[0] + p[1] + p[2]) / 3.0

    # (a) walk the top-edge row (py=0, y=Y0) across the last several
    # straight columns into the corner box (x approaching X1-PR) and
    # check no single-column jump exceeds a real antialiasing step.
    y = Y0
    vals = [gray(pixel(img, x, y)) for x in range(X1 - PR - 8, X1 - PR + 8)]
    max_jump = max(abs(vals[i + 1] - vals[i]) for i in range(len(vals) - 1))
    print("top-edge row gray values across the seam:", [round(v) for v in vals])
    print("max single-column jump:", max_jump)
    if max_jump > 40:
        fails.append("step/notch at the corner seam: max column-to-column jump %.1f (row y=%d)" % (max_jump, y))

    # (b) the corner arc must have real intermediate (blended) pixels,
    # not a binary staircase of pure color / pure background.
    blended = 0
    checked = 0
    for dy in range(2, PR - 2):
        for dx in range(2, PR - 2):
            x = X1 - PR + dx
            yy = Y0 + dy
            # only look near the true arc boundary (radius PR from the
            # corner's own centre) to avoid counting the solid interior
            cxp, cyp = X1 - PR, Y0 + PR
            d = ((x - cxp) ** 2 + (yy - cyp) ** 2) ** 0.5
            if abs(d - PR) > 3: continue
            checked += 1
            p = pixel(img, x, yy)
            g = gray(p)
            cream_g = gray(CREAM)
            if 15 < abs(g - cream_g) < cream_g - 15:
                blended += 1
    print("corner arc: %d/%d boundary samples show real intermediate blend" % (blended, checked))
    if checked == 0: fails.append("no corner boundary samples found to check")
    elif blended == 0: fails.append("corner arc has no intermediate (blended) pixels: pure binary staircase")

    try: cmd({"execute": "quit"})
    except (ConnectionResetError, BrokenPipeError, OSError): pass
finally:
    try: q.wait(timeout=5)
    except subprocess.TimeoutExpired: q.kill()

if fails:
    print("FAIL:")
    for m in fails: print(" -", m)
    sys.exit(1)
print("PASS: window top edge and corner arc are one continuous, antialiased shape")
