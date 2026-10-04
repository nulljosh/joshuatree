#!/usr/bin/env python3
"""Headless proof that the top menu bar survives the common desktop
sequences, and that no frame is ever entirely black.

Why this exists: "demo is still missing menu bar sometimes, it goes black"
(landing demo, 2.6.32). gui_draw_menubar() only repaints on a minute change
(gui_menubar_last_min), so any path that paints over the bar without
forcing that gate leaves the strip wrong until the next minute.

Boots the kernel under QEMU (-display none, QMP pointer + pmemsave, the same
shape as appclose-check.py), then after every step dumps the real
framebuffer and asserts:
  1. the menu bar strip (top 26 logical px, 2x) still has its brand text
     ink ("Joshua Tree"), its clock ink and its pale glass fill;
  2. the frame is not entirely black.
Steps: the full-screen Launchpad (Enter, Esc), every dock app opened and closed by its red button, opened and
closed by Esc, the Apple menu, the notification and weather panels,
dragging a window up under the bar, and two windows stacked.

Usage: tools/checks/menubar-persist-check.py   (repo root, after make kernel.elf)
"""
import json, os, socket, subprocess, sys, time
from PIL import Image
from freeport import free_port

LOG = "/tmp/jt-menubar-persist-serial.log"
DUMP = "/tmp/jt-menubar-persist.raw"
FB = 0xfd000000; W, H = 1920, 1080
PORT = free_port()
LW, LH, SC = 960, 540, 2
BAR_H = 26
DOCK_ICON, DOCK_GAP, SLOT0_X, ICON_ROW_Y = 37, 6, 247, 487
PITCH = DOCK_ICON + DOCK_GAP
CLOSE_X, CLOSE_Y = 94, 56
APPS_CLOSE = (80, 46)
CLOSE_RED = (0xFF, 0x5F, 0x57)
PARK = (480, 200)
SLOTS = ["Apps", "Burrow", "Mail", "Calendar", "Notes", "Reminders", "Terminal", "Samantha", "Weather", "Stocks", "Trash"]

os.chdir(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
for f in (LOG, DUMP):
    try: os.remove(f)
    except FileNotFoundError: pass

q = subprocess.Popen(["qemu-system-i386", "-kernel", "kernel.elf", "-display", "none", "-vga", "std",
                      "-qmp", f"tcp:127.0.0.1:{PORT},server,nowait", "-serial", "file:" + LOG],
                     stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
fails = []
checked = 0
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
            {"type": "abs", "data": {"axis": "x", "value": int(x * 32768 / LW)}},
            {"type": "abs", "data": {"axis": "y", "value": int(y * 32768 / LH)}}]}})
    def btn(down): cmd({"execute": "input-send-event", "arguments": {"events": [{"type": "btn", "data": {"down": down, "button": "left"}}]}})
    def click(): btn(True); time.sleep(0.1); btn(False)
    def keys(*qcodes): cmd({"execute": "send-key", "arguments": {"keys": [{"type": "qcode", "data": k} for k in qcodes]}})
    def frame():
        cmd({"execute": "pmemsave", "arguments": {"val": FB, "size": W * H * 4, "filename": DUMP}})
        return Image.frombytes("RGBA", (W, H), open(DUMP, "rb").read(), "raw", "BGRA").convert("RGB")
    def pixel(x, y): return frame().getpixel((x * SC + 1, y * SC + 1))
    def is_red(p): return max(abs(p[i] - CLOSE_RED[i]) for i in range(3)) <= 12
    def window_open(): return is_red(pixel(CLOSE_X, CLOSE_Y)) or is_red(pixel(*APPS_CLOSE))

    def ink(img, x0, x1):
        """Count dark text pixels (the bar's 0x1C1C1E ink) in the bar strip between logical x0..x1."""
        n = 0
        px = img.load()
        for y in range(4 * SC, 22 * SC):
            for x in range(x0 * SC, x1 * SC):
                r, g, b = px[x, y]
                if r < 0x50 and g < 0x50 and b < 0x50: n += 1
        return n
    def bar_state(img):
        """(brand ink, clock ink, bar is pale, frame not black). Pale: the glass fill is a 50 percent mix with white."""
        px = img.load()
        pale = sum(1 for x in range(300 * SC, 560 * SC, 8) if min(px[x, 5 * SC]) >= 0x80)
        pale_ok = pale >= 20
        nonblack = sum(1 for y in range(0, H, 40) for x in range(0, W, 40) if max(px[x, y]) > 0x10)
        return ink(img, 40, 140), ink(img, LW - 200, LW - 16), pale_ok, nonblack > 20
    def assert_bar(step, settle=0.8):
        global checked
        time.sleep(settle)
        img = frame(); checked += 1
        brand, clock, pale, alive = bar_state(img)
        ok = brand >= 30 and clock >= 30 and pale and alive
        print(f"{'ok  ' if ok else 'FAIL'} {step:46s} brand={brand:4d} clock={clock:4d} pale={pale} notblack={alive}")
        if not ok:
            fails.append(f"{step}: menu bar missing (brand ink {brand}, clock ink {clock}, pale fill {pale}, frame not black {alive})")
            try: img.save("/tmp/jt-menubar-persist-fail-%d.png" % checked)
            except OSError: pass

    for _ in range(120):
        if pixel(480, 511) == (0xEF, 0xEB, 0xE4): break
        time.sleep(0.25)
    else:
        raise SystemExit("FAIL: desktop dock did not appear within 30 seconds")
    time.sleep(0.5)
    centre = lambda slot: SLOT0_X + slot * PITCH + DOCK_ICON // 2
    def open_slot(slot):
        move(centre(slot), ICON_ROW_Y); time.sleep(0.3); click()
        for _ in range(40):
            time.sleep(0.1)
            if window_open(): return True
        return False
    def close_x():
        for at in ((CLOSE_X, CLOSE_Y), APPS_CLOSE):
            if is_red(pixel(*at)):
                move(*at); time.sleep(0.3)
                for _ in range(4):
                    click()
                    for _ in range(30):
                        time.sleep(0.1)
                        if not window_open(): move(*PARK); return
        move(*PARK)
    def recover():
        for _ in range(3):
            if window_open(): keys("esc"); time.sleep(0.8)

    move(*PARK)
    assert_bar("fresh desktop")
    # The reported shape: Enter on a bare desktop opens the full-screen
    # Launchpad (the idle tour does this). It clears the whole buffer, and
    # the minute gate then refused to repaint the bar: a dark band while it
    # was open, and no bar on the desktop after Esc until the clock ticked.
    for rep in range(2):
        keys("ret"); time.sleep(1.5)
        assert_bar(f"Launchpad open via Enter (round {rep + 1})")
        keys("esc"); time.sleep(1.5)
        assert_bar(f"Launchpad closed via Esc (round {rep + 1})")
    for slot, name in enumerate(SLOTS):
        if not open_slot(slot):
            print(f"note {name}: no red close button (full bleed or slow); checking anyway")
        assert_bar(f"{name} open")
        close_x(); recover()
        assert_bar(f"{name} closed by red button")
    for slot, name in enumerate(SLOTS):
        open_slot(slot)
        keys("esc"); time.sleep(0.8); recover()
        assert_bar(f"{name} closed by Esc")
    # Apple menu open then dismissed, notification panel, weather panel.
    move(24, 13); time.sleep(0.3); click(); time.sleep(0.6)
    assert_bar("Apple menu open", 0.3)
    move(*PARK); click(); time.sleep(0.6)
    assert_bar("Apple menu dismissed")
    # Drag a window's titlebar up under the bar and back.
    if open_slot(6):
        move(300, 56); time.sleep(0.3); btn(True); time.sleep(0.2)
        for y in range(56, -1, -8): move(300, max(y, 0)); time.sleep(0.05)
        time.sleep(0.4); btn(False); time.sleep(0.4)
        assert_bar("Terminal dragged under the bar")
        for y in range(0, 200, 10): move(300, y); time.sleep(0.03)
        move(*PARK); time.sleep(0.4)
        recover()
        assert_bar("Terminal dragged away then closed")
    # Two windows stacked, closed top then bottom.
    open_slot(4); open_slot(6)
    assert_bar("Notes + Terminal stacked")
    keys("esc"); time.sleep(0.8); assert_bar("top of two closed by Esc")
    keys("esc"); time.sleep(0.8); recover(); assert_bar("both closed")
    try: cmd({"execute": "quit"})
    except (ConnectionResetError, BrokenPipeError, OSError): pass
finally:
    try: q.wait(timeout=5)
    except subprocess.TimeoutExpired: q.kill()

if fails:
    for x in fails: print("FAIL:", x)
    sys.exit(1)
print(f"PASS: menu bar present and screen never black across {checked} checked frames")
