#!/usr/bin/env python3
"""Clock runs as a real ring-3 process, the tenth app out of the kernel
(roadmap 2.0, 1.9.4).

Boots headless with `open=clock`, which launches Clock from the dock path the
moment the desktop is up. Clock is user/clock.c, a flat binary loaded off the
VFS by exec_user and run at CPL 3 through the table-driven launcher
(kernel/ring3app.c, RING3_APPS). It reads the time through SYS_TIME. The
check then:

  1. asserts, off the serial log, that the program opened a window of the
     dock viewport's size (804x345);
  2. grabs the big time digits twice, a little over two seconds apart, and
     asserts they changed: the clock is drawing the real, moving time;
  3. presses space, types 1, presses Enter: asserts the program's own
     "clock: timer 60" line, so the input and timer logic ran;
  4. presses Esc: asserts it exited 0, released its window, and the desktop
     is back (Mail opens from a dock click).

Every wait has a deadline. Discriminating: freeze the time in user/clock.c
and step 2 fails; drop the Enter handling and step 3 fails.

Usage: tools/checks/ring3clock-check.py   (from the repo root, after make kernel.elf)
"""
import json, os, socket, subprocess, sys, time
from PIL import Image
from freeport import free_port

LOG = "/tmp/jt-ring3clock-serial.log"
DUMP = "/tmp/jt-ring3clock.raw"
FB = 0xfd000000; W, H = 1920, 1080
PORT = free_port()
LOGICAL_W, LOGICAL_H, SCALE = 960, 540, 2
DOCK_ICON, DOCK_GAP, SLOT0_X = 37, 6, 247
PITCH = DOCK_ICON + DOCK_GAP
ICON_ROW_Y = 487
CLOSE_X, CLOSE_Y = 94, 56
CLOSE_RED = (0xFF, 0x5F, 0x57)
VIEW_X, VIEW_Y = 78, 72   # gui_launch_from_dock: viewport at (x+8, y+32) for x=70, y=40
PARK = (480, 200)
TIME_X, TIME_Y, TIME_W, TIME_H = 46, 32, 280, 280  # user/clock.c: the analog face (FACE_CX/CY/R), its hands move every second

os.chdir(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
for f in (LOG, DUMP):
    try: os.remove(f)
    except FileNotFoundError: pass

q = subprocess.Popen(["qemu-system-i386", "-kernel", "kernel.elf", "-append", "open=clock",
                      "-display", "none", "-vga", "std",
                      "-qmp", f"tcp:127.0.0.1:{PORT},server,nowait", "-serial", "file:" + LOG],
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
    def keys(*qcodes):
        r = cmd({"execute": "send-key", "arguments": {"keys": [{"type": "qcode", "data": k} for k in qcodes]}})
        if "error" in r: raise SystemExit(f"FAIL: QMP rejected send-key {qcodes}: {r['error']}")
    def move(x, y):
        cmd({"execute": "input-send-event", "arguments": {"events": [
            {"type": "abs", "data": {"axis": "x", "value": int(x * 32768 / LOGICAL_W)}},
            {"type": "abs", "data": {"axis": "y", "value": int(y * 32768 / LOGICAL_H)}}]}})
    def click():
        cmd({"execute": "input-send-event", "arguments": {"events": [{"type": "btn", "data": {"down": True, "button": "left"}}]}})
        time.sleep(0.1)
        cmd({"execute": "input-send-event", "arguments": {"events": [{"type": "btn", "data": {"down": False, "button": "left"}}]}})
    def frame():
        cmd({"execute": "pmemsave", "arguments": {"val": FB, "size": W * H * 4, "filename": DUMP}})
        return Image.frombytes("RGBA", (W, H), open(DUMP, "rb").read(), "raw", "BGRA").convert("RGB")
    def pixel(x, y, img=None):
        return (img or frame()).getpixel((x * SCALE + 1, y * SCALE + 1))
    def near(p, c, tol=12): return max(abs(p[i] - c[i]) for i in range(3)) <= tol
    def digits():
        x0, y0 = (VIEW_X + TIME_X) * SCALE, (VIEW_Y + TIME_Y) * SCALE
        return frame().crop((x0, y0, x0 + TIME_W * SCALE, y0 + TIME_H * SCALE)).tobytes()

    # 1. the program is up and has its window
    if not wait_serial("ring3app: launching CLOCK.BIN at ring 3", 40):
        fails.append("Clock was never launched as a ring-3 program (open=clock flag or ring3app.c broken)")
    if not wait_serial("syscall: window opened for ring-3 task", 10):
        fails.append("SYS_WINDOW_OPEN never succeeded from ring 3")
    if not wait_serial("clock: ring-3 window 804x345", 10):
        fails.append("the program did not report the app viewport's size (expected 804x345) through write()")
    time.sleep(0.5)

    # 2. the digits are ink on the page color, and they move with the real clock
    a = digits()
    if os.environ.get("JT_SHOT"): frame().save(os.environ["JT_SHOT"])
    ink = sum(1 for i in range(0, len(a), 3) if a[i] < 0x60 and a[i + 1] < 0x60 and a[i + 2] < 0x60)
    print(f"time digits: {ink} ink pixels")
    if ink < 200:
        fails.append(f"the time digits were not drawn (only {ink} dark pixels in the digit box)")
    changed = False
    for _ in range(12):
        time.sleep(0.5)
        if digits() != a: changed = True; break
    if not changed:
        fails.append("the time digits did not change in 6s: the clock is not reading the moving time")

    # 3. space, 1, Enter starts a one-minute timer through the real input logic
    keys("spc"); time.sleep(0.3)
    keys("1"); time.sleep(0.3)
    keys("ret")
    if not wait_serial("clock: timer 60", 5):
        fails.append('space, 1, Enter did not log "clock: timer 60"')
    if "clock: crashing" in serial():
        fails.append("the program crashed before the crash key was pressed")

    # 4. Esc closes it cleanly and the desktop comes back
    exits = serial().count("CLOCK.BIN exited 0")
    keys("esc")
    if not wait_serial("clock: closed", 5):
        fails.append("Esc did not reach the program (no closed line)")
    if not wait_serial("syscall: window released, task gone", 5) or serial().count("CLOCK.BIN exited 0") <= exits:
        fails.append("Clock did not exit 0 and release its window on Esc")
    if "exception: ring-0" in serial() or "panic in" in serial() or "ring3app: BUG" in serial():
        fails.append("the kernel faulted or ring3app logged a BUG line")
    move(*PARK); time.sleep(0.5)
    for _ in range(100):
        if not near(pixel(CLOSE_X, CLOSE_Y), CLOSE_RED): break
        time.sleep(0.1)
    if near(pixel(CLOSE_X, CLOSE_Y), CLOSE_RED):
        fails.append("an app window is still open after Esc")
    dock = pixel(480, 511)
    if dock != (0xEF, 0xEB, 0xE4):
        fails.append(f"desktop dock not on screen after the close (got {dock})")
    move(SLOT0_X + 2 * PITCH + DOCK_ICON // 2, ICON_ROW_Y); time.sleep(0.3); click()
    opened = False
    for _ in range(40):
        time.sleep(0.1)
        if near(pixel(CLOSE_X, CLOSE_Y), CLOSE_RED): opened = True; break
    print(f"Mail opens from the dock after the close: {'yes' if opened else 'NO'}")
    if not opened: fails.append("Mail did not open from a dock click after Clock closed: desktop not responsive")
    keys("esc"); time.sleep(0.5)
finally:
    try: cmd({"execute": "quit"})
    except (ConnectionResetError, BrokenPipeError, OSError, ValueError, NameError): pass
    q.terminate()
    try: q.wait(5)
    except subprocess.TimeoutExpired: q.kill()

if fails:
    print("FAIL:")
    for x in fails: print("  - " + x)
    print("--- serial tail ---")
    print(serial()[-1500:])
    sys.exit(1)
print("PASS: Clock ran at ring 3 with its own window, drew the moving time, took a timer through the real input logic, closed on Esc, and the desktop stayed alive")
