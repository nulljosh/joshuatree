#!/usr/bin/env python3
"""Headless proof that the terminal grid draws with the mono face at its
true advance, so every glyph fits its cell (term-mono fix).

Root cause this guards: a character grid that drew every glyph through the
PROPORTIONAL Sans face left-aligned inside a fixed 8-logical-px cell. Sans
glyphs have very different left-bearings and widths per character: 'm' is
wide and butts into its neighbour, 'i'/'l' are narrow and leave dead space,
so "help" visibly read "hel p" and "mem" read "nen". user/terminal.c now
draws every cell through libjt's jt_mono_draw (user/libjt/mono.c, DejaVu Sans
Mono, JT_MONO_ADV = 8), whose glyphs all advance the same 8 px, so
left-aligning them in the fixed cell keeps every column aligned instead.

The Terminal is a ring-3 compositor window now, opened by the `open=term`
boot flag (never a dock slot), and its window buffer is logical resolution
(each pixel shows as a 2x2 physical block). This types "mmmmiiii" into the
prompt line (never pressing Enter, so it is never executed) and dumps the
real framebuffer:
  - the four 'm's must not be crushed together: each of the four 8-
    logical-px cells they occupy must have ink, and the total inked span
    must be close to four cell-widths (proportional Sans crushes them
    into roughly half that).
  - the four 'i's must not float apart: same per-cell-has-ink check, and
    the span must not blow out past four cells + slack (proportional
    Sans leaves each 'i' with a lot of dead space around it).

Geometry lives in jtvm.py (TERM_*), next to the helper that types a line.
Usage: tools/checks/termmono-check.py   (from the repo root, after make kernel.elf)
"""
import sys, tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from jtvm import VM, SCALE, TERM_INPUT_X0, TERM_INPUT_Y0, TERM_CELL_L, TERM_BG, term_cell_has_ink, term_type_line

CELL_P = TERM_CELL_L * SCALE   # 16 physical px per cell


def is_bg(p):
    return max(abs(p[i] - TERM_BG[i]) for i in range(3)) <= 10


def main():
    vm = VM(None, 'open=term', 'terminal: ring-3 window', work=tempfile.mkdtemp(prefix='jt-termmono-'))
    fails = []
    try:
        import time
        time.sleep(1.0)   # a key sent the instant the window opens can land before it polls
        img = term_type_line(vm, 'mmmmiiii')

        def span(lo_cell, hi_cell):
            """Leftmost/rightmost inked physical x across cells [lo, hi)."""
            x0 = (TERM_INPUT_X0 + lo_cell * TERM_CELL_L) * SCALE
            x1 = (TERM_INPUT_X0 + hi_cell * TERM_CELL_L) * SCALE
            y0 = TERM_INPUT_Y0 * SCALE
            left = right = None
            for y in range(y0, y0 + CELL_P * 2):
                for x in range(x0, x1):
                    if not is_bg(img.getpixel((x, y))):
                        if left is None or x < left: left = x
                        if right is None or x > right: right = x
            return left, right

        m_ink = [term_cell_has_ink(img, i) for i in range(4)]
        i_ink = [term_cell_has_ink(img, 4 + i) for i in range(4)]
        print('m cells inked:', m_ink)
        print('i cells inked:', i_ink)
        if not all(m_ink): fails.append(f"'m' missing ink in at least one of its 4 cells: {m_ink}")
        if not all(i_ink): fails.append(f"'i' missing ink in at least one of its 4 cells: {i_ink}")

        four_cells = 4 * CELL_P
        for name, (lo, hi) in (("'mmmm'", span(0, 4)), ("'iiii'", span(4, 8))):
            if lo is None or hi is None:
                fails.append(f'{name} has no ink at all')
                continue
            sp = hi - lo
            print(f'{name} inked span: {sp}px, four cells = {four_cells}px')
            if not (four_cells - CELL_P <= sp <= four_cells + CELL_P):
                fails.append(f'{name} inked span {sp}px not within one cell ({CELL_P}px) of four cells ({four_cells}px) -- looks crushed or overspread')
    finally:
        vm.quit()
    if fails:
        for x in fails: print('FAIL:', x)
        sys.exit(1)
    print('PASS: terminal grid draws the mono face at its true advance, every glyph in its own cell')


main()
