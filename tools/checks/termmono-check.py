#!/usr/bin/env python3
"""Headless proof that the terminal grid draws with the mono face at its
true advance, so every glyph fits its cell (term-mono fix).

Root cause this guards: the terminal's character grid drew every glyph
through font_draw_char, which (once the v44 AA hook is active) rendered
the PROPORTIONAL DejaVu Sans face left-aligned inside a fixed 8-logical-px
cell. Sans glyphs have very different left-bearings and widths per
character: 'm' is wide and butts into its neighbour, 'i'/'l' are narrow
and leave dead space, so "help" visibly read "hel p" and "mem" read
"nen". The fix points the terminal (and Keyrate) at a real monospace
face (font_draw_char_mono / gui_aa_char_mono in kernel.c) whose glyphs
all share left=0 and the same advance, so left-aligning them in the
fixed cell keeps every column aligned instead.

This types "mmmmiiii" into the terminal's prompt line (never pressing
Enter, so it is never executed) and dumps the real framebuffer:
  - the four 'm's must not be crushed together: each of the four 8-
    logical-px cells they occupy must have ink, and the total inked span
    must be close to four cell-widths (proportional Sans crushes them
    into roughly half that).
  - the four 'i's must not float apart: same per-cell-has-ink check, and
    the span must not blow out past four cells + slack (proportional
    Sans leaves each 'i' with a lot of dead space around it).

Same QMP + pmemsave shape as appclose-check.py; Terminal is dock slot 6.
Usage: tools/checks/termmono-check.py   (from the repo root, after make kernel.elf)
"""
import json, os, socket, subprocess, sys, time
from PIL import Image

LOG = "/tmp/jt-termmono-serial.log"
DUMP = "/tmp/jt-termmono.raw"
FB = 0xfd000000; W, H = 1920, 1080
PORT = 4453
LOGICAL_W, LOGICAL_H, SCALE = 960, 540, 2
DOCK_ICON, DOCK_GAP, SLOT0_X = 37, 6, 247
PITCH = DOCK_ICON + DOCK_GAP
ICON_ROW_Y = 487
CLOSE_X, CLOSE_Y = 94, 56
CLOSE_RED = (0xFF, 0x5F, 0x57)
PARK = (480, 200)
TERM_SLOT = 6

# Terminal window: gui_launch_from_dock x=70,y=40,w=820,h=385; viewport is
# (x+8,y+32,w-16,h-40). term_render's prompt line sits at local
# (32, window_height()-60) = (32, 345-60) = (32, 285) inside that viewport,
# so in screen-logical coords: (78+32, 72+285) = (110, 357). Cell is
# 8 logical / 16 physical px wide, 16 logical / 32 physical tall.
INPUT_X0, INPUT_Y0 = 110, 357
CELL_L = 8          # logical px per cell
CELL_P = CELL_L * SCALE  # 16 physical px per cell
BG = (0x1A, 0x15, 0x12)  # term_draw_chrome's window_clear colour

os.chdir(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
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
    def pixel(img, x, y): return img.getpixel((x, y))  # already physical coords
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

    for ch in "mmmmiiii":
        keys(ch)
        time.sleep(0.1)
    time.sleep(0.3)
    img = dump()

    def cell_has_ink(cell_index):
        """True if any non-background pixel sits inside this cell's own
        physical box, i.e. the glyph did not get fully clipped out of it."""
        x0 = (INPUT_X0 + cell_index * CELL_L) * SCALE
        y0 = INPUT_Y0 * SCALE
        for y in range(y0, y0 + CELL_P * 2):
            for x in range(x0, x0 + CELL_P):
                if not is_bg(pixel(img, x, y)): return True
        return False

    def span(lo_cell, hi_cell):
        """Leftmost/rightmost inked physical x across cells [lo, hi)."""
        x0 = (INPUT_X0 + lo_cell * CELL_L) * SCALE
        x1 = (INPUT_X0 + hi_cell * CELL_L) * SCALE
        y0 = INPUT_Y0 * SCALE
        left = right = None
        for y in range(y0, y0 + CELL_P * 2):
            for x in range(x0, x1):
                if not is_bg(pixel(img, x, y)):
                    if left is None or x < left: left = x
                    if right is None or x > right: right = x
        return left, right

    m_ink = [cell_has_ink(i) for i in range(4)]
    i_ink = [cell_has_ink(4 + i) for i in range(4)]
    print("m cells inked:", m_ink)
    print("i cells inked:", i_ink)
    if not all(m_ink): fails.append(f"'m' missing ink in at least one of its 4 cells: {m_ink}")
    if not all(i_ink): fails.append(f"'i' missing ink in at least one of its 4 cells: {i_ink}")

    m_left, m_right = span(0, 4)
    i_left, i_right = span(4, 8)
    four_cells = 4 * CELL_P
    if m_left is not None and m_right is not None:
        m_span = m_right - m_left
        print(f"'mmmm' inked span: {m_span}px, four cells = {four_cells}px")
        if not (four_cells - CELL_P <= m_span <= four_cells + CELL_P):
            fails.append(f"'mmmm' inked span {m_span}px not within one cell ({CELL_P}px) of four cells ({four_cells}px) -- looks crushed or overspread")
    else:
        fails.append("'mmmm' has no ink at all")
    if i_left is not None and i_right is not None:
        i_span = i_right - i_left
        print(f"'iiii' inked span: {i_span}px, four cells = {four_cells}px")
        if not (four_cells - CELL_P <= i_span <= four_cells + CELL_P):
            fails.append(f"'iiii' inked span {i_span}px not within one cell ({CELL_P}px) of four cells ({four_cells}px) -- looks crushed or overspread")
    else:
        fails.append("'iiii' has no ink at all")

    try: cmd({"execute": "quit"})
    except (ConnectionResetError, BrokenPipeError, OSError): pass
finally:
    try: q.wait(timeout=5)
    except subprocess.TimeoutExpired: q.kill()

if fails:
    for x in fails: print("FAIL:", x)
    sys.exit(1)
print("PASS: terminal grid draws the mono face at its true advance, every glyph in its own cell")
