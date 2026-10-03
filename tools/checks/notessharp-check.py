"""Headless sharpness check for Notes' text: real antialiased TrueType, not a
scaled-up bitmap.

Notes is user/notes.c now and draws through libjt's runtime-TTF faces. The old
in-kernel Notes had a size control from 12pt to 200pt; the ring-3 one has a
single body size (libjt's jt_text_draw takes a face, never a point size), so
the two-ends-of-the-range half of this check has nothing left to drive. What
it proved is still true and still checked: types "Ag" into a new note, takes
the physical framebuffer, reads it back at the window's own pixel grid and proves

  - real antialiasing: glyph pixels at intermediate coverage (20..80% ink),
    the signal that catches a binary/jagged renderer;
  - no 2x2 duplicated blocks on the glyph edges: a nearest-neighbour bitmap
    upscale (the bug this guards against) makes every edge block four copies
    of one value; real per-pixel rasterization does not.

assert_sharp() below is shared: termsharp-check.py loads this file by path and
reuses it against the Terminal's crop.

Usage: python3 tools/checks/notessharp-check.py   (repo root, after make kernel.elf)
"""
import sys, tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent
sys.path.insert(0, str(Path(__file__).resolve().parent))
SCALE = 2
VIEW_X, VIEW_Y = 78, 72     # the dock-opened window's viewport
TEXT_X, TEXT_Y = 56, 34     # user/notes.c: ED_MARGIN, ED_TOP + 6, where the first line's glyphs start


def assert_sharp(crop, label):
    """The actual sharpness measurement, factored out so other checks (the
    Terminal's mono grid, termsharp-check.py) can reuse it against their own
    QEMU/pmemsave crop instead of re-deriving the antialiasing/duplicated-
    block logic. crop is a PIL 'L' (grayscale) image of just the glyph
    region. Returns nothing; raises AssertionError on failure, same as the
    inline version this replaced."""
    w, h = crop.size
    px = list(crop.getdata())
    bg = max(set(px), key=px.count)
    # Notes is dark ink on a light page (fg = darkest pixel); the Terminal
    # is light text on a dark surface (fg = brightest pixel). Pick
    # whichever extreme sits farther from the background mode so the same
    # measurement works for either polarity.
    fg = min(px) if (bg - min(px)) >= (max(px) - bg) else max(px)
    span = abs(bg - fg)
    assert span >= 60, f'{label}: no text found (surface {bg}, extreme {fg})'
    levels = [abs(bg - p) / span for p in px if abs(bg - p) / span > 0.08]
    mid = sum(0.2 < a < 0.8 for a in levels)
    assert mid >= 8, f'{label}: no intermediate coverage values, edges are not antialiased ({mid} mid-tone px)'
    # No 2x2 duplicated blocks, checked on the EDGE pixels specifically:
    # a solid glyph interior legitimately has same-value neighbours
    # under any renderer, real or upscaled, so that's not a useful
    # signal. A nearest-neighbour bitmap upscale (the bug this guards
    # against) instead stretches its edge pixels too, so an edge block
    # -- one containing an intermediate-coverage pixel -- comes out as
    # four identical values far more often than real per-pixel
    # rasterization, which recomputes coverage at every physical pixel
    # independently.
    edge_xy = {(i % w, i // w) for i, p in enumerate(px) if 0.08 < abs(bg - p) / span < 0.92}
    assert edge_xy, f'{label}: no antialiased edge pixels to check for duplicated blocks'
    uniform, total = 0, 0
    seen = set()
    for (ex, ey) in edge_xy:
        bx, by = ex - ex % 2, ey - ey % 2
        if (bx, by) in seen or bx + 1 >= w or by + 1 >= h:
            continue
        seen.add((bx, by))
        block = [crop.getpixel((bx + dx, by + dy)) for dy in (0, 1) for dx in (0, 1)]
        total += 1
        if len(set(block)) == 1:
            uniform += 1
    duplicated_ratio = uniform / total if total else 0
    assert duplicated_ratio < 0.5, (
        f'{label}: {duplicated_ratio:.0%} of edge-pixel 2x2 blocks are duplicated pixels '
        f'(looks like a nearest-neighbour bitmap upscale, not real rasterization)')
    print(f'PASS: {label}: {mid} mid-tone px, {duplicated_ratio:.0%} duplicated 2x2 blocks (< 50%)')


def check_notes_face():
    from jtvm import VM
    vm = VM(None, 'open=notes', 'notes: folders=', work=tempfile.mkdtemp(prefix='jt-notessharp-'))
    try:
        vm.key('n')
        assert vm.wait('notes: edit=', 8), 'n did not open a new note in the editor'
        import time
        time.sleep(1.0)   # a key sent the instant the editor opens can land before it polls
        vm.type('Ag')
        time.sleep(0.8)
        img = vm.frame().convert('L')
        x0, y0 = (VIEW_X + TEXT_X - 6) * SCALE, (VIEW_Y + TEXT_Y - 6) * SCALE
        crop = img.crop((x0, y0, x0 + 90 * SCALE, y0 + 44 * SCALE))
        # A ring-3 window's buffer is logical resolution; the compositor shows each
        # buffer pixel as a 2x2 physical block. So the physical crop is duplicated
        # blocks by construction. Sample one pixel per block to get back the pixels
        # the app itself rasterized, and hold THOSE to the bar: real coverage values
        # at the glyph edges, and no duplicated 2x2 blocks among them (which would
        # mean Notes drew a half-resolution bitmap and stretched it twice).
        from PIL import Image
        logical = crop.resize((crop.width // 2, crop.height // 2), Image.NEAREST)
        assert_sharp(logical, 'Notes body face "Ag" (logical pixels)')
    finally:
        vm.quit()


if __name__ == '__main__':
    # Guarded so termsharp-check.py can load this module by path for
    # assert_sharp without a second Notes/QEMU run firing.
    check_notes_face()
    print('PASS: Notes renders real antialiased TrueType, no duplicated-block upscaling')
