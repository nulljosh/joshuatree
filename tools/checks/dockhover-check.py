#!/usr/bin/env python3
"""Headless proof of the dock hover path, the same shape as vmmouse-check.sh:
boots kernel.elf with -display none, drives the pointer over QMP absolute
events (the v62 vmmouse path, the first synthetic input that reliably
reaches this kernel's GUI loop headlessly) across the dock exactly the way
roadmap.md's recording-evidence entry describes (park on one icon, then
hop across four more), then pmemsaves the real framebuffer mid-animation
and again after it has settled, and asserts on actual pixels:

  1. no tile is missing from the tray in either dump (the v63 finding: the
     offscreen band compose was silently failing its kmalloc at 1920x1080
     and every hover step repainted the visible tray from scratch, so a
     dump taken mid-step caught an empty tray; this one is timing-based,
     it catches the old kernel most runs, not every run);
  2. the settled frame lifts exactly the slot under the cursor, not the
     one to its left (the v63 gui_slot_at fix; deterministic, the old
     kernel fails it every run), and no icon more than one hop behind the
     cursor is still lifted mid-animation (the original report's claim,
     which the kernel's own animation state never actually did).

Geometry is derived from the kernel's own constants for a 960x540 logical
window at scale 2 (gui_run's window_open_scaled), dock_scale_pct 7,
GUI_ICON_COUNT 11: DOCK_ICON 37, tray x 258..702, slot pitch 43, slot s
tile spans x 268+43s .. 304+43s, icon row y 469..506, magnified+lifted
top at y 450. Update the numbers below if any of those change.

Usage: tools/checks/dockhover-check.py   (from the repo root, after make kernel.elf)
"""
import json, os, socket, subprocess, sys, time
from PIL import Image

LOG = "/tmp/jt-dockhover-serial.log"
REST, MID, END = "/tmp/jt-dockhover-rest.raw", "/tmp/jt-dockhover-mid.raw", "/tmp/jt-dockhover-end.raw"
FB = 0xfd000000; W, H = 1920, 1080
PORT = 4449
LOGICAL_W, LOGICAL_H, SCALE = 960, 540, 2
DOCK_ICON, DOCK_GAP, SLOT0_X, ICONS = 37, 6, 247, 11
PITCH = DOCK_ICON + DOCK_GAP
ICON_ROW_Y = 487      # inside the normal-size tile
LIFTED_Y = 455        # above a normal tile's top (469), inside a magnified+lifted one (450..)
TRAY = (239, 235, 228)
LABEL_BG = (0xF4, 0xF1, 0xEC)  # DOCK_LABEL_BG in kernel.c
LABEL_Y = 446                  # hover label capsule centre line: gui_dock_y0() (459) - 21 + 8

os.chdir(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
for f in (LOG, MID, END):
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
    def dump(path):
        cmd({"execute": "pmemsave", "arguments": {"val": FB, "size": W * H * 4, "filename": path}})
    centre = lambda slot: SLOT0_X + slot * PITCH + DOCK_ICON // 2

    move(480, 200); time.sleep(0.3); dump(REST)           # cursor nowhere near the dock: nothing lifted
    move(centre(1), ICON_ROW_Y); time.sleep(0.6)          # Files fully lifted
    for slot in (2, 3, 4, 5):
        move(centre(slot), ICON_ROW_Y); time.sleep(0.12)  # hop across four more
    time.sleep(0.05); dump(MID)                           # slot 5 still growing, 4 still shrinking
    time.sleep(0.8);  dump(END)                           # settled
    # QEMU can tear down the QMP socket the instant it processes quit,
    # before this side ever reads a reply -- a real race, not a bug in
    # the assertions above (which already ran); a reset here must not
    # mask a genuine PASS as a crash.
    try: cmd({"execute": "quit"})
    except (ConnectionResetError, BrokenPipeError, OSError): pass
finally:
    try: q.wait(timeout=5)
    except subprocess.TimeoutExpired: q.kill()

fail = 0
def load(path):
    return Image.frombytes("RGBA", (W, H), open(path, "rb").read(), "raw", "BGRA").convert("RGB")
def tile_present(img, slot):
    """A tile is present when most of its 37x37 box is not tray colour. An
    absent tile is exactly tray (0 pixels differ); a drawn one is >90%. The
    threshold is tight (12) because white glyph pixels sit within ~27 of
    the cream tray in every channel."""
    x0 = (SLOT0_X + slot * PITCH) * SCALE; y0 = 469 * SCALE
    n = DOCK_ICON * SCALE; non_tray = 0
    for y in range(y0, y0 + n):
        for x in range(x0, x0 + n):
            p = img.getpixel((x, y))
            if max(abs(p[i] - TRAY[i]) for i in range(3)) > 12: non_tray += 1
    return non_tray > n * n // 2
def lifted(img, slot):
    """Lifted when the row above a normal tile's top, across the tile's own
    width, differs from the same row in the resting frame.

    Measured against the rest frame, not against a brightness threshold.
    The threshold version assumed a dark wallpaper and broke every time the
    wallpaper changed tone (60, then 150, then for good once the pending-map
    placeholder became the baked satellite capture and every slot read as
    lifted). Row 455 is below the wind band (WIND_HORIZON_ROW 395), so with
    nothing lifted it is pixel-stable between frames; 24 clears any AA
    shimmer while a real lifted tile changes nearly every pixel."""
    x0 = (SLOT0_X + slot * PITCH) * SCALE; y = LIFTED_Y * SCALE
    changed = sum(1 for x in range(x0, x0 + DOCK_ICON * SCALE)
                  if max(abs(a - b) for a, b in zip(img.getpixel((x, y)), rest.getpixel((x, y)))) > 24)
    return changed > DOCK_ICON * SCALE // 3

rest = load(REST)
for tag, path in (("mid", MID), ("end", END)):
    img = load(path)
    missing = [s_ for s_ in range(ICONS) if not tile_present(img, s_)]
    lifted_slots = [s_ for s_ in range(ICONS) if lifted(img, s_)]
    print(f"{tag}: missing tiles {missing}, lifted slots {lifted_slots}")
    if missing:
        print(f"FAIL: {tag} frame has tiles missing from the tray (direct-repaint fallback caught mid-step)"); fail = 1
    if tag == "mid" and any(s_ < 4 for s_ in lifted_slots):
        print("FAIL: mid frame still shows an icon lifted more than one hop behind the cursor"); fail = 1
    if tag == "end":
        # The hover label's capsule is wider than one slot, so "which slots
        # changed" is 4..6 for a long name; what must hold is that the mark
        # is centred on the slot under the cursor.
        y = LABEL_Y * SCALE
        xs = [x for x in range(SLOT0_X * SCALE, (SLOT0_X + ICONS * PITCH) * SCALE)
              if max(abs(a - b) for a, b in zip(img.getpixel((x, y)), rest.getpixel((x, y)))) > 24]
        want = (SLOT0_X + 5 * PITCH + DOCK_ICON // 2) * SCALE
        got = (min(xs) + max(xs)) // 2 if xs else -1
        print(f"end: hover mark centred at x={got // SCALE}, slot 5 centre {want // SCALE}")
        if not xs or abs(got - want) > PITCH * SCALE // 2:
            print("FAIL: settled frame's hover label is not over the slot under the cursor (5)"); fail = 1
        # The label has a backing (DOCK_LABEL_BG capsule), not bare text on
        # the wallpaper: its light fill shows on both sides of the text.
        backing = sum(1 for x in xs if max(abs(img.getpixel((x, y))[i] - LABEL_BG[i]) for i in range(3)) <= 10)
        print(f"end: label backing pixels on row {LABEL_Y}: {backing}")
        if backing < 60:
            print("FAIL: hover label has no backing"); fail = 1

if not fail: print("PASS: dock hover, tray intact mid-animation, lifted icon is the one under the cursor")
sys.exit(fail)
