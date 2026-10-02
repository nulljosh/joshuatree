#!/usr/bin/env python3
"""Headless proof that the Terminal's fixed-width grid draws real antialiased
TrueType (libjt's DejaVu Sans Mono through jt_mono_draw, not a baked bitmap)
and that the monospace grid has zero column drift.

The Terminal is a ring-3 compositor window now (user/terminal.c), opened by the
`open=term` boot flag. Its window buffer is logical resolution and the
compositor shows each buffer pixel as a 2x2 physical block, so the sharpness
half samples one pixel per block to get back what the app itself rasterized.

Two things, both against a real pmemsave dump of the terminal's typed input line:
  1. Sharpness: reuses notessharp-check.py's own assert_sharp() (real
     antialiasing, no 2x2 duplicated-pixel blocks) against the typed
     text's crop at logical resolution, instead of re-deriving that measurement.
  2. Grid alignment: types a run of "|" and checks every successive pair
     lands on the exact same integer physical pitch (CELL_P), i.e. no
     drift -- a monospace grid rounding its per-glyph advance instead of
     snapping to the fixed cell would show up here as a growing offset.

Geometry and the typing helper live in jtvm.py (TERM_*), shared with
termmono-check.py.
"""
import importlib.util
import sys, tempfile, time
from pathlib import Path
from PIL import Image

ROOT = Path(__file__).resolve().parent.parent.parent
sys.path.insert(0, str(Path(__file__).resolve().parent))
from jtvm import VM, SCALE, TERM_INPUT_X0, TERM_INPUT_Y0, TERM_CELL_L, TERM_BG, term_type_line

# notessharp-check.py's own filename is not a valid module identifier
# (hyphens), so it's loaded by path; its script body is guarded behind
# `if __name__ == '__main__'` for exactly this reuse.
spec = importlib.util.spec_from_file_location('notessharp_check', ROOT / 'tools' / 'checks' / 'notessharp-check.py')
notessharp_check = importlib.util.module_from_spec(spec)
spec.loader.exec_module(notessharp_check)
assert_sharp = notessharp_check.assert_sharp

CELL_P = TERM_CELL_L * SCALE
LINE = 'Ag||||'


def is_bg(p):
    return max(abs(p[i] - TERM_BG[i]) for i in range(3)) <= 10


vm = VM(None, 'open=term', 'terminal: ring-3 window', work=tempfile.mkdtemp(prefix='jt-termsharp-'))
fails = []
try:
    time.sleep(1.0)   # a key sent the instant the window opens can land before it polls
    img = term_type_line(vm, LINE)

    # 1. Sharpness: crop the typed line at physical resolution, sample one
    # pixel per 2x2 block back to the app's own logical pixels, and hand that
    # to notessharp-check.py's own measurement.
    x0, y0 = TERM_INPUT_X0 * SCALE, TERM_INPUT_Y0 * SCALE
    x1, y1 = (TERM_INPUT_X0 + len(LINE) * TERM_CELL_L) * SCALE, y0 + 16 * SCALE
    crop = img.crop((x0, y0, x1, y1)).convert('L')
    logical = crop.resize((crop.width // SCALE, crop.height // SCALE), Image.NEAREST)
    try:
        assert_sharp(logical, 'terminal "Ag||||" (logical pixels)')
    except AssertionError as e:
        fails.append(str(e))

    # 2. Column pitch: the four "|" glyphs (cells 2..5) must each have a
    # single inked column at the same offset within their cell, spaced by
    # exactly CELL_P physical px -- no drift.
    def pipe_x(cell_index):
        """Leftmost inked physical x within one cell's own box, for a
        vertical-stem glyph like '|' this is effectively the stem's x."""
        cx0 = (TERM_INPUT_X0 + cell_index * TERM_CELL_L) * SCALE
        for x in range(cx0, cx0 + CELL_P):
            for y in range(y0 + 4, y0 + 4 + CELL_P):
                if not is_bg(img.getpixel((x, y))):
                    return x
        return None

    xs = [pipe_x(c) for c in (2, 3, 4, 5)]
    print('pipe stem x positions:', xs)
    if any(x is None for x in xs):
        fails.append(f"one or more '|' glyphs never inked: {xs}")
    else:
        deltas = [xs[i + 1] - xs[i] for i in range(len(xs) - 1)]
        print('pipe pitch deltas:', deltas)
        if len(set(deltas)) != 1:
            fails.append(f"'|' pitch drifts across the grid, deltas {deltas} (expected all equal to the cell width {CELL_P}px)")
        elif deltas[0] != CELL_P:
            fails.append(f"'|' pitch is {deltas[0]}px, not the {CELL_P}px cell width -- grid misaligned")
        else:
            print(f"PASS: '|' pitch is exactly {CELL_P}px across all four glyphs, no drift")
finally:
    vm.quit()

if fails:
    for x in fails: print('FAIL:', x)
    sys.exit(1)
print('PASS: terminal renders sharp antialiased TrueType with a driftless monospace grid')
