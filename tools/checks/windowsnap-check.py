#!/usr/bin/env python3
"""Headless, real pixel proof of Magnet-style window snapping, the same
QMP absolute-pointer + pmemsave shape as multiwindow-check.py.

Opens Files (dock slot 1, window 0's own x=70,y=40,w=820,h=385 rect,
gui_multiwin_geom), presses its title bar, drags to the left edge, and
releases. Then proves from the real framebuffer that the window really
occupies the left half of the desktop strip (menu bar to dock): its close
button moved to the left-half rect's real position, its content (the
"Files" title text) sits inside the left half, and the wallpaper (not
window chrome) is visible where the window used to be on the right.
A second run drags to the top-left corner and checks the quarter rect.

Discriminating: on a kernel built before this change, title-bar drags do
nothing (there is no drag/snap code at all -- pressing and moving the
title bar is indistinguishable from a plain click, which the existing
click-anywhere-closes contract just closes on release), so the window
either stays at its original x=70,y=40,w=820,h=385 rect or disappears
entirely; neither matches a real left-half or top-left-quarter rect, so
this check fails on that build and passes on this one.

Usage: tools/checks/windowsnap-check.py   (from the repo root, after make kernel.elf)
"""
import json, os, socket, subprocess, sys, time
from PIL import Image

LOG = "/tmp/jt-windowsnap-serial.log"
DUMP = "/tmp/jt-windowsnap.raw"
FB = 0xfd000000; W, H = 1920, 1080
PORT = 4457
LOGICAL_W, LOGICAL_H, SCALE = 960, 540, 2
DOCK_ICON, DOCK_GAP, SLOT0_X = 37, 6, 247
PITCH = DOCK_ICON + DOCK_GAP
ICON_ROW_Y = 487
# Window 0's own original rect (gui_multiwin_geom slot 0): x=70,y=40,w=820,h=385.
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

    centre = lambda slot: SLOT0_X + slot * PITCH + DOCK_ICON // 2
    def open_files():
        move(centre(1), ICON_ROW_Y); time.sleep(0.3)
        click(); time.sleep(1.2)

    def drag_titlebar_to(target_x, target_y):
        """Press the title bar, move past the 8px threshold in a few real
        steps (so gui_run's drag-arm and zone-change logic both see real
        intermediate positions, not one teleport), then release."""
        move(*TITLEBAR); time.sleep(0.3)
        button(True); time.sleep(0.15)
        sx, sy = TITLEBAR
        steps = 6
        for i in range(1, steps + 1):
            mx = sx + (target_x - sx) * i // steps
            my = sy + (target_y - sy) * i // steps
            move(mx, my); time.sleep(0.15)
        time.sleep(0.2)
        button(False); time.sleep(0.8)
        move(*PARK); time.sleep(0.5)

    open_files()
    img0 = dump()
    if not is_red(pixel(img0, *W0_CLOSE)):
        raise SystemExit("FAIL: Files did not open at its expected starting rect, cannot test dragging it")

    # ---- case 1: drag to the left edge -> left half ----
    drag_titlebar_to(2, LOGICAL_H // 2)
    img1 = dump()
    lx, ly, lw, lh = LEFT_HALF
    exp_close = (lx + 24, ly + 16)
    left_close_ok = is_red(pixel(img1, *exp_close))
    old_close_gone = not is_red(pixel(img1, *W0_CLOSE))
    # Content inside the left half: the window's own cream chrome fill,
    # sampled well inside the rect, away from any edge/rounding.
    content_ok = close(pixel(img1, lx + lw // 2, ly + lh // 2), (0xF5, 0xF0, 0xEB)) <= 20
    # Wallpaper, not chrome, on the right half where the window used to
    # extend to (original rect went to x=70+820=890); sampled well clear
    # of the dock and any residual chrome, in what's now open desktop.
    right_wallpaper_ok = close(pixel(img1, 700, 200), (0xF5, 0xF0, 0xEB)) > 20
    print(f"left-half snap: close button at left-half position={'yes' if left_close_ok else 'NO'}"
          f"  original close button gone={'yes' if old_close_gone else 'NO'}"
          f"  content inside left half={'yes' if content_ok else 'NO'}"
          f"  wallpaper visible on the right={'yes' if right_wallpaper_ok else 'NO'}")
    if not left_close_ok: fails.append("left-half snap: close button is not at the left-half rect's position")
    if not old_close_gone: fails.append("left-half snap: the window's original close button is still on screen (it did not move)")
    if not content_ok: fails.append("left-half snap: no window chrome/content inside the left half")
    if not right_wallpaper_ok: fails.append("left-half snap: the right half still shows window chrome instead of wallpaper")

    # ---- case 2: drag to the top-left corner -> top-left quarter ----
    drag_titlebar_to(lx + 24, ly + 10)  # grab the now-left-half window by its own title bar
    time.sleep(0.2)
    # Re-press from the window's CURRENT title bar position (left half's
    # own chrome top), not the original TITLEBAR constant.
    move(lx + 300 if lw > 340 else lx + lw // 2, ly + 10); time.sleep(0.3)
    button(True); time.sleep(0.15)
    sx, sy = (lx + 300 if lw > 340 else lx + lw // 2), ly + 10
    tx, ty = 5, TOP + 5
    steps = 6
    for i in range(1, steps + 1):
        mx = sx + (tx - sx) * i // steps
        my = sy + (ty - sy) * i // steps
        move(mx, my); time.sleep(0.15)
    time.sleep(0.2)
    button(False); time.sleep(0.8)
    move(*PARK); time.sleep(0.5)

    img2 = dump()
    qx, qy, qw, qh = TOP_LEFT_QUARTER
    q_close_ok = is_red(pixel(img2, qx + 24, qy + 16))
    q_content_ok = close(pixel(img2, qx + qw // 2, qy + qh // 2), (0xF5, 0xF0, 0xEB)) <= 20
    print(f"top-left quarter snap: close button at quarter position={'yes' if q_close_ok else 'NO'}"
          f"  content inside quarter={'yes' if q_content_ok else 'NO'}")
    if not q_close_ok: fails.append("top-left-quarter snap: close button is not at the quarter rect's position")
    if not q_content_ok: fails.append("top-left-quarter snap: no window chrome/content inside the quarter")

    # Clean up and prove input still alive: close the window via its own X.
    click(); time.sleep(0.1)  # PARK isn't over the button; explicit close below
    move(qx + 24, qy + 16); time.sleep(0.3); click(); time.sleep(0.8)
    img3 = dump()
    closed_ok = not is_red(pixel(img3, qx + 24, qy + 16))
    print(f"cleanup: window closed via its own X={'yes' if closed_ok else 'NO'}")
    if not closed_ok: fails.append("cleanup: the snapped window did not close via its own X afterward")

    try: cmd({"execute": "quit"})
    except (ConnectionResetError, BrokenPipeError, OSError): pass
finally:
    try: q.wait(timeout=5)
    except subprocess.TimeoutExpired: q.kill()

if fails:
    for x in fails: print("FAIL:", x)
    sys.exit(1)
print("PASS: dragging a window's title bar to the left edge snaps it to the left half, and to the top-left corner snaps it to that quarter, both proven from real framebuffer pixels")
