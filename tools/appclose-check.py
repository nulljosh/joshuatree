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
Terminal, Chat, Weather, Trash), in order:
  1. click the slot, dump the real framebuffer, assert the window chrome's
     red close button is on screen (0xFF5F57 at logical (94,56), a colour
     the wallpaper never has);
  2. click that red button, park the pointer away from it, dump again,
     assert the red button is gone (the desktop is back).
Then the "all apps" scenario itself: open Notes, click the Reminders
dock slot with Notes still open, and assert the screen is no longer stuck
on Notes. Finally open and close Mail once more to prove input survived
the whole sweep. Any app that stays open fails the run by name.

Discriminating: on the v66 kernel Notes and Chat both fail step 2 (proven
before the fix landed, see roadmap.md's v67 entry).

Usage: tools/appclose-check.py   (from the repo root, after make kernel.elf)
"""
import json, os, socket, subprocess, sys, time
from PIL import Image

LOG = "/tmp/jt-appclose-serial.log"
DUMP = "/tmp/jt-appclose.raw"
FB = 0xfd000000; W, H = 1920, 1080
PORT = 4451
LOGICAL_W, LOGICAL_H, SCALE = 960, 540, 2
DOCK_ICON, DOCK_GAP, SLOT0_X = 37, 6, 268
PITCH = DOCK_ICON + DOCK_GAP
ICON_ROW_Y = 487
CLOSE_X, CLOSE_Y = 94, 56          # gui_launch_from_dock: red circle at (x+24, y+16) for x=70, y=40
APPS_CLOSE_X, APPS_CLOSE_Y = 80, 46  # the Apps folder's own larger window origin (56, 30)
CLOSE_RED = (0xFF, 0x5F, 0x57)
PARK = (480, 200)                  # open wallpaper, away from every hit target
SLOTS = ["Apps", "Files", "Mail", "Calendar", "Notes", "Reminders", "Terminal", "Chat", "Weather", "Trash"]

os.chdir(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
for f in (LOG, DUMP):
    try: os.remove(f)
    except FileNotFoundError: pass

q = subprocess.Popen(["qemu-system-i386", "-kernel", "kernel.elf", "-display", "none", "-vga", "std",
                      "-qmp", f"tcp:127.0.0.1:{PORT},server,nowait", "-serial", "file:" + LOG],
                     stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
fails = []
try:
    time.sleep(1.0)
    s = socket.create_connection(("127.0.0.1", PORT)); f = s.makefile("rw")
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
    def pixel(x, y):
        cmd({"execute": "pmemsave", "arguments": {"val": FB, "size": W * H * 4, "filename": DUMP}})
        img = Image.frombytes("RGBA", (W, H), open(DUMP, "rb").read(), "raw", "BGRA").convert("RGB")
        return img.getpixel((x * SCALE + 1, y * SCALE + 1))  # +1: inside the s x s block, never its seam
    def is_red(p): return max(abs(p[i] - CLOSE_RED[i]) for i in range(3)) <= 12
    def close_button():
        """Where the red button is right now: the app window's, the Apps folder's, or None."""
        if is_red(pixel(CLOSE_X, CLOSE_Y)): return (CLOSE_X, CLOSE_Y)
        if is_red(pixel(APPS_CLOSE_X, APPS_CLOSE_Y)): return (APPS_CLOSE_X, APPS_CLOSE_Y)
        return None
    def window_open(): return close_button() is not None
    centre = lambda slot: SLOT0_X + slot * PITCH + DOCK_ICON // 2
    def keys(*qcodes):
        cmd({"execute": "send-key", "arguments": {"keys": [{"type": "qcode", "data": k} for k in qcodes]}})

    def open_slot(slot):
        move(centre(slot), ICON_ROW_Y); time.sleep(0.3)
        click(); time.sleep(1.2)
    def close_via_x():
        at = close_button()
        if at is None: return
        move(*at); time.sleep(0.3)
        click(); time.sleep(0.8)
        move(*PARK); time.sleep(0.5)

    move(*PARK); time.sleep(0.5)
    if window_open(): fails.append("desktop: red close button visible before anything was opened (sampling point is wrong)")

    for slot, name in enumerate(SLOTS):
        open_slot(slot)
        opened = window_open()
        print(f"{name:9s} open: {'yes' if opened else 'NO'}", end="  ")
        if not opened:
            fails.append(f"{name}: dock click did not open a window"); print(); continue
        if name == "Notes":
            # Real typed text, so the close also runs the dirty-save path. This
            # boot has no disk (same as the landing page's v86), so the save
            # fails; the close must still go through, text kept in RAM.
            for k in ("h", "i"): keys(k); time.sleep(0.15)
            time.sleep(0.5)
        close_via_x()
        closed = not window_open()
        print(f"close via X: {'yes' if closed else 'NO, still open'}")
        if not closed:
            fails.append(f"{name}: still open after clicking its close button")
            # Try to recover so the sweep can go on: esc is the keyboard exit every app honours.
            for _ in range(2):
                if window_open(): keys("esc"); time.sleep(0.8)

    # The reported "all apps" shape: Notes open, then a dock click on another app.
    open_slot(4)
    if not window_open(): fails.append("scenario: Notes did not open")
    else:
        move(centre(5), ICON_ROW_Y); time.sleep(0.3); click(); time.sleep(1.2)
        move(*PARK); time.sleep(0.5)
        # Either outcome is acceptable here (Notes closed, or Reminders opened in its place);
        # what is NOT acceptable is the screen still being Notes: its toolbar strip is
        # 0xEAE4DC at logical (78+400, 72+50) inside the viewport, no other app draws that there.
        p = pixel(78 + 400, 72 + 50)
        still_notes = max(abs(p[i] - (0xEA, 0xE4, 0xDC)[i]) for i in range(3)) <= 8
        print(f"scenario  Notes open, click Reminders in dock: {'STUCK on Notes' if still_notes else 'not stuck'}")
        if still_notes: fails.append("scenario: dock click with Notes open left the screen stuck on Notes")
        if window_open(): close_via_x()
        for _ in range(2):
            if window_open(): keys("esc"); time.sleep(0.8)

    open_slot(2)
    ok = window_open()
    if ok: close_via_x(); ok = not window_open()
    print(f"after sweep, Mail open+close again: {'yes' if ok else 'NO'}")
    if not ok: fails.append("input dead after the sweep: Mail could not be opened and closed again")
    cmd({"execute": "quit"})
finally:
    try: q.wait(timeout=5)
    except subprocess.TimeoutExpired: q.kill()

if fails:
    for x in fails: print("FAIL:", x)
    sys.exit(1)
print("PASS: every dock app opens and closes from the pointer alone, input intact afterwards")
