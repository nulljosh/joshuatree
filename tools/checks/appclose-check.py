#!/usr/bin/env python3
"""Headless proof that every dock app opens AND closes from the pointer
alone, the same QMP absolute-pointer + pmemsave shape as dockhover-check.py.

Why this exists (v67 / 0.62.2): "that notes app stuck glitch" was reported
from the landing page's live v86 demo, then re-reported as "all apps",
and the honest finding was narrower and real: Notes (kernel/editor.h) ran
its own mouse loop on mouse_get_delta only, never mouse_get_absolute, so
under v62's vmmouse backdoor (live on QEMU's default pc machine and in
v86) its pointer froze at the dock-click spot and the close hitbox could
never be reached; Chat's message box read get_key() only, click-blind.
Every other app goes through gui_wait_close/get_key_or_click and was
fine. The "all apps" impression was the modal Notes window sitting over
the still-visible dock, so every dock click after it looked dead.

For every dock slot (Apps folder, Files, Mail, Calendar, Notes, Reminders,
Terminal, Chat, Weather, Stocks, Trash), in order:
  1. click the slot and assert its window opened; regular windows have a
     red close button, while Apps has a glass panel instead;
  2. close it from the pointer and assert the desktop is back.
Then the "all apps" scenario itself: open Notes, click the Reminders
dock slot with Notes still open, and assert the screen is no longer stuck
on Notes. Finally open and close Mail once more to prove input survived
the whole sweep. Any app that stays open fails the run by name.

Discriminating: on the v66 kernel Notes and Chat both fail step 2 (proven
before the fix landed, see roadmap.md's v67 entry).

Usage: tools/checks/appclose-check.py   (from the repo root, after make kernel.elf)
"""
import json, os, socket, subprocess, sys, time
from PIL import Image

LOG = "/tmp/jt-appclose-serial.log"
DUMP = "/tmp/jt-appclose.raw"
FB = 0xfd000000; W, H = 1920, 1080
PORT = 4451
LOGICAL_W, LOGICAL_H, SCALE = 960, 540, 2
DOCK_ICON, DOCK_GAP, SLOT0_X = 37, 6, 247
PITCH = DOCK_ICON + DOCK_GAP
ICON_ROW_Y = 487
CLOSE_X, CLOSE_Y = 94, 56          # gui_launch_from_dock: red circle at (x+24, y+16) for x=70, y=40
APPS_PANEL_X, APPS_PANEL_Y = 480, 95  # glass panel, above its grid tiles
CLOSE_RED = (0xFF, 0x5F, 0x57)
PARK = (480, 200)                  # open wallpaper, away from every hit target
SLOTS = ["Apps", "Files", "Mail", "Calendar", "Notes", "Reminders", "Terminal", "Chat", "Weather", "Stocks", "Trash"]

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
    for _ in range(50):  # QEMU's QMP socket can take a few seconds to come up on a loaded machine
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
    # QMP becoming available does not mean the guest has finished booting.
    # On loaded CI runners the old fixed five-second delay began the sweep
    # before the desktop could handle clicks, losing the first few apps.

    def move(x, y):
        cmd({"execute": "input-send-event", "arguments": {"events": [
            {"type": "abs", "data": {"axis": "x", "value": int(x * 32768 / LOGICAL_W)}},
            {"type": "abs", "data": {"axis": "y", "value": int(y * 32768 / LOGICAL_H)}}]}})
    def click():
        cmd({"execute": "input-send-event", "arguments": {"events": [{"type": "btn", "data": {"down": True, "button": "left"}}]}})
        time.sleep(0.1)
        cmd({"execute": "input-send-event", "arguments": {"events": [{"type": "btn", "data": {"down": False, "button": "left"}}]}})
    def pixel(x, y):
        cmd({"execute": "pmemsave", "arguments": {"val": FB, "size": W * H * 4, "filename": DUMP}})
        img = Image.frombytes("RGBA", (W, H), open(DUMP, "rb").read(), "raw", "BGRA").convert("RGB")
        return img.getpixel((x * SCALE + 1, y * SCALE + 1))  # +1: inside the s x s block, never its seam
    def is_red(p): return max(abs(p[i] - CLOSE_RED[i]) for i in range(3)) <= 12
    # Left padding of the dock tray, away from icons and rounded corners.
    # Its opaque color appears only once the GUI has presented the desktop.
    for _ in range(120):
        if pixel(239, 500) == (0xEF, 0xEB, 0xE4):
            break
        time.sleep(0.25)
    else:
        raise SystemExit("FAIL: desktop dock did not appear within 30 seconds")
    time.sleep(0.3)  # let the input loop begin after its first presentation
    def close_button():
        """Regular app window's red button, or None."""
        if is_red(pixel(CLOSE_X, CLOSE_Y)): return (CLOSE_X, CLOSE_Y)
        return None
    def window_open(): return close_button() is not None
    centre = lambda slot: SLOT0_X + slot * PITCH + DOCK_ICON // 2
    def keys(*qcodes):
        cmd({"execute": "send-key", "arguments": {"keys": [{"type": "qcode", "data": k} for k in qcodes]}})

    def open_slot(slot):
        move(centre(slot), ICON_ROW_Y); time.sleep(0.3)
        click(); time.sleep(1.2)
    def close_via_x():
        # v0.76.18: real CI flake found and fixed, not hand-waved. Run 111
        # (GitHub Actions, not reproduced in ~10 local runs) failed here on
        # the very first slot with "still open after clicking its close
        # button" -- then the sweep's own esc-key recovery immediately
        # closed it and every remaining slot passed clean, proving the
        # window really did close, just not within this function's old
        # single fixed 0.8s wait on a more loaded/slower CI runner. A fixed
        # sleep-then-check has no way to tell "genuinely stuck" from "closed
        # one frame later than usual" apart; polling does, without weakening
        # the real assertion (still fails if truly stuck after the same
        # ~2s worst case this used to allow only 0.8s of).
        at = close_button()
        if at is None: return
        move(*at); time.sleep(0.3)
        click()
        for _ in range(20):
            time.sleep(0.1)
            if not window_open(): break
        move(*PARK); time.sleep(0.5)

    move(*PARK); time.sleep(0.5)
    if window_open(): fails.append("desktop: red close button visible before anything was opened (sampling point is wrong)")
    apps_desktop_pixel = pixel(APPS_PANEL_X, APPS_PANEL_Y)

    def apps_open():
        p = pixel(APPS_PANEL_X, APPS_PANEL_Y)
        return max(abs(p[i] - apps_desktop_pixel[i]) for i in range(3)) > 12

    def close_apps():
        # The Apps folder has no traffic-light button. A click outside its
        # grid closes it; check the panel itself disappears from framebuffer.
        move(900, 250); time.sleep(0.3)
        click()
        for _ in range(20):
            time.sleep(0.1)
            if not apps_open(): break
        move(*PARK); time.sleep(0.5)

    for slot, name in enumerate(SLOTS):
        open_slot(slot)
        opened = apps_open() if name == "Apps" else window_open()
        print(f"{name:9s} open: {'yes' if opened else 'NO'}", end="  ")
        if not opened:
            fails.append(f"{name}: dock click did not open a window"); print(); continue
        if name == "Notes":
            # Real typed text, so the close also runs the dirty-save path. This
            # boot has no disk (same as the landing page's v86), so the save
            # fails; the close must still go through, text kept in RAM.
            for k in ("h", "i"): keys(k); time.sleep(0.15)
            time.sleep(0.5)
        if name == "Apps": close_apps()
        else: close_via_x()
        closed = not (apps_open() if name == "Apps" else window_open())
        print(f"close via pointer: {'yes' if closed else 'NO, still open'}")
        if not closed:
            fails.append(f"{name}: still open after pointer close")
            # Try to recover so the sweep can go on: esc is the keyboard exit every app honours.
            for _ in range(2):
                if window_open(): keys("esc"); time.sleep(0.8)

    # The reported "all apps" shape: Notes open, then a dock click on another app.
    # v67 made that click close Notes; v68 (0.63.0) makes it open the clicked app
    # in Notes' place, the "close-and-open" the report literally asked for.
    # Terminal is the target because its content is unmistakable: it clears its
    # viewport to 0x1A1512, so the viewport centre tells Terminal apart from Notes
    # (cream), from the desktop (no red close button), and from any other app.
    open_slot(4)
    if not window_open(): fails.append("scenario: Notes did not open")
    else:
        move(centre(6), ICON_ROW_Y); time.sleep(0.3); click(); time.sleep(1.2)
        move(*PARK); time.sleep(0.5)
        p = pixel(78 + 400, 72 + 50)
        still_notes = max(abs(p[i] - (0xEA, 0xE4, 0xDC)[i]) for i in range(3)) <= 8
        c = pixel(78 + 402, 72 + 172)
        terminal = window_open() and max(abs(c[i] - (0x1A, 0x15, 0x12)[i]) for i in range(3)) <= 8
        print(f"scenario  Notes open, click Terminal in dock: {'STUCK on Notes' if still_notes else ('Terminal opened in its place' if terminal else 'Notes closed, Terminal NOT opened')}")
        if still_notes: fails.append("scenario: dock click with Notes open left the screen stuck on Notes")
        elif not terminal: fails.append("scenario: dock click with Notes open closed Notes but did not open Terminal (v68 close-and-open)")
        if window_open(): close_via_x()
        for _ in range(2):
            if window_open(): keys("esc"); time.sleep(0.8)

    open_slot(2)
    ok = window_open()
    if ok: close_via_x(); ok = not window_open()
    print(f"after sweep, Mail open+close again: {'yes' if ok else 'NO'}")
    if not ok: fails.append("input dead after the sweep: Mail could not be opened and closed again")
    # QEMU can tear down the QMP socket the instant it processes quit,
    # before this side ever reads a reply -- a real race, not a bug in
    # the assertions above (which already ran); a reset here must not
    # mask a genuine PASS as a crash.
    try: cmd({"execute": "quit"})
    except (ConnectionResetError, BrokenPipeError, OSError): pass
finally:
    try: q.wait(timeout=5)
    except subprocess.TimeoutExpired: q.kill()

if fails:
    for x in fails: print("FAIL:", x)
    sys.exit(1)
print("PASS: every dock app opens and closes from the pointer alone, input intact afterwards")
