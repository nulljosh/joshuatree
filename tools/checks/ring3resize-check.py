#!/usr/bin/env python3
"""Ring-3 window resize, headless: snap Notes to the top-left quarter and
prove the app really re-laid out at the new size, not just got clipped.

The compositor notices the viewport no longer matches the window's private
framebuffer, queues JT_EV_RESIZE (new w,h) into that window's event ring,
and the app answers by calling SYS_WINDOW_OPEN again (jt_window_resized in
user/jtsys.h), which maps a fresh, zeroed buffer of the new size at
JT_USER_FB. Until it answers, the compositor keeps blitting the old buffer
clipped. This check asserts, after a title-bar drag to the quarter:
  1. the kernel logged "syscall: window resized" (a new buffer was mapped),
  2. the app logged "ring3: window now WxH" with the quarter's content size
     (window w-16 by h-40), the serial marker that it redrew at that size,
  3. real framebuffer pixels: Notes' own content colour sits just inside the
     quarter's far (bottom-right) corner, and nothing is drawn past it.

Usage: tools/checks/ring3resize-check.py   (from the repo root, after make kernel.elf)
"""
import json, os, re, socket, subprocess, sys, time
from PIL import Image
from freeport import free_port

LOG = "/tmp/jt-ring3resize-serial.log"
DUMP = "/tmp/jt-ring3resize.raw"
FB = 0xfd000000; W, H = 1920, 1080
PORT = free_port()
LOGICAL_W, LOGICAL_H, SCALE = 960, 540, 2
DOCK_ICON, DOCK_GAP, SLOT0_X = 37, 6, 247
PITCH = DOCK_ICON + DOCK_GAP
ICON_ROW_Y = 487
# Window 0's own original rect (gui_multiwin_geom slot 0, the rect every
# single-open app uses): x=70,y=40,w=820,h=385.
W0_CLOSE = (94, 56)
TITLEBAR = (70 + 300, 40 + 10)  # well inside the title bar band, off the traffic lights
CLOSE_RED = (0xFF, 0x5F, 0x57)
PARK = (480, 460)
# The desktop strip snapping lands inside: menu bar bottom (GUI_MENUBAR_H=26)
# to just above the dock (gui_dock_y0() - 10), matching gui_snap_area in
# kernel/kernel.c. Computed the same way here rather than hard-coded, since
# the dock's own height is a function of the icon count.
TOP = 26
DOCK_ICON_H, DOCK_PAD, DOCK_MARGIN_BOT = 37, 10, 24
BOTTOM = LOGICAL_H - DOCK_ICON_H - 2 * DOCK_PAD - DOCK_MARGIN_BOT - 10
LEFT_HALF = (0, TOP, LOGICAL_W // 2, BOTTOM - TOP)
TOP_LEFT_QUARTER = (0, TOP, LOGICAL_W // 2, (BOTTOM - TOP) // 2)
QX, QY, QW, QH = TOP_LEFT_QUARTER
# Sample points just past the quarter's own right/bottom edges, real
# desktop the whole time (never covered by any window's rect in any of
# these cases), checked against a clean-boot baseline below.
EDGE_POINTS = [(QX + QW + 20, QY + 40), (QX + 200, QY + QH + 15)]
# Dock slots (SLOTS order in appclose-check.py/multiwindow-check.py):
# 0 Apps, 1 Files, 2 Mail, 3 Calendar, 4 Notes, 5 Reminders, ...
SLOT = {"Burrow": 1, "Mail": 2, "Notes": 4}  # 1.9.12: Calendar is a ring-3 program with a fixed viewport now; 1.9.20 gave Notes compositor hooks so it stands in for Weather (a lone Notes click opens the blocking editor, so it is opened as a second window)
W1_CLOSE = (154, 116)             # window 1 (second concurrent window): x=130,y=100
NOTES_TITLEBAR = (130 + 300, 100 + 10)
BAND = (0xED, 0xE6, 0xDC)         # Notes browse view's selected-row band

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
    def button(down):
        cmd({"execute": "input-send-event", "arguments": {"events": [{"type": "btn", "data": {"down": down, "button": "left"}}]}})
    def click():
        button(True); time.sleep(0.1); button(False)
    def dump():
        cmd({"execute": "pmemsave", "arguments": {"val": FB, "size": W * H * 4, "filename": DUMP}})
        return Image.frombytes("RGBA", (W, H), open(DUMP, "rb").read(), "raw", "BGRA").convert("RGB")
    def pixel(img, x, y):
        return img.getpixel((x * SCALE + 1, y * SCALE + 1))
    def close(p1, p2): return max(abs(p1[i] - p2[i]) for i in range(3))
    def is_red(p): return close(p, CLOSE_RED) <= 12
    def key(qcode):
        cmd({"execute": "send-key", "arguments": {"keys": [{"type": "qcode", "data": qcode}]}})
        time.sleep(0.15)

    centre = lambda slot: SLOT0_X + slot * PITCH + DOCK_ICON // 2
    def open_app(name):
        move(centre(SLOT[name]), ICON_ROW_Y); time.sleep(0.3)
        click(); time.sleep(1.2)

    def drag_from(sx, sy, tx, ty):
        """Press at (sx,sy), move past the 8px threshold in a few real
        steps (so gui_run's drag-arm and zone-change logic both see real
        intermediate positions, not one teleport), then release at
        (tx,ty)."""
        move(sx, sy); time.sleep(0.3)
        button(True); time.sleep(0.15)
        steps = 6
        for i in range(1, steps + 1):
            mx = sx + (tx - sx) * i // steps
            my = sy + (ty - sy) * i // steps
            move(mx, my); time.sleep(0.15)
        time.sleep(0.2)
        button(False); time.sleep(0.8)
        move(*PARK); time.sleep(0.5)

    centre = lambda slot: SLOT0_X + slot * PITCH + DOCK_ICON // 2
    def open_app(name):
        move(centre(SLOT[name]), ICON_ROW_Y); time.sleep(0.3)
        click(); time.sleep(1.2)

    def drag_from(sx, sy, tx, ty):
        move(sx, sy); time.sleep(0.3)
        button(True); time.sleep(0.15)
        for i in range(1, 7):
            move(sx + (tx - sx) * i // 6, sy + (ty - sy) * i // 6); time.sleep(0.15)
        time.sleep(0.2)
        button(False); time.sleep(1.5)
        move(*PARK); time.sleep(0.5)

    def close_at(x, y):
        move(x, y); time.sleep(0.3); click(); time.sleep(0.8)
        move(*PARK); time.sleep(0.5)

    baseline_img = dump()
    baseline_edges = [pixel(baseline_img, x, y) for x, y in EDGE_POINTS]

    # Notes joins the compositor only as a second window: Files, Notes on top, close Files.
    open_app("Burrow"); open_app("Notes")
    close_at(*W0_CLOSE); close_at(*W0_CLOSE)
    imgA = dump()
    if not (is_red(pixel(imgA, *W1_CLOSE)) and not is_red(pixel(imgA, *W0_CLOSE))):
        raise SystemExit("FAIL: Notes did not end up alone at window 1's rect, cannot test resizing it")
    log0 = open(LOG, errors="replace").read()
    if "ring3: window now" in log0: fails.append("a resize was logged before any rect change")
    drag_from(*NOTES_TITLEBAR, 5, TOP + 5)
    imgB = dump()
    log = open(LOG, errors="replace").read()

    if "syscall: window resized" not in log: fails.append("the kernel never mapped a new buffer (no 'syscall: window resized')")
    m = re.findall(r"ring3: window now (\d+)x(\d+)", log)
    print(f"app resize markers: {m}")
    if not m: fails.append("Notes never logged 'ring3: window now WxH': it did not answer JT_EV_RESIZE")
    else:
        w, h = map(int, m[-1])
        if w != QW - 16 or abs(h - (QH - 40)) > 2: fails.append(f"Notes redrew at {w}x{h}, expected {QW - 16}x{QH - 40}")

    # Pixels: Notes' own page colour just inside the far corner, wallpaper/baseline untouched past it.
    far = pixel(imgB, QX + QW - 14, QY + QH - 14)
    notes_bg = pixel(imgB, QX + QW // 2, QY + QH - 14)
    print(f"far-corner pixel {far}, bottom-middle pixel {notes_bg}")
    if close(far, notes_bg) > 6 or close(far, baseline_edges[0]) <= 6: fails.append("the quarter's far corner is not Notes' content colour")
    edge_clean = all(close(pixel(imgB, x, y), base) <= 6 for (x, y), base in zip(EDGE_POINTS, baseline_edges))
    if not edge_clean: fails.append("something is drawn past the quarter's edges")
finally:
    q.kill()
if fails:
    for x in fails: print("FAIL:", x)
    sys.exit(1)
print("PASS: Notes snapped to a quarter, got JT_EV_RESIZE, mapped a new buffer, redrew at the quarter's size (serial marker) and its far-corner pixels are in the quarter")
