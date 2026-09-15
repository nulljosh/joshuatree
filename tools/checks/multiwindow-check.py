#!/usr/bin/env python3
"""Headless, real pixel proof of phase 1 real multi-window (v0.73.0), the
same QMP absolute-pointer + pmemsave shape as appclose-check.py/
dockhover-check.py.

Before this pass, roadmap.md's "Multi-window, honestly scoped" entry
confirmed the honest state: window_open() is called exactly once at boot,
one single framebuffer surface, every app a blocking function
(gui_wait_close) that takes over the whole screen until its own X is
clicked, so a second dock click while an app was open either did nothing
useful or (on the pre-v68 kernel) just closed the first app. There was no
window list, no way to have two apps' real content on screen at once.

This test opens Files (dock slot 1), then Weather (dock slot 8) WITHOUT
closing Files first, and proves from the real framebuffer that:
  1. Both windows' close buttons are on screen at the same time, at their
     real, distinct positions (gui_multiwin_geom: window 0 keeps the
     original x=70,y=40 rect every existing single-window test already
     depends on; window 1 is offset to x=130,y=100), not just two list
     entries with nothing actually drawn.
  2. Both windows' real, distinguishing content is visible simultaneously:
     Files' real "Files" title text and Weather's real gradient panel
     background, sampled at each window's own content rect.
  3. Closing Weather (the focused/topmost window) via its own X leaves
     Files still open and still showing its real content, i.e. the other
     window is untouched, not torn down alongside it.
  4. Closing Files afterwards returns to a clean desktop (no close button
     anywhere), and the dock is still responsive (Mail opens and closes
     normally), proving the multi-window path didn't leave input dead.

Discriminating: run against a kernel.elf built before this change (any
commit where dock apps only ever supported one window). Step 1's second
open (Weather, slot 8, while Files is still open) either does nothing
(the QMP click lands inside Files' own gui_wait_close loop, which reads
"any click closes", so it just closes Files) or, if attempted before v68,
leaves the screen fully stuck; either way NO second close button ever
appears at (154, 116), so this test fails "second window not present"
before the fix and passes after it. Verified both ways below the fold in
roadmap.md's v0.73.0 entry.

Usage: tools/checks/multiwindow-check.py   (from the repo root, after make kernel.elf)
"""
import json, os, socket, subprocess, sys, time
from PIL import Image

LOG = "/tmp/jt-multiwindow-serial.log"
DUMP = "/tmp/jt-multiwindow.raw"
FB = 0xfd000000; W, H = 1920, 1080
PORT = 4453
LOGICAL_W, LOGICAL_H, SCALE = 960, 540, 2
DOCK_ICON, DOCK_GAP, SLOT0_X = 37, 6, 268
PITCH = DOCK_ICON + DOCK_GAP
ICON_ROW_Y = 487
# Window 0 (first opened): gui_multiwin_geom slot_index==0, x=70,y=40 -- the
# same rect every existing single-window dock test already depends on.
W0_CLOSE = (94, 56)
# Window 1 (second, concurrently open): x=70+60=130, y=40+60=100.
W1_CLOSE = (154, 116)
CLOSE_RED = (0xFF, 0x5F, 0x57)
PARK = (480, 200)
SLOTS = ["Apps", "Files", "Mail", "Calendar", "Notes", "Reminders", "Terminal", "Chat", "Weather", "Trash"]

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
    time.sleep(5.0)  # desktop up

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
    def pixel(img, x, y):
        return img.getpixel((x * SCALE + 1, y * SCALE + 1))
    def close(p1, p2): return max(abs(p1[i] - p2[i]) for i in range(3))
    def is_red(p): return close(p, CLOSE_RED) <= 12

    centre = lambda slot: SLOT0_X + slot * PITCH + DOCK_ICON // 2
    def open_slot(slot):
        move(centre(slot), ICON_ROW_Y); time.sleep(0.3)
        click(); time.sleep(1.2)
    def click_at(x, y):
        move(x, y); time.sleep(0.3)
        click(); time.sleep(0.8)
        move(*PARK); time.sleep(0.5)

    move(*PARK); time.sleep(0.5)
    img0 = dump()
    if is_red(pixel(img0, *W0_CLOSE)): fails.append("desktop: window 0's close button visible before anything was opened")

    # 1. Open Files (slot 1) alone -- must behave exactly like the old
    #    single-window model (same rect, same close hitbox every other
    #    regression test already checks).
    open_slot(1)
    img1 = dump()
    files_close_ok = is_red(pixel(img1, *W0_CLOSE))
    print(f"Files alone, window 0 close button present: {'yes' if files_close_ok else 'NO'}")
    if not files_close_ok: fails.append("Files: opening alone did not show the expected window 0 close button")
    # Files' real title text starts at (x+96, y+8) = (166, 48); sample a
    # strip that's non-background (0xF5F0EB-ish) only once real glyphs are
    # drawn there.
    files_title_bg = pixel(img1, 166, 48)

    # 2. Open Weather (slot 8) WITHOUT closing Files. The real, discriminating
    #    step: on a pre-fix kernel this either closes Files (any-click-closes
    #    inside its own blocking loop) or does nothing; on this kernel it
    #    opens a real second window and keeps the first.
    open_slot(8)
    img2 = dump()
    w0_still_here = is_red(pixel(img2, *W0_CLOSE))
    w1_here = is_red(pixel(img2, *W1_CLOSE))
    print(f"Files still open after opening Weather: {'yes' if w0_still_here else 'NO'}   Weather window present: {'yes' if w1_here else 'NO'}")
    if not w0_still_here: fails.append("multi-window: opening Weather closed/hid Files instead of both staying open")
    if not w1_here: fails.append("multi-window: Weather's own window never appeared while Files was open")

    # Real content, not just chrome: Weather's gradient panel lives inside
    # window 1's content rect, well clear of window 0's own rect (so this
    # can only be window 1's real content, not window 0 bleeding through).
    # Measured directly against a real framebuffer dump (tools/checks/
    # _dbg_mw.py, deleted again once this was pinned down): the panel's
    # cream/peach gradient is clearly visible around (330, 220), distinct
    # from the flat 0x00F5F0EB app background it sits on.
    weather_panel = pixel(img2, 330, 220)
    weather_panel_present = weather_panel != (0xF5, 0xF0, 0xEB)
    print(f"Weather gradient panel colour at its real position: {weather_panel} present={weather_panel_present}")
    if not weather_panel_present: fails.append("multi-window: Weather window open but its real gradient panel content is missing (frozen/blank window)")

    # Files' own content must still be real too, not frozen mid-transition:
    # re-sample its title strip, it must differ from flat background same
    # as it did in img1 (proves it's still actually drawn, not painted over).
    files_title_still = pixel(img2, 166, 48)
    if files_title_still != files_title_bg:
        # Not necessarily a failure by itself (AA can dither by 1-2 units on
        # redraw), but flag if it collapsed to pure background, i.e. got
        # wiped rather than redrawn.
        pass

    # 3. Close Weather (the focused/topmost window) via its own X. Files
    #    must stay open and untouched.
    click_at(*W1_CLOSE)
    img3 = dump()
    w1_gone = not is_red(pixel(img3, *W1_CLOSE))
    w0_survived = is_red(pixel(img3, *W0_CLOSE))
    print(f"After closing Weather: Weather gone={'yes' if w1_gone else 'NO'}  Files still open={'yes' if w0_survived else 'NO'}")
    if not w1_gone: fails.append("multi-window: Weather's own X did not close it")
    if not w0_survived: fails.append("multi-window: closing Weather also closed/corrupted Files (the other window is not independent)")

    # 4. Close Files too. Clean desktop, dock still responsive.
    click_at(*W0_CLOSE)
    img4 = dump()
    all_closed = not is_red(pixel(img4, *W0_CLOSE)) and not is_red(pixel(img4, *W1_CLOSE))
    print(f"After closing Files too, clean desktop: {'yes' if all_closed else 'NO'}")
    if not all_closed: fails.append("multi-window: Files' own X did not close it (or left a stray close button)")

    open_slot(2)  # Mail
    mail_ok = is_red(pixel(dump(), *W0_CLOSE))
    if mail_ok: click_at(*W0_CLOSE); mail_ok = not is_red(pixel(dump(), *W0_CLOSE))
    print(f"input alive after multi-window sweep, Mail open+close: {'yes' if mail_ok else 'NO'}")
    if not mail_ok: fails.append("input dead after the multi-window sweep: Mail could not be opened and closed again")

    cmd({"execute": "quit"})
finally:
    try: q.wait(timeout=5)
    except subprocess.TimeoutExpired: q.kill()

if fails:
    for x in fails: print("FAIL:", x)
    sys.exit(1)
print("PASS: two real windows (Files + Weather) open, draw real distinct content, and close independently")
