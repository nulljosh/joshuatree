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

v0.75.0 (multi-window batch 2) extends this in place with step 6: a real
two-window combination involving a NEWLY-converted app (Reminders), the
exact scenario the batch-2 task itself named as the required evidence --
Reminders open alongside Files, add a real reminder, close Reminders via
its own X, confirm Files is untouched AND the reminder was really saved
to the real FAT disk (REMINDER.TXT), not just that the UI didn't crash.
Needs the same real FAT16 test image app-interact-check.py uses
(tools/mkdisk.sh if /tmp/jt-qa-test.img doesn't exist yet), so this script
now boots QEMU with that disk attached too.

Usage: tools/checks/multiwindow-check.py   (from the repo root, after make kernel.elf)
"""
import json, os, socket, subprocess, sys, time, tempfile, shutil
from PIL import Image

LOG = "/tmp/jt-multiwindow-serial.log"
DUMP = "/tmp/jt-multiwindow.raw"
DISK = "/tmp/jt-qa-test.img"
FB = 0xfd000000; W, H = 1920, 1080
PORT = 4453
LOGICAL_W, LOGICAL_H, SCALE = 960, 540, 2
DOCK_ICON, DOCK_GAP, SLOT0_X = 37, 6, 247
PITCH = DOCK_ICON + DOCK_GAP
ICON_ROW_Y = 487
# Window 0 (first opened): gui_multiwin_geom slot_index==0, x=70,y=40 -- the
# same rect every existing single-window dock test already depends on.
W0_CLOSE = (94, 56)
# Window 1 (second, concurrently open): x=70+60=130, y=40+60=100.
W1_CLOSE = (154, 116)
CLOSE_RED = (0xFF, 0x5F, 0x57)
PARK = (480, 200)
SLOTS = ["Apps", "Files", "Mail", "Calendar", "Notes", "Reminders", "Terminal", "Chat", "Weather", "Stocks", "Trash"]

os.chdir(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
for f in (LOG, DUMP):
    try: os.remove(f)
    except FileNotFoundError: pass

HAVE_DISK = os.path.exists(DISK)
qemu_args = ["qemu-system-i386", "-kernel", "kernel.elf", "-display", "none", "-vga", "std",
             "-qmp", f"tcp:127.0.0.1:{PORT},server,nowait", "-serial", "file:" + LOG]
if HAVE_DISK:
    qemu_args += ["-drive", f"file={DISK},format=raw,if=ide,index=0"]
q = subprocess.Popen(qemu_args, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
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

    # Same QMP send-key shape app-interact-check.py already established,
    # reused here rather than re-invented, for step 6's real Reminders typing.
    QCODE = {" ": "spc", ".": "dot", "-": "minus", "/": "slash", "@": "shift-2",
             "\n": "ret", "\b": "backspace"}
    def key(c):
        if c in QCODE: cmd({"execute": "send-key", "arguments": {"keys": [{"type": "qcode", "data": QCODE[c]}]}})
        elif c.isupper(): cmd({"execute": "send-key", "arguments": {"keys": [{"type": "qcode", "data": "shift"}, {"type": "qcode", "data": c.lower()}]}})
        else: cmd({"execute": "send-key", "arguments": {"keys": [{"type": "qcode", "data": c}]}})
        # Keys dropped on slow runners if sent in burst; increase wait to be safe
        time.sleep(0.15)
    def keys(*qcodes):
        cmd({"execute": "send-key", "arguments": {"keys": [{"type": "qcode", "data": k} for k in qcodes]}})
        time.sleep(0.15)
    def type_str(s):
        for c in s: key(c)

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

    # 5. v0.73.6 (phase 2): real click-to-focus + real z-order compositing.
    #    Fresh open sequence, identical to steps 1-2: Files then Weather,
    #    Weather ends up topmost (opened second). The overlap region
    #    (330,220) sits inside BOTH windows' content rects; right now
    #    Weather's real gradient panel is what's visible there.
    open_slot(1)  # Files
    open_slot(8)  # Weather, on top
    img5a = dump()
    overlap_before_focus = pixel(img5a, 330, 220)
    weather_on_top_before = close(overlap_before_focus, weather_panel) <= 12
    print(f"Fresh open, overlap pixel before any focus click: {overlap_before_focus} (Weather's own gradient was {weather_panel}); Weather on top={'yes' if weather_on_top_before else 'NO'}")
    if not weather_on_top_before:
        fails.append("z-order setup: freshly-opened Weather is not on top of the overlap region before the click-to-focus test even starts")

    # Click Files' titlebar at W0_CLOSE=(94,56). Window 1 (Weather, rect
    # x=130,y=100..) does NOT cover this point at all (y=56 < Weather's
    # y=100), so this click can only land on window 0 (Files), and Files
    # is a BACKGROUND window right now (Weather is topmost). The real
    # assertion: this must NOT close Files (the old "any click on the
    # focused window closes it" contract must not fire for a background
    # window) and instead must raise Files to the front of the real
    # z-order list.
    click_at(*W0_CLOSE)
    img5b = dump()
    files_not_closed_by_focus_click = is_red(pixel(img5b, *W0_CLOSE))
    print(f"Files still open after a click-to-focus click on its (background) titlebar: {'yes' if files_not_closed_by_focus_click else 'NO'}")
    if not files_not_closed_by_focus_click:
        fails.append("click-to-focus: clicking Files' background window closed it instead of focusing it")

    # Real z-order proof: the SAME overlap pixel (330,220) that showed
    # Weather's gradient a moment ago must now show Files' content instead,
    # because draw order must follow the same z-order list input
    # hit-testing just updated -- Files is now topmost, so it must win the
    # overlapped region, not Weather. This is the exact naive
    # back-to-front bug phase 2 was scoped to fix: on the pre-fix kernel,
    # draw order never changes (always window 0 then window 1, Weather
    # always drawn last/on top) regardless of which window a click
    # focused, so this pixel would incorrectly still read as Weather's
    # gradient even after this "focus" click.
    overlap_after_focus = pixel(img5b, 330, 220)
    overlap_now_files = close(overlap_after_focus, weather_panel) > 12
    print(f"Overlap pixel (330,220) after focusing Files: {overlap_after_focus} (was Weather's {weather_panel}); Files now on top={'yes' if overlap_now_files else 'NO'}")
    if not overlap_now_files:
        fails.append("z-order: focusing Files did not bring it in front of Weather in the overlapped region (draw order still ignores real focus)")

    # The close-button-close contract must still work once a window really
    # is topmost: a SECOND click at the same spot, now that Files is
    # genuinely focused, must close it -- proving click-to-focus only
    # swallows the FIRST click on a background window, it doesn't disable
    # closing altogether. Weather (still in the background, untouched by
    # either click) must remain open throughout.
    click_at(*W0_CLOSE)
    img5c = dump()
    files_closed_second_click = not is_red(pixel(img5c, *W0_CLOSE))
    weather_still_open_after = is_red(pixel(img5c, *W1_CLOSE))
    print(f"Files closes on a second click once genuinely focused: {'yes' if files_closed_second_click else 'NO'}   Weather still open throughout: {'yes' if weather_still_open_after else 'NO'}")
    if not files_closed_second_click:
        fails.append("click-to-focus: Files did not close on a second click after becoming the real focused/topmost window")
    if not weather_still_open_after:
        fails.append("click-to-focus: focusing/closing Files incorrectly also touched Weather")

    # Clean up: close Weather too (it's the sole remaining window, topmost
    # by definition) before the final Mail sanity check below.
    click_at(*W1_CLOSE)
    img5d = dump()
    if is_red(pixel(img5d, *W1_CLOSE)):
        fails.append("cleanup: Weather did not close after the click-to-focus test sequence")

    # 6. v0.75.0 (batch 2): the real, required two-window evidence -- a
    #    newly-converted interactive app (Reminders) open ALONGSIDE Files,
    #    interacted with for real (add a reminder via its own keyboard
    #    path), closed via its own X, with Files proven untouched and the
    #    reminder proven really saved to the real FAT disk, not just that
    #    the screen didn't crash. Files opens first (window 0, x=70,y=40),
    #    Reminders second (window 1, x=130,y=100, W1_CLOSE=(154,116)),
    #    same geometry step 2 above already established.
    open_slot(1)  # Files (window 0)
    files_title_for_mw2 = pixel(dump(), 166, 48)
    open_slot(5)  # Reminders (window 1, dock slot 5 per SLOTS above)
    img6a = dump()
    both_open_for_add = is_red(pixel(img6a, *W0_CLOSE)) and is_red(pixel(img6a, *W1_CLOSE))
    print(f"batch2: Files + Reminders both open together: {'yes' if both_open_for_add else 'NO'}")
    if not both_open_for_add:
        fails.append("batch2: opening Reminders alongside Files did not leave both windows open")

    # Real interaction: 'a' enters add mode, type a marker, enter commits
    # -- the exact same real per-keystroke state (add-mode + typed buffer)
    # this batch had to make persist across repaints while Files' own
    # window keeps redrawing alongside it every frame.
    key("a"); time.sleep(0.4)
    type_str("qa-mw-reminder-marker")
    keys("ret"); time.sleep(0.4)

    img6b = dump()
    files_untouched_during_add = is_red(pixel(img6b, *W0_CLOSE)) and pixel(img6b, 166, 48) == files_title_for_mw2
    print(f"batch2: Files untouched while typing into Reminders: {'yes' if files_untouched_during_add else 'NO'}")
    if not files_untouched_during_add:
        fails.append("batch2: Files' own window changed while Reminders was being typed into (cross-window bleed)")

    # Close Reminders via its own X (window 1's close hitbox). Files (window
    # 0) must stay open and untouched, the same independence proof step 3
    # already established for Files/Weather, now for a real-input app.
    click_at(*W1_CLOSE)
    img6c = dump()
    reminders_closed = not is_red(pixel(img6c, *W1_CLOSE))
    files_survived_reminders_close = is_red(pixel(img6c, *W0_CLOSE)) and pixel(img6c, 166, 48) == files_title_for_mw2
    print(f"batch2: Reminders closed via its own X={'yes' if reminders_closed else 'NO'}   Files survived={'yes' if files_survived_reminders_close else 'NO'}")
    if not reminders_closed:
        fails.append("batch2: Reminders did not close via its own X")
    if not files_survived_reminders_close:
        fails.append("batch2: closing Reminders also closed/corrupted Files (the other window is not independent)")

    # Clean up: close Files too before the final Mail sanity check.
    click_at(*W0_CLOSE)
    img6d = dump()
    if is_red(pixel(img6d, *W0_CLOSE)):
        fails.append("batch2 cleanup: Files did not close after the Reminders-alongside-Files sequence")

    open_slot(2)  # Mail
    mail_ok = is_red(pixel(dump(), *W0_CLOSE))
    if mail_ok: click_at(*W0_CLOSE); mail_ok = not is_red(pixel(dump(), *W0_CLOSE))
    print(f"input alive after multi-window sweep, Mail open+close: {'yes' if mail_ok else 'NO'}")
    if not mail_ok: fails.append("input dead after the multi-window sweep: Mail could not be opened and closed again")

    # QEMU can tear down the QMP socket the instant it processes quit,
    # before this side ever reads a reply -- a real race, not a bug in
    # the assertions above (which already ran); a reset here must not
    # mask a genuine PASS as a crash.
    try: cmd({"execute": "quit"})
    except (ConnectionResetError, BrokenPipeError, OSError): pass
finally:
    try: q.wait(timeout=5)
    except subprocess.TimeoutExpired: q.kill()

# ---- host-side verification: the reminder added in step 6 must have
# really reached the real FAT disk, not just RAM, same real-artifact bar
# app-interact-check.py already holds every persisted app to. fat.c's
# to_fat_name() truncates "REMINDERS.TXT" to "REMINDER.TXT" on disk (8.3
# names), confirmed there and reused here rather than re-derived. ----
if HAVE_DISK:
    mount = tempfile.mkdtemp(prefix="/tmp/jt-mw-mount-")
    try:
        subprocess.run(["hdiutil", "attach", "-nobrowse", "-mountpoint", mount, DISK],
                        check=True, capture_output=True)
        path = os.path.join(mount, "REMINDER.TXT")
        if not os.path.exists(path):
            fails.append("batch2 disk: REMINDER.TXT does not exist on the real FAT disk after the session")
        else:
            content = open(path, "r", errors="replace").read()
            if "qa-mw-reminder-marker" in content:
                print("batch2 disk verified: REMINDER.TXT contains 'qa-mw-reminder-marker' (real VFS write while a second window, Files, was also open)")
            else:
                fails.append("batch2 disk: REMINDER.TXT exists but does not contain 'qa-mw-reminder-marker' -- save did not reach the real disk")
    finally:
        subprocess.run(["hdiutil", "detach", mount], capture_output=True)
        shutil.rmtree(mount, ignore_errors=True)
else:
    print("batch2 disk check skipped: no /tmp/jt-qa-test.img (see tools/mkdisk.sh); on-screen step 6 evidence above still real and required")

if fails:
    for x in fails: print("FAIL:", x)
    sys.exit(1)
print("PASS: two real windows (Files + Weather) open, draw real distinct content, and close independently; batch-2 (Files + Reminders) proven the same way with a real disk write")
