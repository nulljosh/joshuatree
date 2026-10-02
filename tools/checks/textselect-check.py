#!/usr/bin/env python3
"""Headless, real-pixel proof of text selection in the ring-3 Notes window
(user/notes.c). One QEMU boot through tools/checks/jtvm.py with the kernel's
`open=notes` boot flag, like clipboard-check.py: `n` makes a fresh, known-empty
note, and every position asserted below is read off the framebuffer.

Notes is a ring-3 program, so it speaks on serial through write(1), which the
kernel logs behind "syscall: write(1) from ring 3: ". Its markers are
edsel=<start>,<end> (every Shift+arrow and Ctrl+A), edcopy=<n> and edcut=<n>
(a copy or cut that took a selection). This proves the real thing:

  1. Type a sentence. Shift+Left four times selects its last four
     characters. A real, distinctive highlight band (0xB4D5FE, drawn
     behind the glyphs) appears on screen, and ONLY under those four
     glyphs -- scanned across the whole framebuffer, not just guessed at,
     so a highlight that leaked over the wrong range or the wrong line
     would fail this just as loudly as no highlight at all. The kernel's
     own `edsel=<start>,<end>` serial marker must report exactly that
     4-character range.
     The band must start at the caret (the maroon bar now sits before
     the 4th-last glyph) and end at the line's last ink, so a band over
     the wrong four glyphs fails even if the marker is right.
  2. Ctrl+C copies the selection: `edcopy=4` plus the clipboard's own
     `CLIPCOPY:4:<fnv1a>` marker (booted with `cliptrace`), whose hash
     must be exactly the hash of the four selected characters, not of
     the whole line. End clears the selection and returns the caret to
     the true end of the line, Ctrl+V pastes the copied text back on
     (`CLIPPASTE:4:<same hash>`) -- real new ink appears just past the
     old end of line, where the framebuffer was plain background.
  3. Shift+Left x4 again, Ctrl+X: `edcut=4`, the same clipboard hash,
     and the pasted ink is gone again (the row is back to how it looked
     before the paste).
  4. Shift+Left x5 selects "chars"; typing one letter replaces all five.
     Shift+Left x1, Delete removes that letter. Two more letters typed.
     Then the collapsed-anchor regression: End, Shift+Right at the very
     end (anchor lands on the caret, no real selection), then two more
     letters. Before the fix the second one replaced the first. Ctrl+A's
     `edsel=0,<n>` marker is the exact-length witness for every step.
  5. Backspace on that select-all empties the note -- the body is real
     background again, no leftover glyph ink anywhere (the caret's own
     maroon bar is not glyph ink and is excluded on purpose).

Discriminating: without a selection model Shift+Left is a plain Left, no
0xB4D5FE band is ever drawn and no edsel marker is ever written, so step 1's
highlight scan and marker check fail loudly.

Usage: tools/checks/textselect-check.py   (from the repo root, after make kernel.elf)
"""
import os, sys, time
from pathlib import Path
from PIL import Image

sys.path.insert(0, str(Path(__file__).resolve().parent))
from jtvm import VM

os.chdir(Path(__file__).resolve().parent.parent.parent)
SCALE = 2
SENTENCE = "select four chars"          # 17 characters, no shift/digits needed
HILITE = (0xB4, 0xD5, 0xFE)
CARET = (0x85, 0x14, 0x4B)
BG = (0xFA, 0xF8, 0xF6)
MARK = "syscall: write(1) from ring 3: "   # the kernel's own prefix for a ring-3 write(1)

def fnv1a(text):
    h = 2166136261
    for b in text.encode():
        h = ((h ^ b) * 16777619) & 0xFFFFFFFF
    return f"{h:08x}"

# Ring-3 Notes: the dock-launched window sits at x=70,y=40 (w=820), its
# viewport at (x+8,y+32); user/notes.c draws text from ED_MARGIN=56 and the
# first line at ED_TOP=28, caret 24px tall starting 2px above the glyph row.
# Absolute logical: text left x = 70+8+56 = 134, caret/band rows 102..125.
TEXT_TOP, TEXT_LEFT = 102, 134
ROW_BOTTOM = TEXT_TOP + 26                # one line's worth, generous
BODY_TOP, BODY_BOTTOM = 98, 300           # scanned for the "note is empty" proof (the "Notes *" title sits above 98)
# Notes' own window is x=70,w=820, content inset 8px each side, so real
# content stops at 70+820-8=882; stay a few px inside that or the scan picks
# up the satellite wallpaper just past the window's right edge, which is dark
# and can false-positive as "ink".
WIN_CONTENT_RIGHT = 875
WIN = (70, 890, 40, 425)                  # x0, x1, y0, y1 of the Notes window rect

fails = []
def fail(msg): fails.append(msg); print("  FAIL: " + msg)
def ok(msg): print("  ok:   " + msg)

vm = VM(None, "cliptrace open=notes", "notes: ring-3 window")
try:
    keep = []
    def dump():
        # A 2x2 block's centre pixel is the logical pixel (nearest resize samples (2x+1, 2y+1)).
        im = vm.frame().resize((960, 540), Image.NEAREST)
        keep.append(im)
        return im.load()
    def close(p1, p2, tol=10): return max(abs(p1[i] - p2[i]) for i in range(3)) <= tol

    def key(c, gap=0.08): vm.key(c, gap=gap)
    def keys(chord, gap=0.15): vm.key(chord, gap=gap)
    def type_str(t):
        vm.type(t, gap=0.08)
    def shift_left(n=1):
        for _ in range(n): keys("shift-left", gap=0.2)

    def serial_text(): return vm.serial()
    def wait_marker(marker, timeout=15.0):
        vm.wait(marker, timeout)
        return serial_text()
    def marks(prefix):
        return [l.split(MARK, 1)[1] for l in serial_text().splitlines() if MARK + prefix in l]

    # Scan the logical framebuffer for pixels matching a given colour; returns the (x,y) hits.
    def find_color(img, color, tol=10, x0=0, x1=960, y0=0, y1=540):
        return [(x, y) for y in range(y0, y1) for x in range(x0, x1) if close(img[x, y], color, tol)]

    def is_text_ink(p):
        # Real glyph ink: dark and roughly neutral (R~=G~=B). Excludes the
        # caret's maroon bar (0x85144B: R much greater than G) and the
        # selection highlight (0xB4D5FE: B much greater than R) on purpose,
        # so "the note is empty" isn't defeated by the caret's own pixels.
        if close(p, BG, 10): return False
        return max(p) < 210 and (max(p) - min(p)) < 30

    time.sleep(1.5)
    vm.key("n")
    if not vm.wait("notes: edit=", 10): raise RuntimeError("Notes: n did not open a fresh note in the editor")
    time.sleep(1.0)
    img0 = dump()
    caret0 = find_color(img0, CARET, tol=12, x0=TEXT_LEFT - 6, x1=TEXT_LEFT + 10, y0=TEXT_TOP - 6, y1=ROW_BOTTOM + 6)
    if not caret0: raise RuntimeError("Notes opened but no caret at the expected spot: the text-area geometry in this check is stale")
    ok(f"fresh empty note open, caret at x={min(x for x, _ in caret0)}")

    # ---- 1: type, select the last 4 characters, prove the highlight ----
    type_str(SENTENCE)
    time.sleep(0.5)
    img_typed = dump()
    pre_hits = find_color(img_typed, HILITE, x0=TEXT_LEFT - 4, x1=WIN_CONTENT_RIGHT, y0=TEXT_TOP - 2, y1=ROW_BOTTOM + 2)
    if pre_hits: fail(f"highlight colour already present before any selection was made ({len(pre_hits)} px)")
    else: ok("no highlight before a selection exists")

    shift_left(4)
    # Wait for the kernel's own marker for THIS range (the seeded-note
    # clear above already left an earlier edsel= line in the log), then
    # give the redraw a moment before sampling the framebuffer: on a
    # loaded runner the four Shift+Left presses can land later than the
    # sleeps in shift_left assume.
    expect_sel = f"edsel={len(SENTENCE) - 4},{len(SENTENCE)}"
    log = wait_marker(expect_sel)
    time.sleep(0.4)
    img_sel = dump()
    if expect_sel in log: ok(f"serial reported {expect_sel}")
    else: fail(f"expected '{expect_sel}' in the serial log, got: " + ", ".join(l for l in log.splitlines() if l.startswith("edsel=")))

    # The highlight must appear, and ONLY over roughly the last 4 glyphs'
    # width -- scanned across the whole visible text row and well past it,
    # not just guessed at a hardcoded x.
    hits = find_color(img_sel, HILITE, x0=TEXT_LEFT - 4, x1=WIN_CONTENT_RIGHT, y0=TEXT_TOP - 2, y1=ROW_BOTTOM + 2)
    if not hits:
        fail("no highlight-coloured pixels found anywhere near the text row after Shift+Left x4")
    else:
        xs = [x for x, _ in hits]
        span = max(xs) - min(xs)
        # A size-1 Sans glyph run of 4 characters is well under 100 logical
        # px; the untouched left ~14 characters of "select four chars"
        # would add well over 150px if the highlight leaked onto them.
        if span > 100:
            fail(f"highlight spans {span}px, far wider than 4 glyphs -- looks like it covers more than the selection")
        else:
            ok(f"highlight band present, {len(hits)} px, {span}px wide (roughly 4 glyphs)")
        # WHERE it sits: the caret now stands at position 14, just left of
        # the four selected glyphs, and the line's last ink marks their
        # right edge. Both come from the framebuffer, not a font table.
        caret_xs = [x for x, _ in find_color(img_sel, CARET, tol=12, x0=TEXT_LEFT - 4, x1=WIN_CONTENT_RIGHT, y0=TEXT_TOP - 2, y1=ROW_BOTTOM + 2)]
        # Measured on img_sel, not img_typed: the glyph cores stay dark and
        # neutral under the band, and img_typed may predate the last few
        # keystrokes landing on a slow runner.
        typed_ink_xs = [x for y in range(TEXT_TOP, ROW_BOTTOM) for x in range(TEXT_LEFT - 4, WIN_CONTENT_RIGHT) if is_text_ink(img_sel[x, y])]
        if not caret_xs or not typed_ink_xs:
            fail("could not find the caret or the typed line's ink to anchor the highlight position against")
        else:
            caret_x, line_end = min(caret_xs), max(typed_ink_xs)
            if line_end <= caret_x:
                fail(f"the line's last ink (x={line_end}) is not right of the caret (x={caret_x}); the selected glyphs did not render")
            if min(xs) < caret_x - 4:
                fail(f"highlight starts at x={min(xs)}, left of the caret at x={caret_x}: it covers glyphs before the selection")
            elif min(xs) > caret_x + 4:
                fail(f"highlight starts at x={min(xs)}, well right of the caret at x={caret_x}: the first selected glyph is not highlighted")
            elif max(xs) < line_end - 4:
                fail(f"highlight ends at x={max(xs)}, short of the line's last ink at x={line_end}: the last selected glyph is not highlighted")
            else:
                ok(f"highlight runs from the caret (x={caret_x}) to the line's last ink (x={line_end})")
        # Nothing highlighted anywhere else in the Notes window: count the
        # highlight colour across the whole window rect before and after the
        # selection; only the band itself may be new. (A before/after diff, not
        # an absolute count, because the rounded corners show satellite
        # wallpaper, which can hold light blues of its own.)
        w0, w1, wy0, wy1 = WIN
        whole_before = len(find_color(img_typed, HILITE, x0=w0, x1=w1, y0=wy0, y1=wy1))
        whole_after = len(find_color(img_sel, HILITE, x0=w0, x1=w1, y0=wy0, y1=wy1))
        if whole_after - whole_before != len(hits):
            fail(f"found {whole_after - whole_before - len(hits)} highlight-coloured pixel(s) OUTSIDE the expected text row")
        else:
            ok("highlight colour appears nowhere else in the window")

    # ---- 2: Ctrl+C, End (clears selection, caret to true end), Ctrl+V ----
    keys("ctrl-c"); time.sleep(0.2)
    log = wait_marker("edcopy=4")
    if "edcopy=4" in log: ok("serial reported edcopy=4")
    else: fail("expected 'edcopy=4' in the serial log after Ctrl+C on a 4-char selection")
    TAIL = SENTENCE[-4:]
    copy_marker = f"CLIPCOPY:4:{fnv1a(TAIL)}"
    log = wait_marker(copy_marker, timeout=3.0)
    if copy_marker in log: ok(f"clipboard holds exactly '{TAIL}' ({copy_marker})")
    else: fail(f"expected '{copy_marker}' (the hash of the 4 selected characters), got: " + ", ".join(l for l in log.splitlines() if l.startswith("CLIPCOPY:")))

    keys("end"); time.sleep(0.2)
    img_before_paste = dump()
    sel_cleared = not find_color(img_before_paste, HILITE, x0=TEXT_LEFT - 4, x1=WIN_CONTENT_RIGHT, y0=TEXT_TOP - 2, y1=ROW_BOTTOM + 2)
    if sel_cleared: ok("End cleared the selection highlight")
    else: fail("highlight still present after End (a plain nav key should clear the selection)")

    # Real end-of-line x: the rightmost non-background pixel (ink or caret)
    # in the text row before the paste -- derived from the framebuffer
    # itself, not a guessed font-metric offset.
    row_pixels = [(x, img_before_paste[x, y]) for y in range(TEXT_TOP, ROW_BOTTOM) for x in range(TEXT_LEFT - 4, WIN_CONTENT_RIGHT)]
    non_bg_xs = [x for x, p in row_pixels if not close(p, BG, 10)]
    end_x = max(non_bg_xs) if non_bg_xs else TEXT_LEFT
    probe = (end_x + 4, end_x + 160)

    def ink_count(img):
        return sum(1 for y in range(TEXT_TOP, ROW_BOTTOM) for x in range(*probe) if is_text_ink(img[x, y]))
    before_ink = ink_count(img_before_paste)

    keys("ctrl-v"); time.sleep(0.3)
    paste_marker = f"CLIPPASTE:4:{fnv1a(TAIL)}"
    log = wait_marker(paste_marker)
    if paste_marker in log: ok(f"pasted exactly '{TAIL}' ({paste_marker})")
    else: fail(f"expected '{paste_marker}' after Ctrl+V, got: " + ", ".join(l for l in log.splitlines() if l.startswith("CLIPPASTE:")))
    img_after_paste = dump()
    after_ink = ink_count(img_after_paste)
    print(f"ink pixels just past the old line end: before paste={before_ink} after paste={after_ink}")
    if after_ink > before_ink + 15:
        ok("pasted text rendered as real new ink where the line used to end")
    else:
        fail("no real new ink appeared past the old end of line after Ctrl+V")

    # ---- 3: select the pasted tail again, Ctrl+X cuts it back out ----
    n = len(SENTENCE) + 4  # "select four charshars"
    shift_left(4); time.sleep(0.2)
    log = wait_marker(f"edsel={n - 4},{n}")
    if f"edsel={n - 4},{n}" in log: ok(f"serial reported edsel={n - 4},{n} (the pasted tail)")
    else: fail(f"expected 'edsel={n - 4},{n}' after Shift+Left x4 on the pasted line")
    copies_before = serial_text().count(copy_marker)
    keys("ctrl-x"); time.sleep(0.3)
    log = wait_marker("edcut=4")
    if "edcut=4" in log: ok("serial reported edcut=4")
    else: fail("expected 'edcut=4' after Ctrl+X on a 4-char selection")
    if serial_text().count(copy_marker) == copies_before + 1: ok(f"cut put exactly '{TAIL}' on the clipboard")
    else: fail(f"expected one more '{copy_marker}' after Ctrl+X")
    img_after_cut = dump()
    cut_ink = ink_count(img_after_cut)
    print(f"ink pixels just past the old line end after cut={cut_ink}")
    if cut_ink <= before_ink + 15: ok("cut removed the pasted ink from the screen")
    else: fail(f"ink past the old line end is still {cut_ink} px after Ctrl+X (was {before_ink} before the paste)")
    n -= 4

    # ---- 4: typing and Delete over a selection; the collapsed-anchor bug ----
    shift_left(5); time.sleep(0.2)
    log = wait_marker(f"edsel={n - 5},{n}")
    if f"edsel={n - 5},{n}" in log: ok(f"serial reported edsel={n - 5},{n} (the last word)")
    else: fail(f"expected 'edsel={n - 5},{n}' after Shift+Left x5")
    key("x"); n = n - 5 + 1                      # typing replaces the 5 selected chars
    shift_left(1); time.sleep(0.2)
    keys("delete"); n -= 1                       # Delete removes the 1 selected char
    type_str("ab"); n += 2
    keys("end"); time.sleep(0.1)
    keys("shift-right"); time.sleep(0.15)     # at the end: anchor == caret, not a selection
    type_str("yz"); n += 2                       # the buggy kernel replaced 'y' with 'z' here
    time.sleep(0.2)
    keys("ctrl-a"); time.sleep(0.2)
    expect_all = f"edsel=0,{n}"
    log = wait_marker(expect_all)
    if expect_all in log: ok(f"serial reported {expect_all}: type-over, Delete and typing after a collapsed anchor all kept the exact length")
    else: fail(f"expected '{expect_all}' after Ctrl+A, got: " + ", ".join(l for l in log.splitlines() if l.startswith("edsel=0,")))

    # ---- 4b: Esc clears the selection and does NOT close the editor ----
    time.sleep(0.3)
    row = dict(x0=TEXT_LEFT - 4, x1=WIN_CONTENT_RIGHT, y0=TEXT_TOP - 2, y1=ROW_BOTTOM + 2)
    if not find_color(dump(), HILITE, **row): fail("select-all drew no highlight band before the Esc test")
    keys("esc"); time.sleep(0.5)
    if find_color(dump(), HILITE, **row): fail("highlight still present after Esc")
    elif "notes: closed" in serial_text() or serial_text().count("notes: saved=") > 0: fail("Esc with a selection closed the editor instead of just clearing the selection")
    else: ok("Esc cleared the selection and left the editor open")
    keys("ctrl-a"); time.sleep(0.2)
    if not vm.wait(expect_all, 10, count=2): fail("Ctrl+A after Esc did not select everything again")

    # ---- 5: Backspace on the select-all empties the note ----
    keys("backspace"); time.sleep(0.4)
    img_empty = dump()
    ink_left = sum(1 for y in range(BODY_TOP, BODY_BOTTOM) for x in range(TEXT_LEFT - 4, WIN_CONTENT_RIGHT) if is_text_ink(img_empty[x, y]))
    print(f"glyph-ink pixels left in the body after Ctrl+A, Backspace: {ink_left}")
    if ink_left == 0: ok("note body is empty, no leftover glyph ink")
    else: fail(f"{ink_left} glyph-ink pixel(s) remain in the body after selecting all and deleting")

except Exception as e:
    fails.append(f"exception: {e}")
finally:
    tail = vm.serial()[-500:]
    vm.quit()

if fails:
    print("FAIL:")
    for m in fails: print("  - " + m)
    print("serial tail: " + repr(tail))
    sys.exit(1)
print("PASS: Shift+Left highlights exactly the last 4 characters where the caret says, Ctrl+C/V/X move exactly those bytes (clipboard hashes), typing/Delete replace a selection, a collapsed anchor never eats a character, and Ctrl+A + Backspace empties the note")
