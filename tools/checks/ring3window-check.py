#!/usr/bin/env python3
"""1.9.23: a ring-3 program as a real compositor window, beside Notes.

Boots headless with open=remi, so Reminders (the first ring-3 app on the
window path, kernel/ring3app.c ring3app_launch_window) opens as window 0
without blocking the desktop loop. Then, from the real framebuffer and the
serial log, it proves the 2.0 gate's multi-window and input-routing items
for a ring-3 program:

  1. Reminders' window chrome is on screen (window 0's red close dot) and
     the program drew into its own private framebuffer ("syscall: window
     opened for ring-3 task"), which the compositor blitted at the window's
     position.
  2. A dock click opens Notes BESIDE it: window 1's close dot appears while
     window 0's is still there. Two apps on screen at once, one of them a
     ring-3 task scheduled next to the desktop loop.
  3. Input goes to the focused window only: with Notes on top, the backquote
     (Reminders' deliberate crash key) must NOT reach Reminders. After a
     click inside Reminders' window (click-to-focus) the same key does, and
     the program announces "reminders: crashing on purpose".
  4. The crash reaps only that task: the kernel logs the reap, the window
     release and the launcher's "crashed (page-fault), window torn down,
     desktop alive" line; Notes' window is still on screen afterwards and
     the dock tray is drawn. No ring-0 exception, no panic, no BUG line.

Discriminating: route keys through the old global gui_poll_event pull and
step 3's first press crashes Reminders while Notes is focused; make the
reaper close every window and step 4 loses Notes.

Usage: tools/checks/ring3window-check.py   (from the repo root, after make kernel.elf)
"""
import json, os, socket, subprocess, sys, time
from PIL import Image
from freeport import free_port

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..")
os.chdir(ROOT)
LOG = "/tmp/jt-ring3window-serial.log"
DUMP = "/tmp/jt-ring3window.raw"
FB = 0xfd000000; W, H = 1920, 1080
LOGICAL_W, LOGICAL_H, SCALE = 960, 540, 2
DOCK_ICON, DOCK_GAP, SLOT0_X = 37, 6, 247
PITCH = DOCK_ICON + DOCK_GAP
ICON_ROW_Y = 487
W0_CLOSE = (94, 56)     # window 0: gui_multiwin_geom slot 0, x=70,y=40
W1_CLOSE = (154, 116)   # window 1: x=130,y=100
W0_CONTENT = (90, 300)  # inside Reminders' window, outside Notes' rect (Notes starts at y=100, x=130)
CLOSE_RED = (0xFF, 0x5F, 0x57)
DOCK_TRAY = (0xEF, 0xEB, 0xE4)
PARK = (480, 200)
NOTES_SLOT = 4

for f in (LOG, DUMP):
    try: os.remove(f)
    except FileNotFoundError: pass
port = free_port()
q = subprocess.Popen(["qemu-system-i386", "-kernel", "kernel.elf", "-append", "open=remi",
                      "-display", "none", "-vga", "std",
                      "-qmp", f"tcp:127.0.0.1:{port},server,nowait", "-serial", "file:" + LOG],
                     stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
fails = []

def serial():
    try: return open(LOG, errors="replace").read()
    except OSError: return ""
def wait_serial(needle, secs):
    for _ in range(int(secs * 10)):
        if needle in serial(): return True
        time.sleep(0.1)
    return False

try:
    s = None
    for _ in range(50):
        time.sleep(0.2)
        try: s = socket.create_connection(("127.0.0.1", port)); break
        except OSError: pass
    if s is None: sys.exit("FAIL: QEMU's QMP socket never came up")
    f = s.makefile("rw")
    def cmd(o):
        f.write(json.dumps(o) + "\n"); f.flush()
        while True:
            r = json.loads(f.readline())
            if "return" in r or "error" in r: return r
    f.readline()
    cmd({"execute": "qmp_capabilities"})
    def press(k):
        cmd({"execute": "send-key", "arguments": {"keys": [{"type": "qcode", "data": k}]}}); time.sleep(0.3)
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
        return img.getpixel((x * SCALE + 1, y * SCALE + 1))
    def near(p, c, tol=12): return max(abs(p[i] - c[i]) for i in range(3)) <= tol
    def wait_pixel(xy, c, want=True, secs=5):
        for _ in range(int(secs * 10)):
            if near(pixel(*xy), c) == want: return True
            time.sleep(0.1)
        return False

    # 1. Reminders opens as a window, without blocking the desktop
    if not wait_serial("ring3app: launching REMINDERS.BIN at ring 3 as a window", 40): sys.exit("FAIL: Reminders was not launched on the window path")
    if not wait_serial("syscall: window opened for ring-3 task", 15): fails.append("the program never opened its window")
    if not wait_serial("autoopen: back on the desktop", 15): fails.append("the launch blocked the desktop loop")
    move(*PARK); time.sleep(0.5)
    if not wait_pixel(W0_CLOSE, CLOSE_RED): fails.append("Reminders' window chrome is not on screen")

    # 2. Notes opens beside it
    move(SLOT0_X + NOTES_SLOT * PITCH + DOCK_ICON // 2, ICON_ROW_Y); time.sleep(0.3); click(); time.sleep(0.5)
    move(*PARK); time.sleep(0.5)
    if not wait_pixel(W1_CLOSE, CLOSE_RED): fails.append("Notes did not open as a second window beside Reminders")
    if not near(pixel(*W0_CLOSE), CLOSE_RED): fails.append("Reminders' window vanished when Notes opened")

    # 3. keys reach the focused window only. Notes is a ring-3 task too and
    # has its own backquote crash key, so with Notes on top the first press
    # must kill NOTES and leave Reminders untouched.
    press("grave_accent")
    if not wait_serial("notes: crashing on purpose", 5): fails.append("the key did not reach Notes, the focused ring-3 window")
    if not wait_serial("ring3app: NOTES.BIN crashed (page-fault), window torn down, desktop alive", 8): fails.append("Notes' crash was not reaped by name")
    if "reminders: crashing on purpose" in serial(): fails.append("a key reached Reminders while Notes was focused (global key pull)")
    if not wait_pixel(W1_CLOSE, CLOSE_RED, want=False): fails.append("Notes' window is still on screen after its crash")
    if not near(pixel(*W0_CLOSE), CLOSE_RED): fails.append("Reminders' window vanished when Notes crashed")
    # reopen Notes on top, then click inside Reminders (click-to-focus, swallowed
    # by the compositor) and the same key must now crash Reminders, not Notes
    move(SLOT0_X + NOTES_SLOT * PITCH + DOCK_ICON // 2, ICON_ROW_Y); time.sleep(0.3); click(); time.sleep(0.5)
    move(*PARK); time.sleep(0.5)
    if not wait_pixel(W1_CLOSE, CLOSE_RED): fails.append("Notes did not reopen beside Reminders")
    n_notes = serial().count("notes: crashing on purpose")
    move(*W0_CONTENT); time.sleep(0.3); click(); time.sleep(0.5)
    move(*PARK); time.sleep(0.3)
    for _ in range(4):
        press("grave_accent")
        if wait_serial("reminders: crashing on purpose", 2): break
    else: fails.append("the crash key never reached Reminders after click-to-focus")
    if serial().count("notes: crashing on purpose") != n_notes: fails.append("the key reached Notes after Reminders was click-focused")

    # 4. only that window dies
    if not wait_serial("exception: ring-3 task hit page-fault, reaped", 8): fails.append("the task was not reaped")
    if not wait_serial("syscall: window released, task gone", 8): fails.append("the window buffers were not released")
    if not wait_serial("ring3app: REMINDERS.BIN crashed (page-fault), window torn down, desktop alive", 8): fails.append("the compositor did not log the crash by name")
    if not wait_pixel(W0_CLOSE, CLOSE_RED, want=False): fails.append("the dead program's window is still on screen")
    if not wait_pixel(W1_CLOSE, CLOSE_RED): fails.append("Notes' window did not survive the ring-3 crash")
    if not near(pixel(480, 511), DOCK_TRAY): fails.append(f"the dock tray is not drawn after the crash (got {pixel(480, 511)})")
    log = serial()
    if "exception: ring-0" in log or "panic in" in log or "ring3app: BUG" in log: fails.append("the KERNEL faulted or ring3app logged a BUG line")
finally:
    q.kill()

if fails:
    for x in fails: print("FAIL:", x)
    sys.exit(1)
print("PASS: Reminders ran as a ring-3 compositor window beside Notes, keys went to the focused window only, its crash closed only its window")
