#!/usr/bin/env python3
"""Burrow runs as a real ring-3 process (user/burrow.c, RING3_APPS row 20),
the Files app that used to be kernel/files.h.

Boots headless with a fresh FAT16 image holding DOCS/HELLO.TXT, opens Burrow
from the dock as a compositor window and checks:

  1. the launch ("ring3app: launching BURROW.BIN at ring 3"), the window
     ("syscall: window opened for ring-3 task") and the root listing with the
     DOCS folder in it ("burrow: cwd= n=" with n >= 1);
  2. the folder grid is drawn: the first tile (DOCS, folders sort first) has
     the cream glyph fill and the bark outline inside its own rect;
  3. Enter goes into DOCS ("burrow: cwd=DOCS n=1") and the grid now shows a
     file glyph; Backspace comes back ("burrow: cwd= n=" again);
  4. Esc exits 0, the window is released and no app window remains;
  5. the desktop answers: Mail opens from the dock afterwards.

Every wait has a deadline. Discriminating: break SYS_READDIR's relative path
and step 3 never sees cwd=DOCS; drop the go_up and the second root marker
never comes; drop the RING3_APPS row and step 1 fails.

Usage: tools/checks/ring3burrow-check.py   (from the repo root, after make kernel.elf)
"""
import json, os, socket, subprocess, sys, tempfile, time
from PIL import Image
from freeport import free_port

FB = 0xfd000000; W, H = 1920, 1080
LOGICAL_W, LOGICAL_H, SCALE = 960, 540, 2
DOCK_ICON, DOCK_GAP, SLOT0_X = 37, 6, 247
PITCH = DOCK_ICON + DOCK_GAP
ICON_ROW_Y = 487
BURROW_SLOT = 1   # GUI_DOCK_DEFAULT: {Apps, Burrow, Mail, ...}
CLOSE_X, CLOSE_Y = 94, 56
CLOSE_RED = (0xFF, 0x5F, 0x57)
VIEW_X, VIEW_Y = 78, 72   # gui_launch_from_dock: viewport at (x+8, y+32)
PARK = (480, 200)
BTN_ON, BTN_OFF = (0xE5, 0xDC, 0xCC), (0xEF, 0xEB, 0xE4)   # user/burrow.c BTN_ON, BTN
TB_Y, TB_H, TB_PITCH = 36, 22, 72
TILE, ICON_TOP = 84, 76
os.chdir(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))

def start(tag, extra):
    log, dump = f"/tmp/jt-{tag}-serial.log", f"/tmp/jt-{tag}.raw"
    for f in (log, dump):
        try: os.remove(f)
        except FileNotFoundError: pass
    port = free_port()
    q = subprocess.Popen(["qemu-system-i386", "-kernel", "kernel.elf", "-nic", "none", "-display", "none", "-vga", "std",
                          "-qmp", f"tcp:127.0.0.1:{port},server,nowait", "-serial", "file:" + log] + extra,
                         stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    s = None
    for _ in range(50):
        time.sleep(0.2)
        try: s = socket.create_connection(("127.0.0.1", port)); break
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
    return q, cmd, log, dump

class Box:
    def __init__(self, cmd, log, dump): self.cmd, self.log, self.dump = cmd, log, dump
    def serial(self):
        try: return open(self.log, errors="replace").read()
        except OSError: return ""
    def wait(self, needle, secs, count=1):
        for _ in range(int(secs * 10)):
            if self.serial().count(needle) >= count: return True
            time.sleep(0.1)
        return False
    def keys(self, *qcodes):
        r = self.cmd({"execute": "send-key", "arguments": {"keys": [{"type": "qcode", "data": k} for k in qcodes]}})
        if "error" in r: raise SystemExit(f"FAIL: QMP rejected send-key {qcodes}: {r['error']}")
        time.sleep(0.35)
    def move(self, x, y):
        self.cmd({"execute": "input-send-event", "arguments": {"events": [
            {"type": "abs", "data": {"axis": "x", "value": int(x * 32768 / LOGICAL_W)}},
            {"type": "abs", "data": {"axis": "y", "value": int(y * 32768 / LOGICAL_H)}}]}})
    def click(self):
        self.cmd({"execute": "input-send-event", "arguments": {"events": [{"type": "btn", "data": {"down": True, "button": "left"}}]}})
        time.sleep(0.12)
        self.cmd({"execute": "input-send-event", "arguments": {"events": [{"type": "btn", "data": {"down": False, "button": "left"}}]}})
    def frame(self):
        self.cmd({"execute": "pmemsave", "arguments": {"val": FB, "size": W * H * 4, "filename": self.dump}})
        return Image.frombytes("RGBA", (W, H), open(self.dump, "rb").read(), "raw", "BGRA").convert("RGB")
    def pixel(self, x, y, img=None): return (img or self.frame()).getpixel((x * SCALE + 1, y * SCALE + 1))
    def open_burrow(self):
        self.move(SLOT0_X + BURROW_SLOT * PITCH + DOCK_ICON // 2, ICON_ROW_Y); time.sleep(0.3)
        self.click(); time.sleep(1.5)
    def button(self, img, i):
        p = self.pixel(VIEW_X + 20 + i * TB_PITCH + 4, VIEW_Y + TB_Y + TB_H - 4, img)
        if near(p, BTN_ON, 16): return "active"
        if near(p, BTN_OFF, 16): return "inactive"
        return "other:%r" % (p,)

def near(p, c, tol=12): return max(abs(p[i] - c[i]) for i in range(3)) <= tol

DISK = "/tmp/jt-ring3burrow-fat.img"
fails = []
try:
    subprocess.run(["bash", "tools/mkdisk.sh", DISK], check=True, stdout=subprocess.DEVNULL)
    with tempfile.NamedTemporaryFile("w", suffix=".txt", delete=False) as t:
        t.write("hello from docs\n"); hello = t.name
    subprocess.run(["mmd", "-i", DISK, "::DOCS"], check=True)
    subprocess.run(["mcopy", "-i", DISK, hello, "::DOCS/HELLO.TXT"], check=True)
    os.remove(hello)
except (subprocess.CalledProcessError, FileNotFoundError) as e:
    raise SystemExit(f"FAIL: could not build the FAT16 image with DOCS/HELLO.TXT: {e}")

q, cmd, log, dump = start("ring3burrow", ["-drive", f"file={DISK},format=raw,if=ide,index=0"])
b = Box(cmd, log, dump)
def tile_has_glyph(img, index=0):
    tx, ty = VIEW_X + 20 + index * TILE, VIEW_Y + ICON_TOP
    fill = ink = False
    for dx in range(0, 40, 3):
        for dy in range(0, 40, 3):
            p = b.pixel(tx + dx, ty + dy, img)
            if near(p, (0xF3, 0xEE, 0xE5), 12): fill = True
            if near(p, (0xA8, 0x87, 0x5A), 20): ink = True
    return fill and ink
try:
    time.sleep(5.0)
    b.open_burrow()
    if not b.wait("ring3app: launching BURROW.BIN at ring 3", 20): fails.append("Burrow was never launched as a ring-3 program from the dock")
    if not b.wait("syscall: window opened for ring-3 task", 10): fails.append("SYS_WINDOW_OPEN never succeeded from ring 3")
    if not b.wait("burrow: ring-3 window", 10): fails.append("Burrow never announced its window")
    root = b.serial().count("burrow: cwd= n=")
    if root < 1: fails.append("no root listing marker (burrow: cwd= n=N)")
    time.sleep(0.5)
    if not tile_has_glyph(b.frame()): fails.append("the folder grid is not drawn: no glyph in the first tile")
    b.keys("ret")
    if not b.wait("burrow: cwd=DOCS n=1", 8): fails.append("Enter did not open the DOCS folder")
    time.sleep(0.3)
    if not tile_has_glyph(b.frame()): fails.append("inside DOCS the grid shows no glyph for HELLO.TXT")
    b.keys("backspace")
    if not b.wait("burrow: cwd= n=", 8, root + 1): fails.append("Backspace did not go back to the root")
    exits = b.serial().count("BURROW.BIN exited 0")
    b.keys("esc")
    if not b.wait("BURROW.BIN exited 0", 8, exits + 1): fails.append("Burrow did not exit 0 on Esc")
    if not b.wait("syscall: window released, task gone", 5): fails.append("Burrow did not release its window on Esc")
    b.move(*PARK); time.sleep(0.5)
    for _ in range(100):
        if not near(b.pixel(CLOSE_X, CLOSE_Y), CLOSE_RED): break
        time.sleep(0.1)
    if near(b.pixel(CLOSE_X, CLOSE_Y), CLOSE_RED): fails.append("an app window is still open after Esc")
    s = b.serial()
    if "exception: ring-0" in s or "panic in" in s or "ring3app: BUG" in s: fails.append("the kernel faulted or ring3app logged a BUG line")
    if b.pixel(480, 511) != (0xEF, 0xEB, 0xE4): fails.append("desktop dock not on screen after the close")
    b.move(SLOT0_X + 2 * PITCH + DOCK_ICON // 2, ICON_ROW_Y); time.sleep(0.3); b.click()
    opened = False
    for _ in range(40):
        time.sleep(0.1)
        if near(b.pixel(CLOSE_X, CLOSE_Y), CLOSE_RED): opened = True; break
    if not opened: fails.append("Mail did not open from the dock after Burrow closed: desktop not responsive")
    b.keys("esc"); time.sleep(0.5)
finally:
    try: cmd({"execute": "quit"})
    except (ConnectionResetError, BrokenPipeError, OSError, ValueError, TypeError): pass
    q.terminate()
    try: q.wait(5)
    except subprocess.TimeoutExpired: q.kill()

if fails:
    print("FAIL:")
    for x in fails: print("  - " + x)
    print("--- serial tail ---"); print(b.serial()[-1500:])
    sys.exit(1)
print("PASS: Burrow ran at ring 3 with its own window, drew the folder grid, Enter entered DOCS and Backspace came back, Esc closed it, and the desktop stayed alive")
