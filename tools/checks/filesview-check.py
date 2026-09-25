#!/usr/bin/env python3
"""Regression test for Files' view switcher (roadmap.md: "Files: view
buttons. Right now it's just a list."). Boots with NO disk attached, the
same no-disk condition ramfs-demo-check.sh already relies on, so kmain's
own ramfs fallback seeds two real files (README.TXT, NOTES.TXT) -- a
blank formatted-but-empty FAT disk (mkdisk.sh's own default) makes Files
show "(no files, or no FAT filesystem)" and every "grid rendered" check
against that screen is trivially, silently meaningless, which is exactly
how the first version of this check shipped green while its own attached
screenshot showed the empty-list message with List still highlighted.

Opens Files headlessly, switches to Icons with the '2' key, dumps the
framebuffer and asserts:
  - the Icons toolbar button is the highlighted one, List is not
  - each of the two real seeded files (README.TXT, NOTES.TXT) has a real
    drawn glyph (fill + ink edges) inside its own tile rect, at the exact
    FILES_ICON_TILE pitch gui_draw_files_content lays tiles out at
  - no third tile beyond those two (nothing phantom)
then closes the app and reopens it, re-asserting the same three things
with no further input -- proving files_view survived the round trip
through SETTINGS.TXT, not just stayed live in RAM for one session.

Same QMP absolute-pointer + qcode-keyboard + pmemsave shape as
tools/checks/app-interact-check.py.

Usage: tools/checks/filesview-check.py   (from repo root, after make kernel.elf)
"""
import json, os, socket, subprocess, sys, tempfile, time
from PIL import Image

REPO = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..")
os.chdir(REPO)
ARTIFACTS = tempfile.mkdtemp(prefix="jt-filesview-")
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
# gui_draw_files_content: toolbar buttons at local (20+i*72, 36), sized
# FILES_TOOLBAR_BTN_W x FILES_TOOLBAR_BTN_H (64x22); grid tiles start at
# local (20, 76), each FILES_ICON_TILE=84 square, glyph FILES_ICON_SIZE=40.
BTN_X0, BTN_Y, BTN_W, BTN_H = 20, 36, 64, 22
BTN_GAP = 72
TILE, TILE_TOP, ICON_SIZE = 84, 76, 40
ICON_ACTIVE = (0xE5, 0xDC, 0xCC)
ICON_INACTIVE = (0xEF, 0xEB, 0xE4)

for f in (LOG, DUMP):
    try: os.remove(f)
    except FileNotFoundError: pass

# No -drive: the exact no-disk boot path ramfs-demo-check.sh proves seeds
# README.TXT and NOTES.TXT into ramfs, real files Files can actually list.
q = subprocess.Popen(["qemu-system-i386", "-kernel", "kernel.elf", "-display", "none", "-vga", "std",
                      "-qmp", f"unix:{SOCKET},server,nowait", "-serial", "file:" + LOG],
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
        cmd({"execute": "send-key", "arguments": {"keys": [{"type": "qcode", "data": qcode}]}})
        time.sleep(0.35)
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
        p = img.getpixel((lx * SCALE, ly * SCALE))
        return p[:3] if len(p) > 3 else p

    def close(a, b, tol=10):
        return all(abs(a[i] - b[i]) <= tol for i in range(3))

    def button_state(img, idx):
        bx = VX + BTN_X0 + idx * BTN_GAP
        by = VY + BTN_Y
        # Sample a corner of the button rect, clear of the label glyph
        # (which sits centred and would pollute a centre-point sample).
        p = px(img, bx + 4, by + BTN_H - 4)
        if close(p, ICON_ACTIVE, 16): return "active"
        if close(p, ICON_INACTIVE, 16): return "inactive"
        return "other:%r" % (p,)

    def tile_has_glyph(img, index):
        """A real tile has both the cream glyph fill (0xF3EEE5-ish) and the
        warm ink edge (0xA8875A-ish) somewhere inside its own rect -- either
        alone could be a stray background pixel, but both together at the
        right pitch is the actual drawn folder/file glyph, not noise."""
        cols = (804 - 40) // TILE  # app_view_w (820-16) minus the 20px left margin
        if cols < 1: cols = 1
        col, row = index % cols, index // cols
        tx = VX + 20 + col * TILE
        ty = VY + TILE_TOP + row * TILE
        fill_hit = ink_hit = False
        for dx in range(0, ICON_SIZE, 3):
            for dy in range(0, ICON_SIZE, 3):
                p = px(img, tx + dx, ty + dy)
                if close(p, (0xF3, 0xEE, 0xE5), 12): fill_hit = True
                if close(p, (0xA8, 0x87, 0x5A), 20): ink_hit = True
        return fill_hit and ink_hit

    def no_extra_tile(img, index):
        """A tile slot well past the real seeded files should show neither
        fill nor ink -- proves the grid isn't drawing phantom entries.
        Index 2 is deliberately NOT used here: switching views persists
        files_view through settings_save(), which real-writes
        SETTINGS.TXT onto the same ramfs Files is listing, so a genuine
        3rd tile (SETTINGS.TXT) is expected once a switch has happened;
        checking one slot further out (index 3) still catches a runaway/
        phantom grid without racing that real, load-bearing side effect."""
        return not tile_has_glyph(img, index)

    def check_icons_screen(img, label):
        st_list = button_state(img, 0)
        st_icons = button_state(img, 1)
        if st_icons != "active":
            fails.append(f"{label}: Icons toolbar button is not highlighted (state={st_icons})")
        if st_list != "inactive":
            fails.append(f"{label}: List toolbar button is still highlighted (state={st_list})")
        if not tile_has_glyph(img, 0):
            fails.append(f"{label}: no glyph rendered in README.TXT's tile (index 0)")
        if not tile_has_glyph(img, 1):
            fails.append(f"{label}: no glyph rendered in NOTES.TXT's tile (index 1)")
        if not no_extra_tile(img, 3):
            fails.append(f"{label}: a phantom tile rendered well past the real seeded files")

    open_files()
    img0 = dump()
    if button_state(img0, 0) != "active":
        fails.append("Files opened with List not the default active view (expected before switching)")

    key("2")  # switch to Icons
    time.sleep(0.3)
    img1 = dump()
    check_icons_screen(img1, "after pressing 2")

    close_files()
    open_files()
    img2 = dump()
    check_icons_screen(img2, "after close/reopen (persistence)")

    try: cmd({"execute": "quit"})
    except (ConnectionResetError, BrokenPipeError, OSError): pass
finally:
    try: q.wait(timeout=5)
    except subprocess.TimeoutExpired: q.kill()

if fails:
    print("FAIL:")
    for msg in fails: print("  - " + msg)
    sys.exit(1)

print("PASS: Files switches to Icons, both seeded files render real glyph tiles at the expected pitch with Icons highlighted, and it all survives close/reopen")
sys.exit(0)
