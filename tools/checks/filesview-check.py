#!/usr/bin/env python3
"""Burrow's List / Icons choice persists (it used to live in the kernel's
files_view and SETTINGS.TXT; Burrow is ring 3 now and keeps it in
BURROW.TXT through the ordinary file syscalls).

Boots headless with no disk (ramfs), opens Burrow from the dock and:
  1. asserts the first run starts on Icons ("burrow: view=1", Icons button
     highlighted, List not) because no BURROW.TXT exists yet;
  2. presses 1: "burrow: saved view=0" and the List button is the
     highlighted one;
  3. closes it with Esc and opens it again from the dock: the fresh process
     reports "burrow: view=0" and List is still highlighted, so the choice
     went through the file and not through RAM;
  4. presses 2: "burrow: saved view=1", Icons highlighted again.
Also asserts the kernel no longer writes a "filesview" key into SETTINGS.TXT
(kernel/kernel.c).

Usage: tools/checks/filesview-check.py   (from repo root, after make kernel.elf)
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

fails = []
q, cmd, log, dump = start("filesview", [])
b = Box(cmd, log, dump)
try:
    time.sleep(5.0)
    def esc_and_wait(n):
        b.keys("esc")
        if not b.wait("burrow: closed", 8, n): fails.append("Burrow did not close on Esc")
        time.sleep(0.8)
    b.open_burrow()
    if not b.wait("burrow: view=1", 15): fails.append("first run did not start on Icons (burrow: view=1)")
    img = b.frame()
    if (b.button(img, 0), b.button(img, 1)) != ("inactive", "active"):
        fails.append(f"first run: toolbar is {b.button(img, 0)}/{b.button(img, 1)}, want List inactive, Icons active")
    b.keys("1")
    if not b.wait("burrow: saved view=0", 8): fails.append("pressing 1 did not save view=0 to BURROW.TXT")
    time.sleep(0.3); img = b.frame()
    if (b.button(img, 0), b.button(img, 1)) != ("active", "inactive"):
        fails.append(f"after 1: toolbar is {b.button(img, 0)}/{b.button(img, 1)}, want List active")
    esc_and_wait(1)
    b.open_burrow()
    if not b.wait("burrow: view=0", 15): fails.append("the reopened Burrow did not read view=0 back from BURROW.TXT")
    img = b.frame()
    if b.button(img, 0) != "active": fails.append(f"after reopen: List is {b.button(img, 0)}, want active (persistence)")
    b.keys("2")
    if not b.wait("burrow: saved view=1", 8): fails.append("pressing 2 did not save view=1")
    time.sleep(0.3); img = b.frame()
    if b.button(img, 1) != "active": fails.append("after 2: Icons is not active")
    if "exception: ring-0" in b.serial() or "panic in" in b.serial(): fails.append("the kernel faulted")
    b.keys("esc")
finally:
    try: cmd({"execute": "quit"})
    except (ConnectionResetError, BrokenPipeError, OSError, ValueError, TypeError): pass
    q.terminate()
    try: q.wait(5)
    except subprocess.TimeoutExpired: q.kill()

if "filesview" in open("kernel/kernel.c").read():
    fails.append("kernel/kernel.c still mentions filesview: the kernel should not own the view choice")
if fails:
    print("FAIL:")
    for x in fails: print("  - " + x)
    print("--- serial tail ---"); print(b.serial()[-1200:])
    sys.exit(1)
print("PASS: Burrow starts on Icons, 1 saves List to BURROW.TXT, a fresh run reads it back, 2 saves Icons, and the kernel no longer owns the choice")
