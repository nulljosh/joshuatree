#!/usr/bin/env python3
"""Headless proof that the Terminal's fixed-width grid now draws through
the same runtime-TTF path Notes uses (kernel/ttf_render.h), not a baked
bitmap, and that the monospace grid still has zero column drift.

Two things, both against a real pmemsave dump of the terminal's typed
input line:
  1. Sharpness: reuses notessharp-check.py's own assert_sharp() (real
     antialiasing, no 2x2 duplicated-pixel blocks) against the typed
     text's crop, instead of re-deriving that measurement.
  2. Grid alignment: types a run of "|" and checks every successive pair
     lands on the exact same integer physical pitch (CELL_P), i.e. no
     drift -- a monospace grid rounding its per-glyph advance instead of
     snapping to the fixed cell would show up here as a growing offset.

Same QMP/pmemsave/dock-click shape as termmono-check.py; reuses its
geometry constants rather than re-deriving them.
"""
import importlib.util
import json, os, socket, subprocess, sys, time
from pathlib import Path
from PIL import Image

ROOT = Path(__file__).resolve().parent.parent.parent
os.chdir(ROOT)

# notessharp-check.py's own filename is not a valid module identifier
# (hyphens), so it's loaded by path; its script body is guarded behind
# `if __name__ == '__main__'` for exactly this reuse.
spec = importlib.util.spec_from_file_location('notessharp_check', ROOT / 'tools' / 'checks' / 'notessharp-check.py')
notessharp_check = importlib.util.module_from_spec(spec)
spec.loader.exec_module(notessharp_check)
assert_sharp = notessharp_check.assert_sharp

LOG = "/tmp/jt-termsharp-serial.log"
DUMP = "/tmp/jt-termsharp.raw"
FB = 0xfd000000; W, H = 1920, 1080
PORT = 4454
LOGICAL_W, LOGICAL_H, SCALE = 960, 540, 2
DOCK_ICON, DOCK_GAP, SLOT0_X = 37, 6, 247
PITCH = DOCK_ICON + DOCK_GAP
ICON_ROW_Y = 487
CLOSE_X, CLOSE_Y = 94, 56
CLOSE_RED = (0xFF, 0x5F, 0x57)
TERM_SLOT = 6

# Same input-line geometry termmono-check.py already worked out.
INPUT_X0, INPUT_Y0 = 110, 357
CELL_L = 8
CELL_P = CELL_L * SCALE
BG = (0x1A, 0x15, 0x12)

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

    def move(x, y):
        cmd({"execute": "input-send-event", "arguments": {"events": [
            {"type": "abs", "data": {"axis": "x", "value": int(x * 32768 / LOGICAL_W)}},
            {"type": "abs", "data": {"axis": "y", "value": int(y * 32768 / LOGICAL_H)}}]}})
    def click():
        cmd({"execute": "input-send-event", "arguments": {"events": [{"type": "btn", "data": {"down": True, "button": "left"}}]}})
        time.sleep(0.1)
        cmd({"execute": "input-send-event", "arguments": {"events": [{"type": "btn", "data": {"down": False, "button": "left"}}]}})
    def keys(*qcodes):
        cmd({"execute": "send-key", "arguments": {"keys": [{"type": "qcode", "data": k} for k in qcodes]}})
    def dump():
        cmd({"execute": "pmemsave", "arguments": {"val": FB, "size": W * H * 4, "filename": DUMP}})
        return Image.frombytes("RGBA", (W, H), open(DUMP, "rb").read(), "raw", "BGRA").convert("RGB")
    def pixel(img, x, y): return img.getpixel((x, y))
    def is_red(img, x, y):
        p = pixel(img, x * SCALE + 1, y * SCALE + 1)
        return max(abs(p[i] - CLOSE_RED[i]) for i in range(3)) <= 12
    def is_bg(p): return max(abs(p[i] - BG[i]) for i in range(3)) <= 10

    for _ in range(120):
        img = dump()
        if pixel(img, 480 * SCALE + 1, 511 * SCALE + 1) == (0xEF, 0xEB, 0xE4):
            break
        time.sleep(0.25)
    else:
        raise SystemExit("FAIL: desktop dock did not appear within 30 seconds")
    time.sleep(0.3)

    centre = lambda slot: SLOT0_X + slot * PITCH + DOCK_ICON // 2
    move(centre(TERM_SLOT), ICON_ROW_Y); time.sleep(0.3); click()
    opened = False
    for _ in range(40):
        time.sleep(0.1)
        img = dump()
        if is_red(img, CLOSE_X, CLOSE_Y): opened = True; break
    if not opened:
        raise SystemExit("FAIL: Terminal did not open from the dock")

    def cell_has_ink(cell_index):
        x0 = (INPUT_X0 + cell_index * CELL_L) * SCALE
        y0 = INPUT_Y0 * SCALE
        for y in range(y0, y0 + CELL_P * 2):
            for x in range(x0, x0 + CELL_P):
                if not is_bg(pixel(img, x, y)): return True
        return False

    LINE = "Ag||||"
    for i, ch in enumerate(LINE):
        if ch == 'A':
            keys('shift', 'a')
        elif ch == '|':
            keys('shift', 'backslash')
        else:
            keys(ch)
        for _ in range(50):
            time.sleep(0.1)
            img = dump()
            if cell_has_ink(i): break
    time.sleep(0.2)
    img = dump()

    # 1. Sharpness: crop the whole typed line at physical resolution and
    # hand it to notessharp-check.py's own measurement.
    x0 = INPUT_X0 * SCALE
    y0 = INPUT_Y0 * SCALE
    x1 = (INPUT_X0 + len(LINE) * CELL_L) * SCALE
    y1 = y0 + CELL_P * 2
    crop = img.crop((x0, y0, x1, y1)).convert('L')
    try:
        assert_sharp(crop, 'terminal "Ag||||"')
    except AssertionError as e:
        fails.append(str(e))

    # 2. Column pitch: the four "|" glyphs (cells 2..5) must each have a
    # single inked column at the same offset within their cell, spaced by
    # exactly CELL_P physical px -- no drift.
    def pipe_x(cell_index):
        """Leftmost inked physical x within one cell's own box, for a
        vertical-stem glyph like '|' this is effectively the stem's x."""
        x0 = (INPUT_X0 + cell_index * CELL_L) * SCALE
        x1 = x0 + CELL_P
        y0 = INPUT_Y0 * SCALE + 4
        y1 = y0 + CELL_P
        left = None
        for x in range(x0, x1):
            for y in range(y0, y1):
                if not is_bg(pixel(img, x, y)):
                    left = x
                    break
            if left is not None:
                break
        return left

    pipe_cells = [2, 3, 4, 5]
    xs = [pipe_x(c) for c in pipe_cells]
    print("pipe stem x positions:", xs)
    if any(x is None for x in xs):
        fails.append(f"one or more '|' glyphs never inked: {xs}")
    else:
        deltas = [xs[i + 1] - xs[i] for i in range(len(xs) - 1)]
        print("pipe pitch deltas:", deltas)
        if len(set(deltas)) != 1:
            fails.append(f"'|' pitch drifts across the grid, deltas {deltas} (expected all equal to the cell width {CELL_P}px)")
        elif deltas[0] != CELL_P:
            fails.append(f"'|' pitch is {deltas[0]}px, not the {CELL_P}px cell width -- grid misaligned")
        else:
            print(f"PASS: '|' pitch is exactly {CELL_P}px across all four glyphs, no drift")

    try: cmd({"execute": "quit"})
    except (ConnectionResetError, BrokenPipeError, OSError): pass
finally:
    try: q.wait(timeout=5)
    except subprocess.TimeoutExpired: q.kill()

if fails:
    for x in fails: print("FAIL:", x)
    sys.exit(1)
print("PASS: terminal renders sharp antialiased TrueType with a driftless monospace grid")
