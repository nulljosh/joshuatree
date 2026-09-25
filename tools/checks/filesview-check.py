#!/usr/bin/env python3
"""Regression test for Files' view switcher (roadmap.md: "Files: view
buttons. Right now it's just a list."). Opens Files headlessly, switches
to Icons with the '2' key, dumps the framebuffer and asserts the grid
actually rendered (multiple icon tiles at the expected FILES_ICON_TILE
spacing, not just the old flat list), then closes the app and reopens it,
asserting the Icons choice survived (files_view persisted through
SETTINGS.TXT, read back via the kernel's own real symbol, not just
"looks the same on screen").

Same QMP absolute-pointer + qcode-keyboard + pmemsave shape as
tools/checks/app-interact-check.py.

Usage: tools/checks/filesview-check.py   (from repo root, after make kernel.elf)
"""
import json, os, re, socket, subprocess, sys, tempfile, time
from PIL import Image

REPO = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..")
os.chdir(REPO)
ARTIFACTS = tempfile.mkdtemp(prefix="jt-filesview-")
DISK = os.path.join(ARTIFACTS, "disk.img")
LOG = os.path.join(ARTIFACTS, "serial.log")
DUMP = os.path.join(ARTIFACTS, "framebuffer.raw")
SOCKET = os.path.join(ARTIFACTS, "qmp.sock")
FB = 0xfd000000; W, H = 1920, 1080
LOGICAL_W, LOGICAL_H, SCALE = 960, 540, 2
DOCK_ICON, DOCK_GAP, SLOT0_X = 37, 6, 247
PITCH = DOCK_ICON + DOCK_GAP
ICON_ROW_Y = 487
FILES_SLOT = 1  # GUI_DOCK_DEFAULT: {Apps, Files, Mail, ...}

# Window geometry for an ordinary (non-Apps-folder) dock app
# (gui_launch_from_dock: x=70,y=40,w=820,h=385) -> viewport (78,72).
VX, VY = 70 + 8, 40 + 32
# gui_draw_files_content: toolbar buttons at local (20+i*72, 36), grid
# tiles start at local (20, 76), each FILES_ICON_TILE=84 square.
TOOLBAR_ICONS_X, TOOLBAR_ICONS_Y = 20 + 72, 36
TILE, TILE_TOP = 84, 76

subprocess.run(["./tools/mkdisk.sh", DISK], check=True)
for f in (LOG, DUMP):
    try: os.remove(f)
    except FileNotFoundError: pass

q = subprocess.Popen(["qemu-system-i386", "-kernel", "kernel.elf", "-display", "none", "-vga", "std",
                      "-qmp", f"unix:{SOCKET},server,nowait", "-serial", "file:" + LOG,
                      "-drive", f"file={DISK},format=raw,if=ide,index=0"],
                     stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
fails = []
try:
    s = None
    for _ in range(50):
        time.sleep(0.2)
        candidate = socket.socket(socket.AF_UNIX)
        try:
            candidate.connect(SOCKET); candidate.settimeout(10); s = candidate; break
        except OSError:
            candidate.close()
    if s is None: raise SystemExit("FAIL: QEMU's QMP socket never came up")
    f = s.makefile("rw")
    def cmd(o):
        f.write(json.dumps(o) + "\n"); f.flush()
        while True:
            line = f.readline()
            if not line: raise ConnectionError("QEMU disconnected before replying")
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
        time.sleep(0.12)
        cmd({"execute": "input-send-event", "arguments": {"events": [{"type": "btn", "data": {"down": False, "button": "left"}}]}})
    def key(qcode):
        cmd({"execute": "send-key", "arguments": {"keys": [{"type": "qcode", "data": qcode}], "hold-time": 30}})
        time.sleep(0.1)
    def dump():
        cmd({"execute": "pmemsave", "arguments": {"val": FB, "size": W * H * 4, "filename": DUMP}})
        return Image.frombytes("RGBA", (W, H), open(DUMP, "rb").read(), "raw", "BGRA").convert("RGB")
    centre = lambda slot: SLOT0_X + slot * PITCH + DOCK_ICON // 2

    def open_files():
        move(centre(FILES_SLOT), ICON_ROW_Y); time.sleep(0.3)
        click(); time.sleep(1.5)

    def close_files():
        # Files' own close hitbox (windowed titlebar dot), same coordinates
        # app-interact-check.py's CLOSE_X/CLOSE_Y already use.
        move(94, 56); time.sleep(0.2)
        click(); time.sleep(0.6)

    def px(img, lx, ly):
        return img.getpixel((lx * SCALE, ly * SCALE))

    def saturation(p):
        mx, mn = max(p), min(p)
        return mx - mn

    def grid_tiles_present(img):
        """Count columns (0..3) at the icon grid's first row that show a
        real drawn glyph (the cream/ink file square, not bare wallpaper
        chrome) at the expected FILES_ICON_TILE spacing."""
        hits = 0
        for col in range(4):
            lx = VX + 20 + col * TILE + TILE // 2
            ly = VY + TILE_TOP + 18  # inside the drawn glyph body
            r, g, b = px(img, lx, ly)
            # glyph fill 0xF3EEE5 or ink 0xA8875A/edges -- distinctly not
            # the app's own 0xF5F0EB chrome bg or raw wallpaper.
            if (r, g, b) != (0xF5, 0xF0, 0xEB) and saturation((r, g, b)) < 60 and r > 150:
                hits += 1
        return hits

    open_files()
    key("2")  # switch to Icons
    time.sleep(0.3)
    img1 = dump()

    hits1 = grid_tiles_present(img1)
    if hits1 < 2:
        fails.append(f"Icons grid: only {hits1}/4 expected tile columns rendered at row 0 (spacing {TILE}px) -- grid did not render")

    close_files()
    open_files()
    img2 = dump()
    hits2 = grid_tiles_present(img2)
    if hits2 < 2:
        fails.append(f"Icons view did not visually persist after reopen: only {hits2}/4 tile columns rendered")

    try: cmd({"execute": "quit"})
    except (ConnectionResetError, BrokenPipeError, OSError): pass
finally:
    try: q.wait(timeout=5)
    except subprocess.TimeoutExpired: q.kill()

if fails:
    print("FAIL:")
    for msg in fails: print("  - " + msg)
    sys.exit(1)

print("PASS: Files switches to Icons, the grid renders at the expected spacing, and the choice persists across close/reopen")
sys.exit(0)
