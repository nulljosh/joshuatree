#!/usr/bin/env python3
"""Headless proof that the dock's Calendar tile is a calendar picture -- a
page with a terracotta binding bar on top and a grid of day squares -- and
that it never draws a date, a dash or any other text over it.

History. v0.89.x drew the live month and day over a blank page
(gui_calendar_face). The Pi has no clock, so it got the "date unknown"
face instead: a red bar and one ink dash on a cream tile, which Joshua's
real-board photo showed as a half-drawn icon (2026-10). The tile is now
authored art only (tools/gen/restyle_icons.py "calendar" ->
art/icons/calendar.svg -> kernel/icon_art.h), the same on i386 and the Pi.

Two boots, two different QEMU -rtc base= dates (Feb 3 and Nov 28), so a
live overlay coming back would show up as the tile changing with the date.
QMP ports 4511/4512, inside this project's reserved 4511-4519 range.

The oracle, on the dock's Calendar tile (slot 3; 960x540 @2x, 74 physical px):
  1. the tile is the same in both boots (at most SAME_MAX pixels differ);
  2. the top band carries the terracotta binding bar (ACCENT_TOP_MIN px);
  3. the lower band carries a grid of grey day squares (GREY_MIN px, and at
     least GRID_COLS separate grey runs across one row of squares);
  4. no dark ink anywhere in the tile (INK_MAX), so no numerals or dashes.
Confirmed discriminating: the previous kernel (blank page + live date)
fails every one of the five asserts below.

Usage: tools/checks/calicon-check.py   (from the repo root, after make kernel.elf)
"""
import json, os, re, shutil, socket, subprocess, sys, time
from PIL import Image

LOG_FMT = "/tmp/jt-calicon-serial-%d.log"
DUMP_FMT = "/tmp/jt-calicon-%d.raw"
FB = 0xfd000000; W, H = 1920, 1080
PORT_A, PORT_B = 4511, 4512

# Same geometry iconlight-check.py / dockhover-check.py use for this boot
# config (960x540 @2x, dock_scale_pct 7, GUI_ICON_COUNT 11).
DOCK_ICON, DOCK_GAP, SLOT0_X, ICON_TOP_Y, SCALE = 37, 6, 247, 469, 2
PITCH = DOCK_ICON + DOCK_GAP
CAL_SLOT = 3  # Apps, Files, Mail, Calendar
TILE_X0 = (SLOT0_X + CAL_SLOT * PITCH) * SCALE
TILE_Y0 = ICON_TOP_Y * SCALE
TILE_W = DOCK_ICON * SCALE  # 74

os.chdir(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))


def boot_and_dump(rtc_base, port):
    log, dump = LOG_FMT % port, DUMP_FMT % port
    for f in (log, dump):
        try: os.remove(f)
        except FileNotFoundError: pass

    syms = {}
    nm = shutil.which("nm") or "nm"
    for line in subprocess.run([nm, "kernel.elf"], capture_output=True, text=True).stdout.splitlines():
        parts = line.split()
        if len(parts) == 3:
            syms[parts[2]] = int(parts[0], 16) - 0xC0000000
    present_addr = syms.get("window_present_count")

    q = subprocess.Popen(["qemu-system-i386", "-name", "jt-calicon", "-kernel", "kernel.elf",
                          "-display", "none", "-vga", "std", "-rtc", "base=" + rtc_base,
                          "-qmp", "tcp:127.0.0.1:%d,server,nowait" % port, "-serial", "file:" + log],
                         stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        time.sleep(1.0)
        s = socket.create_connection(("127.0.0.1", port)); f = s.makefile("rw")

        def cmd(o):
            f.write(json.dumps(o) + "\n"); f.flush()
            while True:
                r = json.loads(f.readline())
                if "return" in r or "error" in r: return r

        def presents():
            if present_addr is None:
                return None
            r = cmd({"execute": "human-monitor-command",
                     "arguments": {"command-line": "xp /4xb 0x%x" % present_addr}})
            out = r.get("return", "")
            vals = [int(v, 16) for line in out.splitlines() if ":" in line
                    for v in re.findall(r"0x([0-9a-f]{2})\b", line.split(":", 1)[1])]
            return int.from_bytes(bytes(vals[:4]), "little") if len(vals) >= 4 else None

        f.readline()
        cmd({"execute": "qmp_capabilities"})
        time.sleep(5.0)
        before = presents()
        if before is not None:
            for _ in range(100):
                if presents() != before:
                    time.sleep(0.05)
                    break
                time.sleep(0.05)
            else:
                print("FAIL: no frame was presented, the screen never updated")
                sys.exit(1)
        cmd({"execute": "pmemsave", "arguments": {"val": FB, "size": W * H * 4, "filename": dump}})
        try: cmd({"execute": "quit"})
        except (ConnectionResetError, BrokenPipeError, OSError): pass
    finally:
        try: q.wait(timeout=5)
        except subprocess.TimeoutExpired: q.kill()

    return Image.frombytes("RGBA", (W, H), open(dump, "rb").read(), "raw", "BGRA").convert("RGB")


SAME_MAX = 4
ACCENT_TOP_MIN = 300
GREY_MIN = 600
GRID_COLS = 4
INK_MAX = 0


def tile(img):
    return {(i, j): img.getpixel((TILE_X0 + i, TILE_Y0 + j)) for j in range(TILE_W) for i in range(TILE_W)}


def accent(c): return c[0] > 140 and c[0] - c[1] > 60 and c[0] - c[2] > 80
def grey(c): return 170 <= min(c) and max(c) <= 220 and max(c) - min(c) <= 12
def ink(c): return max(c) < 0x60


img_feb = boot_and_dump("2026-02-03T12:00:00", PORT_A)
img_nov = boot_and_dump("2026-11-28T12:00:00", PORT_B)
a, b = tile(img_feb), tile(img_nov)

same = sum(1 for k in a if max(abs(x - y) for x, y in zip(a[k], b[k])) > 8)
top = sum(1 for (i, j), c in b.items() if j < TILE_W * 0.36 and accent(c))
low = {k: c for k, c in b.items() if k[1] > TILE_W * 0.38 and k[1] < TILE_W * 0.85}
greys = sum(1 for c in low.values() if grey(c))
row = int(TILE_W * 0.45)  # through the first row of day squares
runs, prev = 0, False
for i in range(TILE_W):
    g = grey(b[(i, row)]) or accent(b[(i, row)])
    if g and not prev: runs += 1
    prev = g
inks = sum(1 for c in list(a.values()) + list(b.values()) if ink(c))

fail = 0
for ok, msg in ((same <= SAME_MAX, "tile pixels that change between Feb 3 and Nov 28: %d (need <= %d)" % (same, SAME_MAX)),
                (top >= ACCENT_TOP_MIN, "terracotta binding-bar pixels in the top band: %d (need >= %d)" % (top, ACCENT_TOP_MIN)),
                (greys >= GREY_MIN, "grey day-square pixels in the lower band: %d (need >= %d)" % (greys, GREY_MIN)),
                (runs >= GRID_COLS, "day squares across row y=%d: %d (need >= %d)" % (row, runs, GRID_COLS)),
                (inks <= INK_MAX, "dark ink pixels (text, dashes) in the tile: %d (need <= %d)" % (inks, INK_MAX))):
    print(("ok:   " if ok else "FAIL: ") + msg)
    fail |= not ok
if fail:
    sys.exit(1)
print("PASS: the Calendar dock tile is a calendar picture (binding bar, day grid), the same on every date, no text")
