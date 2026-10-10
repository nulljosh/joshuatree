#!/usr/bin/env python3
"""Headless sharpness check for the antialiased UI text of the ring-3 apps
(libjt's runtime-TTF faces, drawn into each compositor window's own buffer).

Owner feedback on the DejaVu Sans coverage text: "A-, sharpen them up a
tad". Root cause: raw linear coverage was blended in sRGB, so a 24px stem
(~2.2 physical px, e.g. 'l' = 188,255,108) only had one full-ink column and
the flanking columns read as mid grey. The fix runs coverage through a
stem-darkening + contrast curve for dark-on-light text.

What this measures, on real pixels: boots kernel.elf with -display none and
opens Mail and Notes through their `open=` boot flags (never a dock slot), in
three views, one per distinct text surface the 2.0 apps draw: Mail's hint line
and message row, Notes' browse view (FOLDERS heading and note row) and Notes'
editor body line (a fresh note with a sentence typed into it). It waits until
each view's text is on screen, pmemsaves the physical framebuffer, and
measures two known text rows per view.

A ring-3 window's buffer is logical resolution and the compositor shows each
buffer pixel as a 2x2 physical block, so every crop is sampled back to one
pixel per block: the numbers describe what the app itself rasterized, not
the compositor's stretch. For every glyph pixel (estimated coverage > 8%,
from luminance between the surface and the darkest ink pixel) it computes:
  core  share of glyph pixels at >= 90% ink   (sharpness: dense stems)
  mid   share of glyph pixels at 20..80% ink  (still antialiased, not 1-bit)
The shared kernel/app ink curve raises core coverage from 0.23-0.33 to
0.44-0.52 on this 4-bit logical-resolution atlas. Require core >= 0.40;
the old pre-boost-only renderer fails every row. Keep at least six distinct
intermediate levels and mid >= 0.30 for body text. The bold browse heading
has thicker stems and mid 0.19 after darkening, so its mid floor is 0.18.
A binary renderer still fails both the mid and intermediate-level checks.

Usage: python3 tools/checks/textsharp-check.py   (repo root, after make kernel.elf)
"""
import sys, tempfile, time
from pathlib import Path
from PIL import Image

sys.path.insert(0, str(Path(__file__).resolve().parent))
from jtvm import VM, SCALE

# Physical boxes inside the dock-launch window (viewport origin logical (78,72)),
# all on even physical coordinates so each box starts on a 2x2 block edge.
SENTENCE = "Hamburgefonstiv sphinx"
VIEWS = [
    ("Mail", "open=mail", "mail: n=", None,
     {"Mail hint line": (190, 214, 1130, 252), "Mail message row": (230, 276, 810, 310)}),
    ("Notes browse", "open=notes", "notes: folders=", None,
     {"Notes browse heading": (216, 266, 360, 298), "Notes browse note row": (548, 310, 730, 344)}),
    ("Notes editor", "open=notes", "notes: folders=", SENTENCE,
     {"Notes editor body line": (256, 196, 700, 244)}),
]
CORE_MIN, MID_MIN, LEVELS_MIN = 0.40, 0.30, 6


def logical_px(img, box):
    """One pixel per 2x2 block of the crop: what the app rasterized."""
    crop = img.crop(box).convert('L')
    small = crop.resize((crop.width // SCALE, crop.height // SCALE), Image.NEAREST)
    return list(small.get_flattened_data() if hasattr(small, "get_flattened_data") else small.getdata())


def has_text(img, box):
    px = logical_px(img, box)
    return max(set(px), key=px.count) - min(px) >= 60


shots = {}
for app, flag, ready, typed, rows in VIEWS:
    vm = VM(None, flag, ready, work=tempfile.mkdtemp(prefix='jt-textsharp-'))
    try:
        time.sleep(1.5)   # a key sent the instant the window opens can land before it polls
        if typed:
            vm.key('n')
            if not vm.wait('notes: edit=', 8):
                print(f'FAIL: {app}: n did not open a new note'); continue
            time.sleep(1.0)
            vm.type(typed, 0.1)
        vm.move(930, 300)   # pointer right of the window, clear of every sampled row
        # Wait for the view's own text to be on screen, not a fixed sleep: a
        # capture taken before the window presents measures the desktop.
        img, deadline = None, time.time() + 10
        while time.time() < deadline:
            time.sleep(0.5); img = vm.frame()
            if all(has_text(img, b) for b in rows.values()): break
        time.sleep(0.5)
        shots[app] = vm.frame()
    finally:
        vm.quit()

fail = 0
for app, flag, ready, typed, rows in VIEWS:
    img = shots.get(app)
    if img is None:
        print(f"FAIL: {app}: never captured"); fail = 1; continue
    for name, box in rows.items():
        px = logical_px(img, box)
        mid_min = 0.18 if name == "Notes browse heading" else MID_MIN
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
        print(f"{name}: {len(ink)} glyph px, core {core:.3f} (need >= {CORE_MIN}), mid {mid:.3f} (need >= {mid_min}), {levels} intermediate levels (need >= {LEVELS_MIN})")
        if core < CORE_MIN:
            print(f"FAIL: {name}: stems are soft, only {core:.1%} of glyph pixels are full ink (linear-coverage blending is back?)")
            fail = 1
        if mid < mid_min or levels < LEVELS_MIN:
            print(f"FAIL: {name}: edges have lost their antialiasing (binary/jagged text)")
            fail = 1
print("PASS: UI text has dense stems and still-antialiased edges (Mail, Notes browse view, Notes editor)" if not fail else "textsharp-check: FAILED")
sys.exit(fail)
