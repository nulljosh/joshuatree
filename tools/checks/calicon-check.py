#!/usr/bin/env python3
"""Headless proof that the dock's Calendar tile shows today's date (7.8.0, Joshua: "needs to
show the date"): the month in white on its terracotta band and the day in dark ink on the
page (kernel/clockicon.c gui_calendar_date, the PC's battery clock).

History. v0.89.x drew the live date; then, because the Pi had no clock, the tile became
authored art only and this check forbade any text on it. The Pi now draws the date once
network time is in (arch/arm64/crash.c cal_overlay) and the PC always has its clock, so the
date is back on both.

Two boots, two different QEMU -rtc base= dates (Feb 3 and Nov 28), so a
live overlay coming back would show up as the tile changing with the date.
QMP ports 4511/4512, inside this project's reserved 4511-4519 range.

The oracle, on the dock's Calendar tile (slot 3; 960x540 @2x, 74 physical px):
  1. the two boots differ (DATE_MIN pixels): a different month and day are drawn;
  2. the top band is terracotta (ACCENT_TOP_MIN px) and carries white month letters (MONTH_MIN);
  3. each boot has a dark day number (DAY_INK_MIN px).
Discriminating: take the overlay out of gui_icon_overlay and 1, 2's month and 3 fail.

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
DOCK_ICON, DOCK_GAP, SLOT0_X, ICON_TOP_Y, SCALE = 37, 6, 226, 469, 2
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


DATE_MIN = 40       # 7.8.0: the two dates draw different months and days
MONTH_MIN = 15
DAY_INK_MIN = 30
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
ink_a = sum(1 for c in a.values() if ink(c)); ink_b = sum(1 for c in b.items() if ink(c[1]))
white_top = sum(1 for (i, j), c in b.items() if j < TILE_W * 0.36 and min(c) > 230)   # the month, white on the band

fail = 0
for ok, msg in ((same >= DATE_MIN, "tile pixels that change between Feb 3 and Nov 28: %d (need >= %d, the date is drawn)" % (same, DATE_MIN)),
                (top >= ACCENT_TOP_MIN, "terracotta band pixels in the top band: %d (need >= %d)" % (top, ACCENT_TOP_MIN)),
                (white_top >= MONTH_MIN, "white month pixels on the band: %d (need >= %d)" % (white_top, MONTH_MIN)),
                (min(ink_a, ink_b) >= DAY_INK_MIN, "dark day-number pixels: %d and %d (need >= %d each)" % (ink_a, ink_b, DAY_INK_MIN))):
    print(("ok:   " if ok else "FAIL: ") + msg)
    fail |= not ok
if fail:
    sys.exit(1)
print("PASS: the Calendar dock tile is a calendar picture (binding bar, day grid), the same on every date, no text")
