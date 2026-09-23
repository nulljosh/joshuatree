#!/usr/bin/env python3
"""Headless sharpness check for the antialiased UI text (text_ink curve in
kernel.c, shared by gui_aa_char, wx_text and the Notes editor).

Owner feedback on the DejaVu Sans coverage text: "A-, sharpen them up a
tad". Root cause: raw linear coverage was blended in sRGB, so a 24px stem
(~2.2 physical px, e.g. 'l' = 188,255,108) only had one full-ink column and
the flanking columns read as mid grey. The fix runs coverage through a
stem-darkening + contrast curve for dark-on-light text (and a contrast-only
curve for light-on-dark).

What this measures, on real pixels: boots kernel.elf with -display none and
opens Mail, Notes and Weather from the dock through the real vmmouse path,
one per text path the curve touches (gui_aa_char, editor_draw_glyph and
wx_text), waits until each app's text is on screen, pmemsaves the physical
framebuffer and measures two known text rows per app. For every glyph
pixel (estimated coverage > 8%, from luminance between the surface and the
darkest ink pixel) it computes:
  core  share of glyph pixels at >= 90% ink   (sharpness: dense stems)
  mid   share of glyph pixels at 20..80% ink  (still antialiased, not 1-bit)
Linear blending measured core ~0.50 on both rows; the curve ~0.64.
PASS needs core >= 0.58 on both rows and mid >= 0.12 (a binary/jagged
renderer would have mid ~0) and at least 12 distinct intermediate levels.

Usage: python3 tools/checks/textsharp-check.py   (repo root, after make kernel.elf)
"""
import json, os, socket, subprocess, sys, time
from PIL import Image

PORT = 4494
DUMP = "/tmp/jt-textsharp.raw"
FB = 0xfd000000; W, H = 1920, 1080
LOGICAL_W, LOGICAL_H = 960, 540
DOCK_ICON, DOCK_GAP, SLOT0_X, ICON_ROW_Y = 37, 6, 247, 487
CLOSE = (94, 56)  # window 0's red dot (x+24, y+16) for the x=70, y=40 dock window
# One app per text path the curve touches, each opened from the dock in its
# own window at x=70, y=40 (viewport origin logical (78,72)). Physical boxes:
#   Mail    -> gui_aa_char (every font_draw_string): hint line, first message row
#   Notes   -> editor_draw_glyph: the seeded NOTES.TXT's first two text lines
#   Weather -> wx_text (1:1 faces): "Sample location" and the line under it
APPS = [
    ("Mail", 2, {"Mail hint line": (196, 248, 1000, 280), "Mail message row": (270, 312, 820, 342)}),
    ("Notes", 4, {"Notes title line": (262, 268, 520, 314), "Notes body line": (262, 392, 1000, 434)}),
    ("Weather", 8, {"Weather heading": (222, 178, 490, 218), "Weather caption": (222, 224, 548, 250)}),
]
CORE_MIN, MID_MIN, LEVELS_MIN = 0.58, 0.12, 12

os.chdir(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
try: os.remove(DUMP)
except FileNotFoundError: pass

def has_text(img, box):
    px = list(img.crop(box).tobytes())
    return max(set(px), key=px.count) - min(px) >= 60

shots = {}
q = subprocess.Popen(["qemu-system-i386", "-kernel", "kernel.elf", "-display", "none", "-vga", "std",
                      "-name", "jt-textsharp", "-qmp", f"tcp:127.0.0.1:{PORT},server,nowait", "-serial", "null"],
                     stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
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
        for down in (True, False):
            cmd({"execute": "input-send-event", "arguments": {"events": [{"type": "btn", "data": {"down": down, "button": "left"}}]}})
            time.sleep(0.1)
    def dump():
        cmd({"execute": "pmemsave", "arguments": {"val": FB, "size": W * H * 4, "filename": DUMP}})
        return Image.frombytes("RGBA", (W, H), open(DUMP, "rb").read(), "raw", "BGRA").convert("L")
    for app, slot, rows in APPS:
        move(480, 200); time.sleep(0.3)
        move(SLOT0_X + slot * (DOCK_ICON + DOCK_GAP) + DOCK_ICON // 2, ICON_ROW_Y); time.sleep(0.5)
        click()
        move(930, 300)  # pointer right of the window, clear of every sampled row
        # Wait for the app's own text to be on screen, not a fixed sleep: a
        # capture taken before the window presents measures the desktop.
        img, deadline = None, time.time() + 10
        while time.time() < deadline:
            time.sleep(0.5); img = dump()
            if all(has_text(img, b) for b in rows.values()): break
        time.sleep(0.5); img = dump()
        shots[app] = img
        move(*CLOSE); time.sleep(0.3); click(); time.sleep(1.0)
    try: cmd({"execute": "quit"})
    except (ConnectionResetError, BrokenPipeError, OSError): pass
finally:
    try: q.wait(timeout=5)
    except subprocess.TimeoutExpired: q.kill()

fail = 0
for app, slot, rows in APPS:
    img = shots.get(app)
    if img is None:
        print(f"FAIL: {app}: never captured"); fail = 1; continue
    for name, box in rows.items():
        px = list(img.crop(box).tobytes())
        bg = max(set(px), key=px.count)          # the surface is the most common value
        fg = min(px)                             # darkest ink pixel (cores reach full ink)
        if bg - fg < 60:
            print(f"FAIL: {name}: no text found in {box} (surface {bg}, darkest {fg}); did {app} open?")
            fail = 1; continue
        al = [(bg - p) / (bg - fg) for p in px]
        ink = [a for a in al if a > 0.08]
        core = sum(a >= 0.9 for a in ink) / len(ink)
        mid = sum(0.2 < a < 0.8 for a in ink) / len(ink)
        levels = len({p for p in px if fg + 0.2 * (bg - fg) < p < fg + 0.8 * (bg - fg)})
        print(f"{name}: {len(ink)} glyph px, core {core:.3f} (need >= {CORE_MIN}), mid {mid:.3f} (need >= {MID_MIN}), {levels} intermediate levels (need >= {LEVELS_MIN})")
        if core < CORE_MIN:
            print(f"FAIL: {name}: stems are soft, only {core:.1%} of glyph pixels are full ink (linear-coverage blending is back?)")
            fail = 1
        if mid < MID_MIN or levels < LEVELS_MIN:
            print(f"FAIL: {name}: edges have lost their antialiasing (binary/jagged text)")
            fail = 1
print("PASS: UI text has dense stems and still-antialiased edges (Mail, Notes, Weather)" if not fail else "textsharp-check: FAILED")
sys.exit(fail)
