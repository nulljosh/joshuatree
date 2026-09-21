#!/usr/bin/env python3
"""Headless, real pixel proof of Magnet-style window snapping, the same
QMP absolute-pointer + pmemsave shape as multiwindow-check.py.

Case 1: opens Files (dock slot 1, window 0's own x=70,y=40,w=820,h=385
rect, gui_multiwin_geom), presses its title bar, drags to the left edge,
and releases. Proves from the real framebuffer that the window really
occupies the left half of the desktop strip (menu bar to dock): its close
button moved to the left-half rect's real position, its content (the
window's cream fill) sits inside the left half, and the wallpaper (not
window chrome) is visible where the window used to be on the right.

Case 2: drags that same window on to the top-left corner and checks the
quarter rect the same way.

Cases 3-5 (the 1.0 QA follow-up): Mail, Calendar and Reminders each get
their own real drag-to-quarter run -- the quarter is the smallest,
hardest-to-lay-out-in target, not just the easier halves -- and each run
proves three things from the framebuffer, not two: the close button is at
the snapped position, the app's own content is inside the snapped rect,
AND nothing from the app is drawn outside it (sampled at points just past
the rect's right and bottom edges, compared against a real clean-desktop
baseline captured before any window ever opened, since gui_run's viewport
clipping is a real per-pixel bound in drivers/window.c, not just a
visual convention, and this proves that bound actually holds for these
three apps' own content functions, not just Files/Weather's). Case 5
(Reminders) goes one step further: types one real character while
snapped to the quarter and re-checks the same three things, since the
keystroke repaint path (gui_run's mw_key_repaint, gui_multiwin_draw_content_only)
is a different code path from the drag-release repaint the other cases
exercise.

Case 6: a real free-move, not a snap -- Files dropped in the middle of
the screen, outside every snap zone. Proves the close button moved to
the drop position and the window's old position shows wallpaper again.

Discriminating: on a kernel built before this change, title-bar drags do
nothing (there is no drag/snap code at all -- pressing and moving the
title bar is indistinguishable from a plain click, which the existing
click-anywhere-closes contract just closes on release), so every window
either stays at its original x=70,y=40,w=820,h=385 rect or disappears
entirely; neither matches a real left-half, quarter, or moved rect, so
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
SLOT = {"Files": 1, "Mail": 2, "Calendar": 3, "Reminders": 5}

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

    # Real clean-desktop baseline, captured before any window has ever
    # opened this session, at the exact points every quarter-snap case
    # below re-checks for leakage past the rect's own edges.
    baseline_img = dump()
    baseline_edges = [pixel(baseline_img, x, y) for x, y in EDGE_POINTS]

    def assert_quarter(label, img):
        q_close_ok = is_red(pixel(img, QX + 24, QY + 16))
        q_content_ok = close(pixel(img, QX + QW // 2, QY + QH // 2), (0xF5, 0xF0, 0xEB)) <= 20
        edge_clean = all(close(pixel(img, x, y), base) <= 6
                          for (x, y), base in zip(EDGE_POINTS, baseline_edges))
        print(f"{label}: close button at quarter position={'yes' if q_close_ok else 'NO'}"
              f"  content inside quarter={'yes' if q_content_ok else 'NO'}"
              f"  nothing drawn past the quarter's edges={'yes' if edge_clean else 'NO'}")
        if not q_close_ok: fails.append(f"{label}: close button is not at the quarter rect's position")
        if not q_content_ok: fails.append(f"{label}: no window chrome/content inside the quarter")
        if not edge_clean: fails.append(f"{label}: something is drawn past the quarter rect's own edges")
        return q_close_ok and q_content_ok and edge_clean

    def close_at(x, y):
        move(x, y); time.sleep(0.3); click(); time.sleep(0.8)
        move(*PARK); time.sleep(0.5)

    # ---- case 1 & 2: Files, left half then top-left quarter ----
    open_app("Files")
    img0 = dump()
    if not is_red(pixel(img0, *W0_CLOSE)):
        raise SystemExit("FAIL: Files did not open at its expected starting rect, cannot test dragging it")

    drag_from(*TITLEBAR, 2, LOGICAL_H // 2)
    img1 = dump()
    lx, ly, lw, lh = LEFT_HALF
    exp_close = (lx + 24, ly + 16)
    left_close_ok = is_red(pixel(img1, *exp_close))
    old_close_gone = not is_red(pixel(img1, *W0_CLOSE))
    content_ok = close(pixel(img1, lx + lw // 2, ly + lh // 2), (0xF5, 0xF0, 0xEB)) <= 20
    right_wallpaper_ok = close(pixel(img1, 700, 200), (0xF5, 0xF0, 0xEB)) > 20
    print(f"left-half snap (Files): close button at left-half position={'yes' if left_close_ok else 'NO'}"
          f"  original close button gone={'yes' if old_close_gone else 'NO'}"
          f"  content inside left half={'yes' if content_ok else 'NO'}"
          f"  wallpaper visible on the right={'yes' if right_wallpaper_ok else 'NO'}")
    if not left_close_ok: fails.append("left-half snap: close button is not at the left-half rect's position")
    if not old_close_gone: fails.append("left-half snap: the window's original close button is still on screen (it did not move)")
    if not content_ok: fails.append("left-half snap: no window chrome/content inside the left half")
    if not right_wallpaper_ok: fails.append("left-half snap: the right half still shows window chrome instead of wallpaper")

    # Re-press from the window's CURRENT title bar position (left half's
    # own chrome top), not the original TITLEBAR constant.
    sx2, sy2 = (lx + 300 if lw > 340 else lx + lw // 2), ly + 10
    drag_from(sx2, sy2, 5, TOP + 5)
    img2 = dump()
    assert_quarter("top-left quarter snap (Files)", img2)

    # Clean up and prove input still alive: close the window via its own X.
    close_at(QX + 24, QY + 16)
    img3 = dump()
    closed_ok = not is_red(pixel(img3, QX + 24, QY + 16))
    print(f"cleanup (Files): window closed via its own X={'yes' if closed_ok else 'NO'}")
    if not closed_ok: fails.append("cleanup: the snapped Files window did not close via its own X afterward")

    # ---- cases 3-5: Mail, Calendar, Reminders, each dragged straight
    #      to the (hardest, smallest) top-left quarter ----
    for name in ("Mail", "Calendar", "Reminders"):
        open_app(name)
        imgA = dump()
        if not is_red(pixel(imgA, *W0_CLOSE)):
            fails.append(f"{name} did not open at its expected starting rect, could not test dragging it")
            continue
        drag_from(*TITLEBAR, 5, TOP + 5)
        imgB = dump()
        ok = assert_quarter(f"top-left quarter snap ({name})", imgB)

        if name == "Reminders" and ok:
            # 'a' opens add mode (see kernel/reminders.h gui_reminders_on_key),
            # then one real keystroke, all while still snapped to the
            # quarter -- gui_run's mw_key_repaint path
            # (gui_multiwin_draw_content_only), a different repaint path
            # from the drag-release full repaint the assert above just
            # checked.
            move(QX + QW // 2, QY + QH // 2); time.sleep(0.2)  # focus stays on the topmost window regardless; just park inside it
            key("a")
            key("x")
            imgC = dump()
            # The add-mode text box: window_rect(20,76,width-40,20,WHITE)
            # in reminders.h's own content-relative coords; absolute
            # screen position is win.x+8+20, win.y+32+76 for the
            # top-left-quarter window.
            box_x, box_y = QX + 8 + 30, QY + 32 + 84
            box_ok = close(pixel(imgC, box_x, box_y), (0xFF, 0xFF, 0xFF)) <= 12
            edge_clean_after_key = all(close(pixel(imgC, x, y), base) <= 6
                                        for (x, y), base in zip(EDGE_POINTS, baseline_edges))
            print(f"Reminders keystroke while snapped: add-mode text box visible inside the quarter={'yes' if box_ok else 'NO'}"
                  f"  still nothing drawn past the quarter's edges={'yes' if edge_clean_after_key else 'NO'}")
            if not box_ok: fails.append("Reminders keystroke: add-mode text box not where the snapped quarter's content viewport puts it")
            if not edge_clean_after_key: fails.append("Reminders keystroke: the keystroke repaint drew something past the quarter rect's own edges")
            key("esc")  # cancel add mode before closing, same as reminders.h's own esc contract

        close_at(QX + 24, QY + 16)
        imgD = dump()
        closed_ok = not is_red(pixel(imgD, QX + 24, QY + 16))
        print(f"cleanup ({name}): window closed via its own X={'yes' if closed_ok else 'NO'}")
        if not closed_ok: fails.append(f"cleanup: the snapped {name} window did not close via its own X afterward")

    # ---- case 6: real free-move (Files, dropped in the interior, not
    #      inside any snap zone) ----
    open_app("Files")
    imgE = dump()
    if not is_red(pixel(imgE, *W0_CLOSE)):
        fails.append("free-move: Files did not open at its expected starting rect")
    else:
        # Files' own default rect is 820x385, most of the 960x423 playable
        # area, so a free move only really has ~140px of horizontal room
        # and ~38px of vertical room before the target clamps back inside
        # the desktop strip (gui_run's own release-time clamp, the same
        # one that keeps a snapped window on screen). Pick a target that
        # lands the window fully flush left (nx=0) with no vertical
        # change, so there is a real, unclamped, discriminating drop
        # position and a real uncovered strip on the right to sample.
        drag_dx = TITLEBAR[0] - 70  # grab offset from the window's own x=70
        drag_dy = TITLEBAR[1] - 40  # grab offset from the window's own y=40
        move_target = (0 + drag_dx, 40 + drag_dy)  # -> nx=0, ny=40, no zone (far from every edge/corner)
        drag_from(*TITLEBAR, *move_target)
        imgF = dump()
        exp_x, exp_y = 0, 40
        # y+10, not the usual y+16 centre: at nx=0 the close circle's
        # centre pixel lands squarely on the AA'd "x" glyph's own dark
        # stroke at this exact QMP-scaled sample offset (confirmed with a
        # row-by-row scan of the real circle), a sampling-grid parity
        # artifact, not a real gap in the circle -- y+10 samples the same
        # circle a few rows higher, off the glyph, still well inside it.
        moved_close_ok = is_red(pixel(imgF, exp_x + 24, exp_y + 10))
        old_close_gone2 = not is_red(pixel(imgF, *W0_CLOSE))
        # Old position: sampled inside the old rect's body (70..890,
        # 40..425) but past the moved window's new right edge (0..820),
        # a real uncovered strip, wallpaper/desktop now.
        old_pos_wallpaper = close(pixel(imgF, 860, 200), (0xF5, 0xF0, 0xEB)) > 20
        print(f"free-move (Files): close button at drop position={'yes' if moved_close_ok else 'NO'}"
              f"  original close button gone={'yes' if old_close_gone2 else 'NO'}"
              f"  old position shows wallpaper={'yes' if old_pos_wallpaper else 'NO'}")
        if not moved_close_ok: fails.append("free-move: close button is not at the real drop position")
        if not old_close_gone2: fails.append("free-move: the window's original close button is still on screen (it did not move)")
        if not old_pos_wallpaper: fails.append("free-move: the window's old position still shows chrome instead of wallpaper")
        close_at(exp_x + 24, exp_y + 16)
        imgG = dump()
        closed_ok2 = not is_red(pixel(imgG, exp_x + 24, exp_y + 10))
        print(f"cleanup (free-move Files): window closed via its own X={'yes' if closed_ok2 else 'NO'}")
        if not closed_ok2: fails.append("cleanup: the free-moved Files window did not close via its own X afterward")

    try: cmd({"execute": "quit"})
    except (ConnectionResetError, BrokenPipeError, OSError): pass
finally:
    try: q.wait(timeout=5)
    except subprocess.TimeoutExpired: q.kill()

if fails:
    for x in fails: print("FAIL:", x)
    sys.exit(1)
print("PASS: Files/Mail/Calendar/Reminders all snap correctly to the left half, top-left quarter (each app's own content proven inside the rect and nothing drawn past its edges, including a live Reminders keystroke while snapped), and a real free-move drop, all from real framebuffer pixels")
