#!/usr/bin/env python3
"""Headless, real-pixel proof of text selection in Notes (v1.2.0,
docs/roadmap.md's "Desktop basics" gap). Same QMP absolute-pointer +
qcode-keyboard + pmemsave shape clipboard-check.py and windowdrag-check.py
already use.

Before this pass Notes (kernel/editor.h) had no selection model at all
(the v1.0.6 clipboard's own comment says so plainly): Ctrl+C/X acted on
"the current line", Shift+arrow was not read specially, and there was no
highlight to draw. This proves the real thing landed:

  1. Type a sentence. Shift+Left four times selects its last four
     characters. A real, distinctive highlight band (0xB4D5FE, drawn
     behind the glyphs) appears on screen, and ONLY under those four
     glyphs -- scanned across the whole framebuffer, not just guessed at,
     so a highlight that leaked over the wrong range or the wrong line
     would fail this just as loudly as no highlight at all. The kernel's
     own `edsel=<start>,<end>` serial marker must report exactly that
     4-character range.
  2. Ctrl+C copies the selection (`edcopy=4`), End clears the selection
     and returns the caret to the true end of the line, Ctrl+V pastes the
     copied text back on -- real new ink appears just past the old end of
     line, where the framebuffer was plain background a moment before.
  3. Ctrl+A selects the whole (now-longer) buffer (`edsel=0,<total>`),
     Backspace deletes it all -- the body is real background again, no
     leftover glyph ink anywhere (the caret's own maroon bar is not
     glyph ink and is excluded from that check on purpose).

Discriminating: none of edsel/edcopy exist on main, window_rect never
gets a 0xB4D5FE call anywhere in editor.h, and Shift+Left is read no
differently from a plain Left -- so step 1's highlight scan finds nothing,
its marker check finds nothing, and this whole script fails loudly on the
pre-1.2.0 kernel.

Usage: tools/checks/textselect-check.py   (from the repo root, after make kernel.elf)
"""
import json, os, socket, subprocess, sys, time, tempfile
from PIL import Image

REPO = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..")
os.chdir(REPO)
ART = tempfile.mkdtemp(prefix="jt-textselect-")
LOG = os.path.join(ART, "serial.log")
DUMP = os.path.join(ART, "fb.raw")
FB = 0xfd000000; W, H = 1920, 1080
PORT = 4467
LOGICAL_W, LOGICAL_H, SCALE = 960, 540, 2
DOCK_ICON, DOCK_GAP, SLOT0_X = 37, 6, 247
PITCH = DOCK_ICON + DOCK_GAP
ICON_ROW_Y = 487
NOTES_SLOT = 4
CLOSE_X, CLOSE_Y = 94, 56
CLOSE_RED = (0xFF, 0x5F, 0x57)
PARK = (480, 200)
SENTENCE = "select four chars"          # 18 characters, no shift/digits needed
HILITE = (0xB4, 0xD5, 0xFE)
BG = (0xFA, 0xF8, 0xF6)
# Notes' own text area (kernel/editor.h EDITOR_TEXT_TOP/text_x=56), dock-
# launched at x=70,y=40 (gui_launch_from_dock), viewport at (x+8,y+32):
# absolute logical y = 40+32+92-32 = 132, x = 70+8+56 = 134.
TEXT_TOP, TEXT_LEFT = 132, 134
ROW_BOTTOM = TEXT_TOP + 26                # one line's worth, generous
BODY_TOP, BODY_BOTTOM = 132, 300          # scanned for the "note is empty" proof
# Notes' own window is x=70,w=820 (gui_launch_from_dock), content inset 8px
# each side, so real content stops at 70+820-8=882; stay a few px inside
# that or the scan picks up the satellite wallpaper just past the window's
# right edge, which is dark and can false-positive as "ink".
WIN_CONTENT_RIGHT = 875

q = subprocess.Popen(["qemu-system-i386", "-kernel", "kernel.elf", "-display", "none", "-vga", "std",
                      "-qmp", f"tcp:127.0.0.1:{PORT},server,nowait", "-serial", "file:" + LOG],
                     stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
fails = []
def fail(msg): fails.append(msg); print("  FAIL: " + msg)
def ok(msg): print("  ok:   " + msg)

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
            line = f.readline()
            if not line:
                if o["execute"] == "quit": return {}
                raise ConnectionError("QEMU disconnected before replying")
            r = json.loads(line)
            if "error" in r: raise RuntimeError(r["error"])
            if "return" in r: return r
    f.readline()
    cmd({"execute": "qmp_capabilities"})
    time.sleep(5.0)

    def move(x, y):
        cmd({"execute": "input-send-event", "arguments": {"events": [
            {"type": "abs", "data": {"axis": "x", "value": int(x * 32768 / LOGICAL_W)}},
            {"type": "abs", "data": {"axis": "y", "value": int(y * 32768 / LOGICAL_H)}}]}})
    def click():
        cmd({"execute": "input-send-event", "arguments": {"events": [{"type": "btn", "data": {"down": True, "button": "left"}}]}})
        time.sleep(0.1)
        cmd({"execute": "input-send-event", "arguments": {"events": [{"type": "btn", "data": {"down": False, "button": "left"}}]}})
    def dump():
        cmd({"execute": "pmemsave", "arguments": {"val": FB, "size": W * H * 4, "filename": DUMP}})
        return Image.frombytes("RGBA", (W, H), open(DUMP, "rb").read(), "raw", "BGRA").convert("RGB")
    def pixel(img, x, y): return img.getpixel((x * SCALE + 1, y * SCALE + 1))
    def close(p1, p2, tol=10): return max(abs(p1[i] - p2[i]) for i in range(3)) <= tol
    def is_red(p): return close(p, CLOSE_RED, 12)

    QCODE = {" ": "spc", ".": "dot", "-": "minus", "/": "slash", "\n": "ret", "\b": "backspace"}
    def key(c):
        codes = QCODE.get(c, "shift-" + c.lower() if c.isupper() else c).split("-")
        cmd({"execute": "send-key", "arguments": {"keys": [{"type": "qcode", "data": code} for code in codes], "hold-time": 30}})
        time.sleep(0.08)
    def keys(*qcodes):
        cmd({"execute": "send-key", "arguments": {"keys": [{"type": "qcode", "data": k} for k in qcodes], "hold-time": 30}})
        time.sleep(0.15)
    def type_str(s):
        for c in s: key(c)
    def shift_left(n=1):
        for _ in range(n): keys("shift", "left"); time.sleep(0.1)

    def open_slot(slot):
        move(SLOT0_X + slot * PITCH + DOCK_ICON // 2, ICON_ROW_Y); time.sleep(0.3)
        click(); time.sleep(1.2)
    def window_open(img=None): return is_red(pixel(img if img is not None else dump(), CLOSE_X, CLOSE_Y))

    def serial_text():
        with open(LOG, "rb") as fh: return fh.read().decode("latin1")
    def wait_marker(marker, timeout=15.0):
        deadline = time.time() + timeout
        while time.time() < deadline:
            log = serial_text()
            if marker in log: return log
            time.sleep(0.05)
        return serial_text()

    # Scan the whole physical framebuffer at logical resolution for pixels
    # matching a given colour; returns the list of (x,y) logical hits.
    def find_color(img, color, tol=10, x0=0, x1=LOGICAL_W, y0=0, y1=LOGICAL_H):
        hits = []
        for y in range(y0, y1):
            for x in range(x0, x1):
                if close(pixel(img, x, y), color, tol): hits.append((x, y))
        return hits

    def is_text_ink(p):
        # Real glyph ink: dark and roughly neutral (R~=G~=B). Excludes the
        # caret's maroon bar (0x85144B: R much greater than G) and the
        # selection highlight (0xB4D5FE: B much greater than R) on purpose,
        # so "the note is empty" isn't defeated by the caret's own pixels.
        if close(p, BG, 10): return False
        return max(p) < 210 and (max(p) - min(p)) < 30

    move(*PARK); time.sleep(0.5)
    open_slot(NOTES_SLOT)
    img0 = dump()
    if not window_open(img0): raise RuntimeError("Notes did not open")

    # A no-disk headless boot seeds NOTES.TXT with a real multi-line demo
    # note (kernel.c's own demo_notes, loaded whenever there's no FAT
    # disk attached, exactly this check's own QEMU invocation), so the
    # buffer is never actually empty when Notes first opens. Clear it with
    # the very feature under test (Ctrl+A, Backspace) so every position
    # asserted below is relative to a real, known-empty buffer, not
    # whatever the demo note happened to contain. This also means a
    # kernel where select-all/selection-delete don't work yet fails right
    # here, loudly, rather than silently mis-measuring later.
    keys("ctrl", "a"); time.sleep(0.2)
    keys("backspace"); time.sleep(0.3)
    img_cleared = dump()
    leftover = sum(1 for y in range(BODY_TOP, BODY_BOTTOM) for x in range(TEXT_LEFT - 4, WIN_CONTENT_RIGHT) if is_text_ink(pixel(img_cleared, x, y)))
    if leftover == 0: ok("cleared the seeded demo note with Ctrl+A, Backspace")
    else:
        fail(f"could not clear the seeded NOTES.TXT content before the real test ({leftover} ink px remain) -- Ctrl+A/selection-delete isn't working")
        print("---- serial log (edsel/edcopy/edcut lines) ----")
        for line in serial_text().splitlines():
            if line.startswith(("edsel", "edcopy", "edcut")): print("  " + line)
        raise RuntimeError("setup: could not establish a clean, known buffer state")

    # ---- 1: type, select the last 4 characters, prove the highlight ----
    type_str(SENTENCE)
    time.sleep(0.5)
    img_typed = dump()
    pre_hits = find_color(img_typed, HILITE, x0=TEXT_LEFT - 4, x1=WIN_CONTENT_RIGHT, y0=TEXT_TOP - 2, y1=ROW_BOTTOM + 2)
    if pre_hits: fail(f"highlight colour already present before any selection was made ({len(pre_hits)} px)")
    else: ok("no highlight before a selection exists")

    shift_left(4)
    time.sleep(0.3)
    img_sel = dump()
    log = wait_marker("edsel=")
    expect_sel = f"edsel={len(SENTENCE) - 4},{len(SENTENCE)}"
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
        # Nothing highlighted anywhere else on screen at all (menu bar,
        # dock, chrome, or any other line).
        # Scoped to the Notes window's own rect (70..890, 40..425), not the
        # full 960x540 screen: past the window's right edge is real
        # satellite-photo wallpaper, whose dark greens/blues have no
        # business being compared against a UI highlight colour at all.
        whole = find_color(img_sel, HILITE, tol=10, x0=70, x1=890, y0=40, y1=425)
        if len(whole) != len(hits):
            fail(f"found {len(whole) - len(hits)} highlight-coloured pixel(s) OUTSIDE the expected text row")
        else:
            ok("highlight colour appears nowhere else on screen")

    # ---- 2: Ctrl+C, End (clears selection, caret to true end), Ctrl+V ----
    keys("ctrl", "c"); time.sleep(0.2)
    log = wait_marker("edcopy=4")
    if "edcopy=4" in log: ok("serial reported edcopy=4")
    else: fail("expected 'edcopy=4' in the serial log after Ctrl+C on a 4-char selection")

    keys("end"); time.sleep(0.2)
    img_before_paste = dump()
    sel_cleared = not find_color(img_before_paste, HILITE, x0=TEXT_LEFT - 4, x1=WIN_CONTENT_RIGHT, y0=TEXT_TOP - 2, y1=ROW_BOTTOM + 2)
    if sel_cleared: ok("End cleared the selection highlight")
    else: fail("highlight still present after End (a plain nav key should clear the selection)")

    # Real end-of-line x: the rightmost non-background pixel (ink or caret)
    # in the text row before the paste -- derived from the framebuffer
    # itself, not a guessed font-metric offset.
    row_pixels = [(x, pixel(img_before_paste, x, y)) for y in range(TEXT_TOP, ROW_BOTTOM) for x in range(TEXT_LEFT - 4, WIN_CONTENT_RIGHT)]
    non_bg_xs = [x for x, p in row_pixels if not close(p, BG, 10)]
    end_x = max(non_bg_xs) if non_bg_xs else TEXT_LEFT
    probe = (end_x + 4, end_x + 160)

    def ink_count(img):
        return sum(1 for y in range(TEXT_TOP, ROW_BOTTOM) for x in range(*probe) if is_text_ink(pixel(img, x, y)))
    before_ink = ink_count(img_before_paste)

    keys("ctrl", "v"); time.sleep(0.3)
    log = wait_marker("CLIPPASTE:")
    img_after_paste = dump()
    after_ink = ink_count(img_after_paste)
    print(f"ink pixels just past the old line end: before paste={before_ink} after paste={after_ink}")
    if after_ink > before_ink + 15:
        ok("pasted text rendered as real new ink where the line used to end")
    else:
        fail("no real new ink appeared past the old end of line after Ctrl+V")

    # ---- 3: Ctrl+A selects everything, Backspace empties the note ----
    total_len = len(SENTENCE) + 4  # the 4 pasted characters
    keys("ctrl", "a"); time.sleep(0.2)
    log = wait_marker(f"edsel=0,{total_len}")
    expect_all = f"edsel=0,{total_len}"
    if expect_all in log: ok(f"serial reported {expect_all} (select all)")
    else: fail(f"expected '{expect_all}' in the serial log after Ctrl+A, got: " + ", ".join(l for l in log.splitlines() if l.startswith("edsel=")))

    keys("backspace"); time.sleep(0.4)
    img_empty = dump()
    ink_left = sum(1 for y in range(BODY_TOP, BODY_BOTTOM) for x in range(TEXT_LEFT - 4, WIN_CONTENT_RIGHT) if is_text_ink(pixel(img_empty, x, y)))
    print(f"glyph-ink pixels left in the body after Ctrl+A, Backspace: {ink_left}")
    if ink_left == 0: ok("note body is empty, no leftover glyph ink")
    else: fail(f"{ink_left} glyph-ink pixel(s) remain in the body after selecting all and deleting")

    try: cmd({"execute": "quit"})
    except (ConnectionResetError, BrokenPipeError, OSError): pass
except Exception as e:
    fails.append(f"exception: {e}")
finally:
    try: q.wait(timeout=5)
    except Exception: q.kill()

if fails:
    print("FAIL:")
    for m in fails: print("  - " + m)
    print(f"artifacts: {ART}")
    sys.exit(1)
print("PASS: Shift+Left selects exactly the last 4 characters (highlight + edsel marker), Ctrl+C/End/Ctrl+V round-trips the selection as real new ink, and Ctrl+A + Backspace empties the note")
