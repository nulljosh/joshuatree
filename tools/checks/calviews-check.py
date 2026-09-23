#!/usr/bin/env python3
"""Headless pixel proof of Calendar's Day, Week, Month and Year views
(kernel/calendar.h).

Boots with the clock pinned to 2026-08-15 (a Saturday), opens Calendar from
the dock, and drives it with the real keys: 1/2/3/4 pick a view, right steps
by the view's unit, t returns to today, enter + text + enter saves an event.
From real framebuffer dumps it checks:

  1. The segmented control lights exactly the segment for the view the key
     picked (accent fill inside that pill only).
  2. Day view shows the accent "Today" tag on today, and right (one day
     forward) removes it; t brings it back.
  3. Week view draws its six column separators, and an event saved from
     week view shows up as the accent bar in the selected (Saturday) column.
  4. Year view draws twelve mini months: three rows of mini grids, each
     holding digit rows, and today's accent disc sits in August (column 4,
     row 2 of the 4x3 layout).
  5. Month view is still the default and still fits six weeks (the
     apptop-check.py contract), after visiting the other views.

Before this change the Calendar had one view: keys 1-4 did nothing, so the
segment and every per-view assertion fail.

Usage: tools/checks/calviews-check.py   (from the repo root, after make kernel.elf)
"""
import json, os, socket, subprocess, sys, time
from PIL import Image

LOG = "/tmp/jt-calviews-serial.log"
DUMP = "/tmp/jt-calviews.raw"
FB = 0xfd000000; W, H = 1920, 1080
PORT = 4472
LOGICAL_W, LOGICAL_H, SCALE = 960, 540, 2
DOCK_ICON, DOCK_GAP, SLOT0_X = 37, 6, 247
PITCH = DOCK_ICON + DOCK_GAP
ICON_ROW_Y = 487
PARK = (930, 300)
CLOSE = (94, 56)
VX0, VY0, VX1, VY1 = 78, 72, 890, 417   # window 0's content viewport, logical
T = -32                                  # gui_app_dy() inside a dock window
ACCENT = (0xA0, 0x55, 0x3F)
SEP = (0xEC, 0xE6, 0xDE)

def vx(x): return VX0 + x
def vy(y): return VY0 + y

os.chdir(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
for p in (LOG, DUMP):
    try: os.remove(p)
    except FileNotFoundError: pass

q = subprocess.Popen(["qemu-system-i386", "-kernel", "kernel.elf", "-display", "none", "-vga", "std",
                      "-rtc", "base=2026-08-15T12:00:00", "-name", "jt-calviews-check",
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
    time.sleep(5.0)

    def move(x, y):
        cmd({"execute": "input-send-event", "arguments": {"events": [
            {"type": "abs", "data": {"axis": "x", "value": int(x * 32768 / LOGICAL_W)}},
            {"type": "abs", "data": {"axis": "y", "value": int(y * 32768 / LOGICAL_H)}}]}})
    def click():
        cmd({"execute": "input-send-event", "arguments": {"events": [{"type": "btn", "data": {"down": True, "button": "left"}}]}})
        time.sleep(0.1)
        cmd({"execute": "input-send-event", "arguments": {"events": [{"type": "btn", "data": {"down": False, "button": "left"}}]}})
    def key(qc):
        cmd({"execute": "send-key", "arguments": {"keys": [{"type": "qcode", "data": qc}]}})
        time.sleep(0.35)
    def dump(name):
        time.sleep(0.4)
        cmd({"execute": "pmemsave", "arguments": {"val": FB, "size": W * H * 4, "filename": DUMP}})
        img = Image.frombytes("RGBA", (W, H), open(DUMP, "rb").read(), "raw", "BGRA").convert("RGB")
        img.save(f"/tmp/jt-calviews-{name}.png")
        return img
    def px(img, x, y): return img.getpixel((x * SCALE, y * SCALE))
    def near(a, b, tol=14): return max(abs(a[i] - b[i]) for i in range(3)) <= tol
    def accent_count(img, x0, y0, x1, y1):
        return sum(1 for y in range(y0, y1) for x in range(x0, x1) if near(px(img, x, y), ACCENT))

    def active_segment(img):
        # Each pill spans x 20+64i+2 .. +62 at y T+50..T+68; sample its left interior.
        hits = [i for i in range(4) if near(px(img, vx(20 + 64 * i + 6), vy(T + 59)), ACCENT)]
        return hits

    move(*PARK)
    move(SLOT0_X + 3 * PITCH + DOCK_ICON // 2, ICON_ROW_Y); time.sleep(0.3); click(); time.sleep(1.5)
    move(*PARK); time.sleep(0.4)

    img = dump("month0")
    if active_segment(img) != [2]: fails.append(f"default view: active segment {active_segment(img)}, want [2] (Month)")

    # ---- Day ----
    key("1"); img = dump("day")
    seg = active_segment(img)
    if seg != [0]: fails.append(f"Day: active segment {seg}, want [0]")
    tag = accent_count(img, vx(360), vy(T + 116), vx(444), vy(T + 138))
    print(f"Day: segment {seg}, Today tag accent px {tag}")
    if tag < 200: fails.append(f"Day: no Today tag on today ({tag} accent px)")
    key("right"); img = dump("day-next")
    tag2 = accent_count(img, vx(360), vy(T + 116), vx(444), vy(T + 138))
    if tag2 > 20: fails.append(f"Day: Today tag still shown after stepping a day ({tag2} px)")
    key("t"); img = dump("day-today")
    if accent_count(img, vx(360), vy(T + 116), vx(444), vy(T + 138)) < 200: fails.append("Day: t did not return to today")

    # ---- Week ----
    key("2"); img = dump("week")
    seg = active_segment(img)
    if seg != [1]: fails.append(f"Week: active segment {seg}, want [1]")
    col_w = (804 - 40) // 7; x0 = (804 - col_w * 7) // 2
    seps = sum(1 for c in range(1, 7) if near(px(img, vx(x0 + c * col_w), vy(T + 250)), SEP, 4))
    print(f"Week: segment {seg}, column separators {seps}/6")
    if seps != 6: fails.append(f"Week: {seps} column separators, want 6")
    # Save an event on the selected day (Saturday the 15th, last column).
    key("ret")
    for c in "wk": key(c)
    key("ret"); img = dump("week-event")
    bar = accent_count(img, vx(x0 + 6 * col_w + 6), vy(T + 190), vx(x0 + 6 * col_w + 9), vy(T + 208))
    print(f"Week: event bar accent px in Saturday's column {bar}")
    if bar < 30: fails.append(f"Week: saved event's bar not drawn in the selected column ({bar} px)")

    # ---- Year ----
    key("4"); img = dump("year")
    seg = active_segment(img)
    if seg != [3]: fails.append(f"Year: active segment {seg}, want [3]")
    mw = (804 - 40) // 4; top = T + 116; mh = (345 - 6 - top) // 3
    # August = month 8: column 3, row 1 of the 4x3 layout.
    aug = accent_count(img, vx(20 + 3 * mw), vy(top + mh + 18), vx(20 + 4 * mw), vy(top + 2 * mh))
    other = accent_count(img, vx(20), vy(top + 18), vx(20 + mw), vy(top + mh))  # January
    print(f"Year: segment {seg}, today disc accent px in August {aug}, in January {other}")
    if aug < 30: fails.append(f"Year: today's disc not in August's mini month ({aug} px)")
    if other > 10: fails.append(f"Year: accent ink in January's mini month ({other} px)")
    for row in range(3):
        ink_rows = 0
        for ly in range(top + row * mh + 20, top + (row + 1) * mh):
            if any(sum(px(img, vx(x), vy(ly))) < 450 for x in range(24, 780, 2)): ink_rows += 1
        if ink_rows < 20: fails.append(f"Year: mini-month row {row} has only {ink_rows} ink rows (no digits?)")
    key("right"); img = dump("year-next")
    if accent_count(img, vx(20 + 3 * mw), vy(top + mh + 18), vx(20 + 4 * mw), vy(top + 2 * mh)) > 10:
        fails.append("Year: right did not step to the next year (today's disc still drawn)")
    key("t")

    # ---- Month again ----
    key("3"); img = dump("month")
    seg = active_segment(img)
    if seg != [2]: fails.append(f"Month: active segment {seg}, want [2]")

    move(*CLOSE); time.sleep(0.3); click(); time.sleep(0.8)
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
print("PASS: Calendar Day, Week, Month and Year views")
