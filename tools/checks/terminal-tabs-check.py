#!/usr/bin/env python3
"""Headless proof that the Terminal is a multiplexer (2.11.0): tabs with independent scrollback, a tab rail,
an activity dot on a tab that printed while you were elsewhere, closing a tab, and split panes with their own
shells. Everything is read from real framebuffer pixels (pmemsave) and the app's own serial markers.

The Terminal (user/terminal.c) opens with `open=term`. Geometry, logical px (each shows as a 2x2 block):
the window content starts at (78,72); the tab rail is the first 148 columns, a tab row is 30 tall from y+14,
its activity dot sits 20 px in and the row's vertical middle; the pane area starts at rail+1 and each cell
is 8 wide. The accent #b5502c is the dot colour.

What it proves, in order, each stage named in its FAIL message:
  1. tabs: Ctrl+T makes a second tab; the two tabs print different scrollback (their third text row inks 5
     cells for "alpha" and 4 for "beta"), switching with Ctrl+1 / Ctrl+2 shows each tab's own content again,
     pixel for pixel.
  2. activity dot: `sleep` starts a timer in tab 2, we switch to tab 1, no dot shows while it runs; when it
     ends (serial `job done`) the dot shows on tab 2's row and not on the active tab; going back to tab 2
     clears it.
  3. close: Ctrl+W removes tab 2, tab 1's content is back and the rail has one row.
  4. splits: Ctrl+D makes two panes each with its own shell (different scrollback, a typed line in one does
     not touch the other), Ctrl+O moves focus, Ctrl+W closes just that pane.
Waits are conditions with generous deadlines (serial markers and pixels), never fixed sleeps.
Usage: tools/checks/terminal-tabs-check.py   (from the repo root, after make kernel.elf)
"""
import sys, tempfile, time, hashlib
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from jtvm import VM, SCALE

WX, WY = 78, 72              # window content origin, logical
RAIL = 148
PANE_X = WX + RAIL + 1 + 16  # first text column of a single pane
ACCENT = (0xB5, 0x50, 0x2C)
CELL = 8
DEADLINE = 120               # seconds for any one condition on a slow runner
SLEEP_S = 25                 # the background timer; the pre-dot assertions must land inside it


def near(p, c, tol=36):
    return all(abs(p[i] - c[i]) <= tol for i in range(3))


def px(img, x, y):
    return img.getpixel((int(x * SCALE), int(y * SCALE)))


def dot_pixels(img, row):
    """Accent pixels in the 12x12 logical box where tab `row` (0-based) draws its activity dot."""
    cx, cy = WX + 20, WY + 14 + row * 30 + 13
    n = 0
    for y in range((cy - 6) * SCALE, (cy + 6) * SCALE):
        for x in range((cx - 6) * SCALE, (cx + 6) * SCALE):
            if near(img.getpixel((x, y)), ACCENT):
                n += 1
    return n


def row_selected(img, row):
    """The active tab's row is a filled lighter block; an idle row is the rail colour."""
    p = px(img, WX + 100, WY + 14 + row * 30 + 2)
    return p[0] > 0x22


def rail_rows(img):
    """How many tab rows are drawn: count selected-or-labelled rows by label ink in the rail."""
    n = 0
    for row in range(9):
        y0 = WY + 14 + row * 30
        ink = 0
        for y in range(y0 * SCALE, (y0 + 26) * SCALE, 2):
            for x in range((WX + 30) * SCALE, (WX + 120) * SCALE, 2):
                p = img.getpixel((x, y))
                if p[0] > 0x98:
                    ink += 1
        if ink > 8:
            n += 1
    return n


def region_sig(img, x0, y0, x1, y1):
    return hashlib.sha1(img.crop((x0 * SCALE, y0 * SCALE, x1 * SCALE, y1 * SCALE)).tobytes()).hexdigest()


def row_cells(img, x0, row, cells=24, ox=0):
    """Number of 8-px cells holding ink on text row `row` of a pane whose first text column is x0."""
    y0 = WY + 16 + row * 17
    n = 0
    for c in range(cells):
        hit = False
        for y in range(y0 * SCALE, (y0 + 16) * SCALE):
            for x in range((x0 + c * CELL) * SCALE, (x0 + c * CELL + CELL) * SCALE):
                p = img.getpixel((x, y))
                if p[0] > 0x50:
                    hit = True
                    break
            if hit:
                break
        n += hit
    return n


class Fail(Exception):
    pass


def until(fn, what, secs=DEADLINE):
    end = time.time() + secs
    while time.time() < end:
        v = fn()
        if v:
            return v
        time.sleep(0.15)
    raise Fail(what)


def main():
    vm = VM(None, 'open=term', 'terminal: ring-3 window', work=tempfile.mkdtemp(prefix='jt-termtabs-'))
    try:
        run(vm)
    except Fail as e:
        print('FAIL: ' + str(e))
        return 1
    finally:
        vm.quit()
    print('PASS: Terminal tabs: independent scrollback, rail, switching, activity dot on a background tab, '
          'closing, and split panes with their own shells')
    return 0


def run(vm):
    ran = [0]

    def chord(key, marker):
        """Send a chord and wait until the app reports the state change it causes."""
        before = vm.count(marker)
        vm.key(key, gap=0.2)
        until(lambda: vm.count(marker) > before, f'no "{marker}" after {key}')

    def typeline(text, x0=PANE_X, y=WY + 293, ink_rows=None):
        """Type into the focused pane, wait until every non-space glyph has inked on the prompt row, press Enter,
        wait for the kernel to report the command ran."""
        for ch in text:
            vm.key('spc' if ch == ' ' else ch, gap=0.05)
        want = len(text.replace(' ', ''))
        # prompt row: "~> " is three cells, the text follows.
        def inked():
            img = vm.frame()
            n = 0
            for c in range(3, 3 + len(text)):
                hit = False
                for yy in range(y * SCALE, (y + 16) * SCALE):
                    for xx in range((x0 + c * CELL) * SCALE, (x0 + c * CELL + CELL) * SCALE):
                        if img.getpixel((xx, yy))[0] > 0x50:
                            hit = True
                            break
                    if hit:
                        break
                n += hit
            return n == want
        until(inked, f'typed "{text}" never reached the prompt line')
        k = text.split()[0]
        before = vm.count('terminal: ran=')
        vm.key('ret', gap=0.2)
        if k != 'sleep':
            until(lambda: vm.count('terminal: ran=') > before, f'"{text}" never ran')
        else:
            time.sleep(0.5)

    until(lambda: vm.count('terminal: tabs a=1 b=1') >= 1, 'the Terminal never reported its first tab')
    time.sleep(1.0)   # first key vs the window's first poll (the app buffers, but be kind)

    # ---- 1. tabs ----
    typeline('echo alpha')
    img = vm.frame()
    if not row_selected(img, 0):
        raise Fail('tabs: the first tab is not drawn as the selected row in the rail')
    t1_cells = row_cells(img, PANE_X, 2)
    sig1 = region_sig(img, WX + RAIL + 2, WY + 8, WX + 805, WY + 120)
    chord('ctrl-t', 'terminal: tabs a=2 b=2')
    typeline('echo beta')
    img = vm.frame()
    t2_cells = row_cells(img, PANE_X, 2)
    sig2 = region_sig(img, WX + RAIL + 2, WY + 8, WX + 805, WY + 120)
    if rail_rows(img) != 2 or not row_selected(img, 1) or row_selected(img, 0):
        raise Fail(f'tabs: rail should show two rows with the second selected (rows={rail_rows(img)})')
    if (t1_cells, t2_cells) != (5, 4):
        raise Fail(f'tabs: scrollback differs per tab ("alpha" 5 cells, "beta" 4), got {t1_cells} and {t2_cells}')
    if sig1 == sig2:
        raise Fail('tabs: two tabs show identical scrollback pixels')
    chord('ctrl-1', 'terminal: tabs a=2 b=1')
    img = vm.frame()
    if region_sig(img, WX + RAIL + 2, WY + 8, WX + 805, WY + 120) != sig1 or not row_selected(img, 0):
        raise Fail('tab switching: Ctrl+1 does not bring back tab 1 exactly as it was')
    chord('ctrl-2', 'terminal: tabs a=2 b=2')
    img = vm.frame()
    if region_sig(img, WX + RAIL + 2, WY + 8, WX + 805, WY + 120) != sig2 or not row_selected(img, 1):
        raise Fail('tab switching: Ctrl+2 does not bring back tab 2 exactly as it was')

    # ---- 2. activity dot ----
    typeline(f'sleep {SLEEP_S} build done')
    chord('ctrl-1', 'terminal: tabs a=2 b=1')
    img = vm.frame()
    if vm.count('terminal: job done') != 0:
        raise Fail('activity dot: the runner was too slow, the timer ended before the dot-absent probe')
    if dot_pixels(img, 1) != 0:
        raise Fail('activity dot: a dot is showing on tab 2 before it printed anything')
    until(lambda: vm.count('terminal: job done tab a=2') >= 1, 'activity dot: the background timer never finished')
    img = until(lambda: (lambda f: f if dot_pixels(f, 1) >= 60 else None)(vm.frame()),
                'activity dot: no accent dot on the background tab after it printed')
    if dot_pixels(img, 0) != 0:
        raise Fail('activity dot: the active tab shows a dot')
    chord('ctrl-2', 'terminal: tabs a=2 b=2')
    until(lambda: dot_pixels(vm.frame(), 1) == 0, 'activity dot: the dot did not clear on focus')

    # ---- 3. close ----
    chord('ctrl-w', 'terminal: tabs a=1 b=1')
    img = vm.frame()
    if rail_rows(img) != 1:
        raise Fail(f'close: the rail still shows {rail_rows(img)} rows after Ctrl+W')
    if region_sig(img, WX + RAIL + 2, WY + 8, WX + 805, WY + 120) != sig1:
        raise Fail('close: tab 1 is not back to its own scrollback after closing tab 2')

    # ---- 4. splits ----
    chord('ctrl-d', 'terminal: panes a=2 b=11')
    half = (804 - RAIL - 1) // 2
    rx = WX + RAIL + 1 + half + 16
    typeline('echo hi', x0=rx)
    img = vm.frame()
    left, right = row_cells(img, PANE_X, 2, 12), row_cells(img, rx, 2, 12)
    if (left, right) != (5, 2):
        raise Fail(f'splits: each pane keeps its own scrollback ("alpha" 5 cells left, "hi" 2 right), got {left} and {right}')
    sep = px(img, WX + RAIL + 1 + half - 1, WY + 150)
    if not near(sep, (0x2A, 0x22, 0x1D), 3):
        raise Fail('splits: no divider between the two panes')
    chord('ctrl-o', 'terminal: panes a=2 b=10')
    typeline('echo zz')
    img = vm.frame()
    if row_cells(img, rx, 2, 12) != 2 or row_cells(img, rx, 3, 12) != 0:
        raise Fail('splits: a command run in the left pane leaked into the right pane')
    if row_cells(img, PANE_X, 4, 12) != 2:
        raise Fail('splits: the left pane did not show its own "zz" output')
    chord('ctrl-w', 'terminal: panes a=1 b=0')
    img = vm.frame()
    if row_cells(img, PANE_X, 2, 12) != 2 or row_cells(img, PANE_X, 4, 12) != 0:
        raise Fail('splits: Ctrl+W closes only the focused pane; the right shell ("hi") should now fill the tab')

    chord('ctrl-e', 'terminal: panes a=2 b=21')   # a second split, stacked: layout 2, focus on the new pane
    chord('ctrl-w', 'terminal: panes a=1 b=0')


sys.exit(main())
