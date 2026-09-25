#!/usr/bin/env python3
"""Headless proof that the dock's Calendar tile shows the real current
date, macOS style, instead of the old fixed baked-in "SEP 17" art.

v0.89.x replaced that baked art (art/icons/calendar.svg's month/day strokes,
authored by tools/gen/restyle_icons.py, rasterized by tools/gen/
gen_icon_art.py into kernel/icon_art.h) with a plain white tile, and draws
the real month/day at runtime on top of it (gui_calendar_draw_date in
kernel/kernel.c), off the exact same cmos_read_time_stable() read the menu
bar clock already trusts, so the two can never disagree.

Same boot + pmemsave shape as iconlight-check.py: -display none, -vga std,
wait for a real window_present_count change, dump the real 1920x1080
framebuffer. Two boots, two different QEMU -rtc base= dates (real wall-clock
CMOS seed, not a kernel command-line flag, so this exercises the exact RTC
path the kernel reads on real hardware). QMP ports 4511/4512, inside this
project's reserved 4511-4519 range.

The oracle, on the dock's Calendar tile (slot 3, the same geometry
iconlight-check.py/dockhover-check.py already use for this boot config:
960x540 logical @2x, dock_scale_pct 7, GUI_ICON_COUNT 11, DOCK_ICON 37,
SLOT0_X 247, PITCH 43, tile top y 469):

  1. the day-number band (lower half of the tile) differs substantially
     between the two boots -- pixel-count of "dark ink" (near-black, the
     day numeral's own colour) in that band must differ by at least
     DAY_DIFF_MIN pixels between Feb 3 and Nov 28 (very different glyph
     shapes: "3" vs "28", one digit vs two). The old fixed art shows "17"
     in both boots, so this is near zero on that build.
  2. the month band (upper part of the tile) also differs substantially
     the same way -- "FEB" vs "NOV" -- by at least MONTH_DIFF_MIN
     mismatched red-ink pixels between the two boots.
  3. the month band contains real red ink in both boots (mean of the red
     channel minus the mean of green/blsue over the reddest pixels there
     clears RED_MIN), proving the month text is actually the red caps
     macOS uses, not some other color or nothing at all.

Confirmed discriminating: run with --check-old first (see below) against
the pre-existing baked "SEP 17" art (git stash the working tree's
art/icons/calendar.svg + kernel/icon_art.h + kernel/kernel.c changes,
rebuild, run) -- both boots render the same fixed glyph, so 1 and 2 both
read ~0 and the check fails; after restoring this change and rebuilding,
both pass.

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

# gui_calendar_draw_date's own layout (kernel/kernel.c): month cap top at
# y + size*17/100, day cap top at y + size*41/100, tile height = size.
# Physical bands, generous margins either side of those anchors so a small
# layout tweak doesn't make this check flaky.
MONTH_ROWS = range(int(TILE_W * 0.10), int(TILE_W * 0.42))
DAY_ROWS = range(int(TILE_W * 0.44), int(TILE_W * 0.98))
COLS = range(2, TILE_W - 2)

DAY_DIFF_MIN = 80      # dark-ink pixel count must differ by at least this many
MONTH_DIFF_MIN = 60    # red-ink pixel count must differ by at least this many
RED_MIN = 60           # red channel must lead green/blue by this much, averaged over the reddest pixels

# v0.89.x follow-up: the day numeral used to run edge to edge on the dock's
# 74-physical-px tile (mul_d=2, sized against the bigger Apps-folder grid
# tile and never checked against the dock's own smaller one -- a real crop
# showed "25" with almost no side margin and its stroke crossing into the
# tile's own bottom curve). MARGIN_COLS keeps ink out of the same inset the
# rest of the dock's glyphs respect (restyle_icons.py's shared top-16/
# bottom-20-of-128 band is ~12.5%/15.6%; 8% here on a 74px tile is a real
# margin with slack for AA fringing, not a tight pin). BOTTOM_NO_INK_ROWS
# catches the day numeral's descender crossing the tile's own bottom edge.
MARGIN_COLS = max(3, int(TILE_W * 0.08))
BOTTOM_NO_INK_ROWS = range(int(TILE_W * 0.94), TILE_W)

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


def lum(p):
    return (p[0] * 299 + p[1] * 587 + p[2] * 114) / 1000.0


def dark_mask(img):
    """1 where a pixel in the tile is dark ink (the day numeral), 0 elsewhere."""
    out = {}
    for y in DAY_ROWS:
        for x in COLS:
            p = img.getpixel((TILE_X0 + x, TILE_Y0 + y))
            out[(x, y)] = 1 if lum(p) < 110 else 0
    return out


def red_mask(img):
    """1 where a pixel in the tile is red ink (the month label), 0 elsewhere."""
    out = {}
    for y in MONTH_ROWS:
        for x in COLS:
            p = img.getpixel((TILE_X0 + x, TILE_Y0 + y))
            out[(x, y)] = 1 if (p[0] - max(p[1], p[2])) > 60 and p[0] > 140 else 0
    return out


def edge_ink_count(img):
    """Count of dark or red ink pixels in the tile's own side margins or
    right against its bottom edge -- the exact shape of the overflow bug
    (text running edge to edge with no inset, or a descender crossing the
    tile's own bottom curve)."""
    count = 0
    for y in list(DAY_ROWS) + list(MONTH_ROWS):
        for x in list(range(0, MARGIN_COLS)) + list(range(TILE_W - MARGIN_COLS, TILE_W)):
            p = img.getpixel((TILE_X0 + x, TILE_Y0 + y))
            is_dark = lum(p) < 110
            is_red = (p[0] - max(p[1], p[2])) > 60 and p[0] > 140
            if is_dark or is_red:
                count += 1
    for y in BOTTOM_NO_INK_ROWS:
        for x in COLS:
            p = img.getpixel((TILE_X0 + x, TILE_Y0 + y))
            if lum(p) < 110:
                count += 1
    return count


def red_strength(img):
    reddest = []
    for y in MONTH_ROWS:
        for x in COLS:
            p = img.getpixel((TILE_X0 + x, TILE_Y0 + y))
            reddest.append((p[0] - (p[1] + p[2]) / 2.0, p))
    reddest.sort(key=lambda t: -t[0])
    top = reddest[:40] if len(reddest) >= 40 else reddest
    if not top:
        return 0
    return sum(t[0] for t in top) / len(top)


img_feb = boot_and_dump("2026-02-03T12:00:00", PORT_A)  # "FEB 3"
img_nov = boot_and_dump("2026-11-28T12:00:00", PORT_B)  # "NOV 28"

day_feb, day_nov = dark_mask(img_feb), dark_mask(img_nov)
month_feb, month_nov = red_mask(img_feb), red_mask(img_nov)

day_diff = sum(1 for k in day_feb if day_feb[k] != day_nov[k])
month_diff = sum(1 for k in month_feb if month_feb[k] != month_nov[k])
red_feb, red_nov = red_strength(img_feb), red_strength(img_nov)

print("day-number band pixel diff:  %d (need >= %d)" % (day_diff, DAY_DIFF_MIN))
print("month band pixel diff:       %d (need >= %d)" % (month_diff, MONTH_DIFF_MIN))
print("month band red strength:     Feb boot %.1f, Nov boot %.1f (need >= %d each)" % (red_feb, red_nov, RED_MIN))

fail = 0
if day_diff < DAY_DIFF_MIN:
    print("FAIL: the day-number area barely changed between Feb 3 and Nov 28 -- looks like fixed/baked art, not a live date")
    fail = 1
if month_diff < MONTH_DIFF_MIN:
    print("FAIL: the month area barely changed between Feb 3 and Nov 28 -- looks like fixed/baked art, not a live date")
    fail = 1
if red_feb < RED_MIN or red_nov < RED_MIN:
    print("FAIL: the month area is not red ink in at least one boot")
    fail = 1

edge_nov = edge_ink_count(img_nov)  # two-digit day ("28"), the wider/worst case for side overflow
print("edge-margin ink pixels (NOV 28 boot): %d (need 0, margin %dpx each side + bottom %d%% of tile)"
      % (edge_nov, MARGIN_COLS, int((1 - BOTTOM_NO_INK_ROWS.start / TILE_W) * 100)))
if edge_nov > 0:
    print("FAIL: the date text runs into the tile's own side margin or bottom edge -- "
          "match the inset the rest of the dock's glyphs keep off the squircle")
    fail = 1

if fail:
    sys.exit(1)
print("PASS: the Calendar dock tile shows a real, live date (month in red ink, day number below), not fixed art")
