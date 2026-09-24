#!/usr/bin/env python3
"""Headless typography regression guard: baseline flatness, letter-gap
variance, and container padding, all measured on real rendered pixels
(same QMP screendump + pmemsave pattern as textsharp-check.py and
appsfolder-layout-check.py), not derived from source constants alone.

Real bug this locks in (found in a Joshua-directed typography quality
pass; real 4x before/after crops sit at /tmp/jt-loop/typography-crops,
this pass's own review output, not committed to the repo; described in
docs/roadmap.md's own entry): the Apps folder's glass panel
(gui_apps_redraw_panel in kernel/kernel.c) was sized as if its grid
started at the panel's own top edge (APPS_VIS_ROWS(3) * cell_h(108) =
324, comfortably under the old 375px panel height), but the grid
actually starts 70px lower (y0=95 vs panel_y=25), to leave room for the
"arrow keys to move" hint line above it. The real bottom needed is
70 + 324 = 394, so the last visible row's labels (at the default scroll
offset: "Bookrank", "Quotes", "Plan", "Lexly", "Toroid") rendered only
~9 logical px above the glass panel's true bottom edge -- title-bar-tight
everywhere else in this UI, here almost touching, confirmed with a real
pmemsave crop (see the before/ folder above). Fixed by sizing the panel
(APPS_PANEL_H, 410) to actually hold the grid it draws.

What this checks, on real pixels, against whichever row of labels ends up
bottommost on screen (the folder's own scroll state on open has drifted
between real captures during this pass, e.g. an extra scroll row on a
slow boot; this does not assume a fixed scroll offset or fixed label
text, only that some real row of app labels is the last one visible):
  1. Container padding: that row's label ink sits with real clearance
     above the glass panel's true bottom edge (found by walking down each
     label column until the smooth glass gradient gives way to the
     wallpaper photo's own high-frequency texture), not just inside it by
     a few soft-fringe pixels.
  2. Baseline flatness: every label in that row shares the same ink
     bottom (font_draw_string's pen is placed from the same `cy` for
     every column; a rounding regression that made one column's baseline
     drift from the others would still "pass" a single-label check).
  3. Letter-gap variance: the ink-column gaps inside the row's widest
     label stay in a tight, roughly uniform band (the font_draw_string
     proportional-pen shape textspacing-check.sh already guards for
     texttest/wraptest, exercised here through a third real call site,
     gui_apps_draw_grid, at the same 24px DejaVu Sans face).

Usage: tools/checks/baseline-check.py   (repo root, after make kernel.elf)
"""
import json, os, socket, subprocess, sys, time
from PIL import Image

PORT = 4495
DUMP = "/tmp/jt-baseline.raw"
LOG = "/tmp/jt-baseline-serial.log"
FB = 0xfd000000; W, H = 1920, 1080
LOGICAL_W, LOGICAL_H = 960, 540
DOCK_ICON, DOCK_GAP, SLOT0_X, ICON_ROW_Y = 37, 6, 247, 487

# Row 2 (the Apps folder's default scroll offset, dock slot 0, window
# opened at gui_launch_from_dock's fixed x=56,y=30 -- deterministic every
# boot) is usually "Bookrank", "Quotes", "Plan", "Lexly", "Toroid",
# GUI_LABELS indices 10-14, but this does not assume that scroll offset
# or that exact text: it only assumes the 3rd row slot (fixed y, whatever
# labels ended up there) has APPS_COLS words in it. Their x positions
# are not hardcoded either: found below by scanning the whole row for ink
# clusters, the same way textspacing-check.sh finds letter runs, so a
# column-width or grid-spacing change does not silently stop testing the
# right pixels.
APPS_COLS = 5
ROW_X0, ROW_X1 = 250, 1750      # physical x band inside the grid, clear of the window's own border/corner
SCAN_Y0, SCAN_Y1 = 870, 915     # physical y band that brackets row 2's label ink (a fixed geometric slot; only which app's label lands there can vary)
BG_LUMA = 244                   # the glass panel's own light fill, measured
INK_DROP = 80                   # a column counts as "ink" when its darkest pixel is this far below BG_LUMA
MIN_CLUSTER_W = 40              # physical px; drops stray window-edge noise, real labels are all >= 47px wide
WORD_GAP = 15                   # physical px; bigger than any letter gap, smaller than any inter-word gap
MIN_CLEARANCE_LOGICAL = 20       # old (buggy) panel: ~1-15; fixed panel: ~41-45; drawn well above the old bug
MAX_BASELINE_SPREAD = 3          # physical px; same cy for every column, so this should be ~0
MAX_GAP_SPREAD = 8               # physical px; textspacing-check.sh's own texttest/wraptest pass at spread 1-4

os.chdir(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
for f in (DUMP, LOG):
    try: os.remove(f)
    except FileNotFoundError: pass

# The RTC is pinned to a daytime hour: the panel-edge walk below tells
# glass from wallpaper by the wallpaper reading green, and the desktop's
# day/night tint (gui_daynight_tint) pulls the night wallpaper far enough
# off green that the edge is never found. This check is about typography
# and padding, not the hour, so it measures the same daytime frame every
# run (it went red on CI at 04:36 UTC after passing at 19:29 UTC on the
# very same code). Same -rtc base= pinning calicon-check.py already uses.
q = subprocess.Popen(["qemu-system-i386", "-rtc", "base=2026-09-23T19:30:00", "-kernel", "kernel.elf", "-display", "none", "-vga", "std",
                      "-name", "jt-baseline", "-qmp", f"tcp:127.0.0.1:{PORT},server,nowait", "-serial", "file:" + LOG],
                     stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
img = None
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
        time.sleep(0.12)
        cmd({"execute": "input-send-event", "arguments": {"events": [{"type": "btn", "data": {"down": False, "button": "left"}}]}})
        time.sleep(0.12)
    def dump():
        cmd({"execute": "pmemsave", "arguments": {"val": FB, "size": W * H * 4, "filename": DUMP}})
        return Image.frombytes("RGBA", (W, H), open(DUMP, "rb").read(), "raw", "BGRA").convert("RGB")

    move(480, 200); time.sleep(0.3)
    move(SLOT0_X + DOCK_ICON // 2, ICON_ROW_Y); time.sleep(0.3)  # Apps is dock slot 0
    click(); time.sleep(1.5)
    img = dump()
    try: cmd({"execute": "quit"})
    except (ConnectionResetError, BrokenPipeError, OSError): pass
finally:
    try: q.wait(timeout=5)
    except subprocess.TimeoutExpired: q.kill()

if img is None:
    print("FAIL: never captured a frame"); sys.exit(1)
px = img.load()
def luma(p): return (p[0] * 77 + p[1] * 150 + p[2] * 29) >> 8
def greenish(p): return (p[1] - p[0]) >= 5  # the satellite wallpaper's own green cast; the glass tint never has it

fail = 0

# Find the row's own word clusters instead of hardcoding column x's: a
# per-column ink scan across the whole row, split into runs wherever the
# gap is bigger than any real letter gap (WORD_GAP), same idea textspacing-
# check.sh uses for single letters, one level up for whole words.
cols_min = [min(luma(px[x, y]) for y in range(SCAN_Y0, SCAN_Y1)) for x in range(ROW_X0, ROW_X1)]
ink_cols = [i for i, v in enumerate(cols_min) if v < BG_LUMA - INK_DROP]
if not ink_cols:
    print(f"FAIL: no ink found in row 2's expected band (x {ROW_X0}-{ROW_X1}, y {SCAN_Y0}-{SCAN_Y1}); did the Apps folder open?")
    sys.exit(1)
runs = []
s = p = ink_cols[0]
for i in ink_cols[1:]:
    if i - p > WORD_GAP: runs.append((s, p)); s = i
    p = i
runs.append((s, p))
words = [(ROW_X0 + a, ROW_X0 + b) for a, b in runs if b - a >= MIN_CLUSTER_W]
print(f"row's word clusters: {words}")
if len(words) != APPS_COLS:
    print(f"FAIL: expected {APPS_COLS} labels (APPS_COLS), found {len(words)} clusters: {words}")
    fail = 1

# The widest cluster gets the letter-gap-variance sample: more letters is
# a stronger spacing sample, and it is picked by shape, not by name, so
# this does not depend on which app landed in this row.
widest_idx = max(range(len(words)), key=lambda i: words[i][1] - words[i][0]) if words else -1

bottoms, gaps_sample = [], None
for idx, (x0, x1) in enumerate(words):
    name = f"col{idx}"
    cx = (x0 + x1) // 2

    # Letter gaps within this word's own tight x-range.
    if idx == widest_idx:
        local_ink = [x for x in range(x0, x1 + 1) if min(luma(px[x, y]) for y in range(SCAN_Y0, SCAN_Y1)) < BG_LUMA - INK_DROP]
        lruns = []
        ls = lp = local_ink[0]
        for i in local_ink[1:]:
            if i - lp > 1: lruns.append((ls, lp)); ls = i
            lp = i
        lruns.append((ls, lp))
        gaps_sample = (name, [lruns[i + 1][0] - lruns[i][1] for i in range(len(lruns) - 1)])

    # This word's baseline: the lowest y where most of its ink columns are
    # still dark (>= 30% of the columns that were ever dark for this
    # word), not just the lowest y with ANY dark pixel. A lone descender
    # (a 'p','j','q','y' tail) is real ink below the baseline, not a
    # baseline-rounding wobble, and would otherwise read as one; this
    # tracks where MOST letters stop, the way a ruled line under the word
    # would sit.
    ink_col_set = set(range(x0, x1 + 1))
    counts = {}
    for y in range(SCAN_Y0, SCAN_Y0 + 90):
        counts[y] = sum(1 for x in ink_col_set if luma(px[x, y]) < BG_LUMA - INK_DROP)
    peak = max(counts.values()) if counts else 0
    baseline_y = SCAN_Y0
    for y in range(SCAN_Y0, SCAN_Y0 + 90):
        if counts.get(y, 0) >= 0.3 * peak: baseline_y = y
    ink_bottom = baseline_y
    bottoms.append((name, ink_bottom))

    # Walk down from the ink until the pixels turn "green": the glass tint
    # (gui_apps_glass) blends toward a warm cream/mauve and never reads
    # green, while the satellite wallpaper underneath it does (measured:
    # glass G-R in -10..-3, wallpaper G-R in +9..+29). The panel's real
    # bottom edge is where that switch happens.
    wallpaper_y = None
    for y in range(ink_bottom + 2, ink_bottom + 160):
        row = [px[x, y] for x in range(cx - 10, cx + 10)]
        if sum(greenish(p) for p in row) >= len(row) * 0.6:
            wallpaper_y = y; break
    if wallpaper_y is None:
        print(f"FAIL: {name}: never found the panel's real bottom edge below its label")
        fail = 1; continue
    clearance_logical = (wallpaper_y - ink_bottom) / 2
    print(f"{name}: ink bottom y={ink_bottom}, panel's real bottom y={wallpaper_y}, clearance {clearance_logical:.1f} logical px (need >= {MIN_CLEARANCE_LOGICAL})")
    if clearance_logical < MIN_CLEARANCE_LOGICAL:
        print(f"FAIL: {name}: its label sits too close to the glass panel's real edge (padding regression, APPS_PANEL_H too small again?)")
        fail = 1

if len(bottoms) >= 2:
    ys = [b for _, b in bottoms]
    spread = max(ys) - min(ys)
    print(f"row baseline spread: {spread}px across {bottoms} (need <= {MAX_BASELINE_SPREAD})")
    if spread > MAX_BASELINE_SPREAD:
        print(f"FAIL: baseline wobbles across the row: {bottoms}")
        fail = 1

if gaps_sample:
    gname, gaps = gaps_sample
    spread = max(gaps) - min(gaps)
    print(f"{gname}'s own letter gaps: {gaps}, spread {spread}px (need <= {MAX_GAP_SPREAD})")
    if spread > MAX_GAP_SPREAD:
        print(f"FAIL: uneven letter spacing in {gname}: {gaps}")
        fail = 1
else:
    print("FAIL: never measured a letter-gap sample")
    fail = 1

print("PASS: Apps folder's bottom visible row has real container padding, a flat baseline, and even letter spacing" if not fail else "baseline-check: FAILED")
sys.exit(fail)
