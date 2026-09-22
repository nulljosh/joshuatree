#!/usr/bin/env python3
"""Headless pixel proof that windowed apps start their content right under
the title bar, and that Calendar fits a six-week month inside its window.

Bug (2026-09-21 QA tour, docs/roadmap.md "Bugs"): Mail, Calendar, Notes,
Reminders and Chat (and, found after, Trash, Contacts, Calculator and
Search) drew their first line at y=52, left over from the
full-screen layout where the app drew its own title strip across the top.
Inside a dock window the frame already draws the title bar, so each app
left a blank band about 50px tall under it. Stocks switches its top margin
on gui_app_windowed and looked right. Calendar also placed its grid at a
fixed y=150 with 44px rows, so the fifth week of a month was cut in half by
the bottom of the 385px window and a sixth week was never drawn at all.

This boots with the clock pinned to 2026-08-15 (August 2026 starts on a
Saturday, 31 days, so it needs all six grid rows), opens each app from the
dock, dumps the framebuffer, and checks:

  1. Top gap: the first row of dark ink inside the window's content
     viewport is at most MAX_GAP logical pixels below the viewport's top.
     Before the fix every one of the five apps sits at ~20px + the 32px
     blank band, well over the limit.
  2. Apps-folder apps put their own name in the window frame, and the
     folder's "Apps" comes back when they close.
  3. Calendar: below the weekday rule there are exactly six bands of day
     numbers, and the bottom few rows of the viewport are clear of ink
     (nothing cut off by the window edge).

Usage: tools/checks/apptop-check.py   (from the repo root, after make kernel.elf)
"""
import json, os, socket, subprocess, sys, time
from PIL import Image

LOG = "/tmp/jt-apptop-serial.log"
DUMP = "/tmp/jt-apptop.raw"
FB = 0xfd000000; W, H = 1920, 1080
PORT = 4471
LOGICAL_W, LOGICAL_H, SCALE = 960, 540, 2
DOCK_ICON, DOCK_GAP, SLOT0_X = 37, 6, 247
PITCH = DOCK_ICON + DOCK_GAP
ICON_ROW_Y = 487
PARK = (930, 300)          # right of the window: the pointer sprite must not read as ink
CLOSE = (94, 56)             # window 0's red traffic light (gui_multiwin_geom slot 0 / gui_launch_from_dock)
VX0, VY0, VX1, VY1 = 78, 72, 890, 417  # content viewport: (x+8, y+32, w-16, h-40) for x=70,y=40,w=820,h=385
MAX_GAP = 30
SLOTS = {"Mail": 2, "Calendar": 3, "Notes": 4, "Reminders": 5, "Chat": 7, "Trash": 10}
# Apps-folder apps open inside the folder window (x=56,y=30,w=848,h=490, see
# gui_launch_from_dock), viewport (x+8, y+32, w-16, h-40). Grid index i sits
# at row i/5, col i%5 (APPS_COLS); d moves right, s moves down.
FOLDER_APPS = {"Contacts": 18, "Calculator": 19, "Search": 21}
FOLDER_VIEW = (64, 62, 896, 512)
FOLDER_CLOSE = (80, 46)

os.chdir(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
for p in (LOG, DUMP):
    try: os.remove(p)
    except FileNotFoundError: pass

q = subprocess.Popen(["qemu-system-i386", "-kernel", "kernel.elf", "-display", "none", "-vga", "std",
                      "-rtc", "base=2026-08-15T12:00:00",
                      "-name", "jt-apptop-check",
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
    def click_at(x, y, settle=1.2):
        move(x, y); time.sleep(0.3); click(); time.sleep(settle)
    def dump():
        cmd({"execute": "pmemsave", "arguments": {"val": FB, "size": W * H * 4, "filename": DUMP}})
        return Image.frombytes("RGBA", (W, H), open(DUMP, "rb").read(), "raw", "BGRA").convert("RGB")
    centre = lambda slot: SLOT0_X + slot * PITCH + DOCK_ICON // 2

    def ink_rows(img, view=(VX0, VY0, VX1, VY1)):
        """Logical rows inside the content viewport holding dark (text) ink."""
        x0, y0, x1, y1 = view
        px = img.load(); rows = []
        for ly in range(y0, y1):
            y = ly * SCALE
            if any(sum(px[lx * SCALE, y]) < 520 or sum(px[lx * SCALE + 1, y + 1]) < 520 for lx in range(x0 + 4, x1 - 4)):
                rows.append(ly)
        return rows
    def bands(rows):
        out = []
        for r in rows:
            if out and r - out[-1][1] <= 2: out[-1][1] = r
            else: out.append([r, r])
        return out

    move(*PARK); time.sleep(0.5)
    for name, slot in SLOTS.items():
        click_at(centre(slot), ICON_ROW_Y, 2.0 if name == "Chat" else 1.2)
        move(*PARK); time.sleep(0.6)
        img = dump()
        img.save(f"/tmp/jt-apptop-{name.lower()}.png")
        rows = ink_rows(img)
        if not rows:
            fails.append(f"{name}: no ink in the content viewport at all (did the window open?)")
        else:
            gap = rows[0] - VY0
            print(f"{name}: first ink {gap}px below the title bar")
            if gap > MAX_GAP: fails.append(f"{name}: blank strip under the title bar, first ink {gap}px down (max {MAX_GAP})")
        if name == "Calendar" and rows:
            # The weekday rule: a long run of the rule colour across the grid.
            px = img.load(); rule = None
            for ly in range(VY0, VY1):
                run = sum(1 for lx in range(VX0, VX1) if px[lx * SCALE, ly * SCALE] == (0xDD, 0xD9, 0xD3))
                if run > 300: rule = ly; break
            if rule is None:
                fails.append("Calendar: weekday rule not found")
            else:
                weeks = [b for b in bands([r for r in rows if r > rule])]
                print(f"Calendar: {len(weeks)} week rows under the rule (want 6), bands {weeks}")
                if len(weeks) != 6: fails.append(f"Calendar: August 2026 shows {len(weeks)} week rows, want 6")
            if rows[-1] >= VY1 - 3: fails.append(f"Calendar: ink touches the bottom of the window (y={rows[-1]}), last week is clipped")
        click_at(*CLOSE, 1.0)
        move(*PARK); time.sleep(0.5)
    def title(img):
        # The folder window's frame title strip, x+90..x+410, y+4..y+26 (gui_app_frame_title).
        return img.crop(((56 + 90) * SCALE, (30 + 4) * SCALE, (56 + 410) * SCALE, (30 + 26) * SCALE)).tobytes()
    def key(qc):
        cmd({"execute": "send-key", "arguments": {"keys": [{"type": "qcode", "data": qc}]}}); time.sleep(0.35)  # search-check.py: faster drops scancodes
    for name, idx in FOLDER_APPS.items():
        click_at(SLOT0_X + DOCK_ICON // 2, ICON_ROW_Y, 1.2)
        move(*PARK); time.sleep(0.3)
        title_apps = title(dump())
        for _ in range(idx % 5): key("d")
        for _ in range(idx // 5): key("s")
        key("ret"); time.sleep(1.2)
        img = dump()
        img.save(f"/tmp/jt-apptop-{name.lower()}.png")
        if title(img) == title_apps: fails.append(f"{name}: window frame still says Apps, not the app's own name")
        fx0, fy0, fx1, fy1 = FOLDER_VIEW
        samples = [img.getpixel((x * SCALE, y * SCALE)) for y in range(fy0, fy1, 9) for x in range(fx0, fx1, 9)]
        bg = sum(1 for p in samples if p == (0xFA, 0xF8, 0xF6)) * 100 // len(samples)
        rows = ink_rows(img, FOLDER_VIEW)
        if bg < 80:
            fails.append(f"{name}: did not open from the Apps folder (viewport only {bg}% app background)")
        elif not rows:
            fails.append(f"{name}: no ink in the Apps-folder viewport (did it open?)")
        else:
            gap = rows[0] - FOLDER_VIEW[1]
            print(f"{name}: first ink {gap}px below the title bar")
            if gap > MAX_GAP: fails.append(f"{name}: blank strip under the title bar, first ink {gap}px down (max {MAX_GAP})")
        key("esc"); time.sleep(0.6)
        if title(dump()) != title_apps: fails.append(f"{name}: frame title not restored to Apps after closing it")
        click_at(*FOLDER_CLOSE, 1.0)
        move(*PARK); time.sleep(0.4)
    try: cmd({"execute": "quit"})
    except (ConnectionResetError, BrokenPipeError, json.JSONDecodeError): pass
finally:
    try: q.wait(timeout=5)
    except subprocess.TimeoutExpired: q.kill()

serial = open(LOG, errors="replace").read() if os.path.exists(LOG) else ""
for bad in ("PANIC", "panic", "Page fault", "EXCEPTION"):
    if bad in serial: fails.append(f"serial log contains {bad!r}")

if fails:
    print("FAIL:"); [print("  " + x) for x in fails]; sys.exit(1)
print("PASS: windowed apps start under the title bar, Calendar fits six weeks")
