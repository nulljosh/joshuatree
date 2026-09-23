#!/usr/bin/env python3
"""Headless sharpness check for the antialiased UI text (text_ink curve in
kernel.c, shared by gui_aa_char, wx_text and the Notes editor).

Owner feedback on the DejaVu Sans coverage text: "A-, sharpen them up a
tad". Root cause: raw linear coverage was blended in sRGB, so a 24px stem
(~2.2 physical px, e.g. 'l' = 188,255,108) only had one full-ink column and
the flanking columns read as mid grey. The fix runs coverage through a
stem-darkening + contrast curve for dark-on-light text (and a contrast-only
curve for light-on-dark).

What this measures, on real pixels: boots kernel.elf with -display none,
clicks the Mail dock icon through the real vmmouse path, pmemsaves the
physical framebuffer, and looks at two known text rows in Mail's window:
the grey hint line ("up/down to pick ...", 0x807468 on the window surface)
and the first message row (dark text on the list row). For every glyph
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
MAIL_SLOT = 2
# Physical boxes. Mail's viewport origin is logical (78,72); the hint line
# is font_draw_string(..., 20, 52, ...) in mail.h, the first message row
# sits at logical y ~155..171.
ROWS = {
    "hint line": (196, 248, 1000, 280),
    "message row": (270, 312, 820, 342),
}
CORE_MIN, MID_MIN, LEVELS_MIN = 0.58, 0.12, 12

os.chdir(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
try: os.remove(DUMP)
except FileNotFoundError: pass

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
    move(480, 200); time.sleep(0.3)
    move(SLOT0_X + MAIL_SLOT * (DOCK_ICON + DOCK_GAP) + DOCK_ICON // 2, ICON_ROW_Y); time.sleep(0.5)
    click(); time.sleep(1.5)
    move(480, 330); time.sleep(0.5)  # pointer well clear of both text rows
    cmd({"execute": "pmemsave", "arguments": {"val": FB, "size": W * H * 4, "filename": DUMP}})
    try: cmd({"execute": "quit"})
    except (ConnectionResetError, BrokenPipeError, OSError): pass
finally:
    try: q.wait(timeout=5)
    except subprocess.TimeoutExpired: q.kill()

img = Image.frombytes("RGBA", (W, H), open(DUMP, "rb").read(), "raw", "BGRA").convert("L")
fail = 0
for name, box in ROWS.items():
    px = list(img.crop(box).tobytes())
    bg = max(set(px), key=px.count)          # the surface is the most common value
    fg = min(px)                             # darkest ink pixel (cores reach full ink)
    if bg - fg < 60:
        print(f"FAIL: {name}: no text found in {box} (surface {bg}, darkest {fg}); did Mail open?")
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
print("PASS: UI text has dense stems and still-antialiased edges" if not fail else "textsharp-check: FAILED")
sys.exit(fail)
