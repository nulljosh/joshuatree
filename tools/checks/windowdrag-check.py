#!/usr/bin/env python3
"""Headless, real-pixel proof that app windows drag live by their title
bar (direct request, 1.0.x: "app windows should be draggable").

Case 1, a blocking single-window app (Notes, the shape every app except
the five multi-window ones uses): open it from the dock, type a line,
press the title bar right of the traffic lights, move the pointer in real
steps, release. Proves from the framebuffer that the red close light now
sits at the moved position and no longer at the old one, that the typed
content moved with the chrome (a crop of the old content region matches
the same crop at the new offset), that typing afterwards lands inside the
moved window and nowhere near the old rect, and that the close light
still closes it at its new place. The kernel's own `windrag` serial
marker (gui_app_mouse_tick) must have fired.

Case 2, a multi-window app (Files): the window must move LIVE, mid-drag,
before release (the old code drew only a snap-zone outline and left the
window put until the button came up), and land at the free-move
position on release.

Discriminating: on the kernel before this change a title-bar press on
Notes is a plain "click anywhere closes" click, so Notes vanishes and no
close light exists anywhere (case 1 fails), and Files stays at x=70,y=40
until release (case 2's mid-drag assertion fails).

Usage: tools/checks/windowdrag-check.py   (from the repo root, after make kernel.elf)
"""
import json, os, socket, subprocess, sys, time, tempfile
from PIL import Image, ImageChops

ART = tempfile.mkdtemp(prefix="jt-windowdrag-")
LOG = os.path.join(ART, "serial.log")
DUMP = os.path.join(ART, "fb.raw")
FB = 0xfd000000; W, H = 1920, 1080
PORT = 4463
LOGICAL_W, LOGICAL_H, SCALE = 960, 540, 2
DOCK_ICON, DOCK_GAP, SLOT0_X = 37, 6, 247
PITCH = DOCK_ICON + DOCK_GAP
ICON_ROW_Y = 487
CLOSE_RED = (0xFF, 0x5F, 0x57)
WIN_X, WIN_Y, WIN_W, WIN_H = 70, 40, 820, 385   # gui_launch_from_dock / gui_multiwin_geom slot 0
TITLE = (WIN_X + 300, WIN_Y + 10)                # title band, right of the lights
PARK = (480, 460)
SLOT_FILES, SLOT_NOTES = 1, 4

os.chdir(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
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
            r = json.loads(f.readline())
            if "return" in r or "error" in r: return r
    f.readline()
    cmd({"execute": "qmp_capabilities"})
    time.sleep(5.0)

    def move(x, y):
        cmd({"execute": "input-send-event", "arguments": {"events": [
            {"type": "abs", "data": {"axis": "x", "value": int(x * 32768 / LOGICAL_W)}},
            {"type": "abs", "data": {"axis": "y", "value": int(y * 32768 / LOGICAL_H)}}]}})
    def button(down):
        cmd({"execute": "input-send-event", "arguments": {"events": [{"type": "btn", "data": {"down": down, "button": "left"}}]}})
    def click():
        button(True); time.sleep(0.1); button(False)
    def dump():
        cmd({"execute": "pmemsave", "arguments": {"val": FB, "size": W * H * 4, "filename": DUMP}})
        return Image.frombytes("RGBA", (W, H), open(DUMP, "rb").read(), "raw", "BGRA").convert("RGB")
    def pixel(img, x, y): return img.getpixel((x * SCALE + 1, y * SCALE + 1))
    def is_red(p): return max(abs(p[i] - CLOSE_RED[i]) for i in range(3)) <= 12
    def crop(img, x, y, w, h): return img.crop((x * SCALE, y * SCALE, (x + w) * SCALE, (y + h) * SCALE))
    def differing_fraction(a, b):
        d = ImageChops.difference(a, b).convert("L").point(lambda v: 255 if v > 24 else 0)
        hist = d.histogram()
        return hist[255] / float(a.size[0] * a.size[1])
    def keys(text):
        for c in text:
            code = {" ": "spc"}.get(c, c)
            cmd({"execute": "send-key", "arguments": {"keys": [{"type": "qcode", "data": code}], "hold-time": 30}})
            time.sleep(0.08)
    centre = lambda slot: SLOT0_X + slot * PITCH + DOCK_ICON // 2
    def open_slot(slot):
        move(centre(slot), ICON_ROW_Y); time.sleep(0.3); click(); time.sleep(1.5)
    def drag_steps(sx, sy, tx, ty, steps=6, pause=0.2):
        for i in range(1, steps + 1):
            move(sx + (tx - sx) * i // steps, sy + (ty - sy) * i // steps); time.sleep(pause)
    def serial(): return open(LOG, "rb").read().decode("latin1")

    move(*PARK); time.sleep(0.4)

    # ---- case 1: Notes (single-window, blocking) ----
    print("case 1: Notes")
    # The default 820x385 window leaves 70 px of horizontal and ~10 px of
    # vertical room inside the desktop strip (menu bar to dock band), and
    # the kernel clamps the drag to that strip, so stay well inside it.
    DX, DY = 60, 8
    open_slot(SLOT_NOTES)
    img0 = dump()
    if not is_red(pixel(img0, WIN_X + 24, WIN_Y + 16)):
        raise SystemExit("FAIL: Notes did not open at its expected rect")
    keys("drag me across the desk"); time.sleep(0.8)
    img1 = dump()
    content_before = crop(img1, WIN_X + 12, WIN_Y + 60, 500, 60)
    move(*TITLE); time.sleep(0.3); button(True); time.sleep(0.25)
    drag_steps(TITLE[0], TITLE[1], TITLE[0] + DX, TITLE[1] + DY)
    time.sleep(0.3)
    mid = dump(); mid.save(os.path.join(ART, "notes-mid.png"))
    button(False); time.sleep(0.6)
    img2 = dump(); img2.save(os.path.join(ART, "notes-after.png")); img1.save(os.path.join(ART, "notes-before.png"))
    if is_red(pixel(img2, WIN_X + DX + 24, WIN_Y + DY + 16)): ok(f"close light moved to ({WIN_X + DX + 24},{WIN_Y + DY + 16})")
    else: fail("close light is not at the moved position after the drag")
    if not is_red(pixel(img2, WIN_X + 24, WIN_Y + 16)): ok("old close-light position is wallpaper again")
    else: fail("a close light is still drawn at the old position")
    if is_red(pixel(mid, WIN_X + DX + 24, WIN_Y + DY + 16)): ok("window was already at the new place mid-drag (live, not on release)")
    else: fail("window did not follow the pointer mid-drag")
    content_after = crop(img2, WIN_X + DX + 12, WIN_Y + DY + 60, 500, 60)
    frac = differing_fraction(content_before, content_after)
    if frac < 0.02: ok(f"typed content moved with the chrome (crop differs {frac*100:.2f}%)")
    else: fail(f"content did not move with the window (crop differs {frac*100:.1f}%)")
    # Notes opens with the caret at the end of the seeded note, so new text
    # lands somewhere in the content viewport, not at its top: compare the
    # whole moved viewport, and the strip of the old rect the new one no
    # longer covers (plain wallpaper now, must stay untouched).
    uncovered_before = crop(img2, WIN_X, WIN_Y + DY, DX - 2, WIN_H - DY)
    viewport_before = crop(img2, WIN_X + DX + 8, WIN_Y + DY + 32, WIN_W - 16, WIN_H - 40)
    keys(" and keep typing"); time.sleep(0.8)
    img3 = dump(); img3.save(os.path.join(ART, "notes-typed.png"))
    if differing_fraction(viewport_before, crop(img3, WIN_X + DX + 8, WIN_Y + DY + 32, WIN_W - 16, WIN_H - 40)) > 0.0005: ok("typing after the drag lands inside the moved window")
    else: fail("typing after the drag changed nothing inside the moved window")
    if differing_fraction(uncovered_before, crop(img3, WIN_X, WIN_Y + DY, DX - 2, WIN_H - DY)) < 0.001: ok("nothing drew into the uncovered part of the old rect")
    else: fail("something drew into the window's old, uncovered rect after the drag")
    if serial().count("windrag") > 0: ok("kernel logged windrag")
    else: fail("no windrag marker in the serial log")
    move(WIN_X + DX + 24, WIN_Y + DY + 16); time.sleep(0.3); click(); time.sleep(0.8)
    img4 = dump()
    if not is_red(pixel(img4, WIN_X + DX + 24, WIN_Y + DY + 16)): ok("close light at the new place still closes the app")
    else: fail("clicking the moved close light did not close Notes")
    move(*PARK); time.sleep(0.5)

    # ---- case 2: Files (multi-window path in gui_run) ----
    print("case 2: Files")
    DX, DY = 60, 8
    open_slot(SLOT_FILES)
    img0 = dump()
    if not is_red(pixel(img0, WIN_X + 24, WIN_Y + 16)):
        raise SystemExit("FAIL: Files did not open at its expected rect")
    move(*TITLE); time.sleep(0.3); button(True); time.sleep(0.25)
    drag_steps(TITLE[0], TITLE[1], TITLE[0] + DX, TITLE[1] + DY)
    time.sleep(0.4)
    mid = dump(); mid.save(os.path.join(ART, "files-mid.png"))
    if is_red(pixel(mid, WIN_X + DX + 24, WIN_Y + DY + 16)) and not is_red(pixel(mid, WIN_X + 24, WIN_Y + 16)):
        ok("Files followed the pointer live, before release")
    else: fail("Files did not move live mid-drag (old outline-only behaviour)")
    button(False); time.sleep(0.8)
    img2 = dump()
    if is_red(pixel(img2, WIN_X + DX + 24, WIN_Y + DY + 16)): ok("Files landed at the free-move position on release")
    else: fail("Files is not at the free-move position after release")
    move(WIN_X + DX + 24, WIN_Y + DY + 16); time.sleep(0.3); click(); time.sleep(0.8)
    img3 = dump()
    if not is_red(pixel(img3, WIN_X + DX + 24, WIN_Y + DY + 16)): ok("moved Files closes from its new close light")
    else: fail("moved Files did not close")
    try: cmd({"execute": "quit"})
    except Exception: pass
except Exception as e:
    fails.append(f"exception: {e}")
finally:
    try: q.wait(timeout=5)
    except Exception: q.kill()

if fails:
    print("FAIL:"); [print("  - " + m) for m in fails]; print(f"artifacts: {ART}"); sys.exit(1)
print("PASS: single-window (Notes) and multi-window (Files) windows both drag live by their title bar, content follows, input keeps landing in the moved window, and the moved close light still closes")
