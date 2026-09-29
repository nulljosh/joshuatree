#!/usr/bin/env python3
"""Activity runs as a real ring-3 process, the twelfth app out of the kernel
(roadmap 2.0, 1.9.6).

Boots headless with `open=activ`, which launches Activity from the dock path
the moment the desktop is up. Activity is user/activity.c, a flat binary
loaded off the VFS by exec_user and run at CPL 3 through the table-driven
launcher (kernel/ring3app.c, RING3_APPS). It reads the scheduler and memory
through the one new call, SYS_TASKS. The check then:

  1. asserts, off the serial log, that the program opened a window of the
     dock viewport's size (804x345) and drew once ("activitycontent");
  2. reads the task rows' pixels: slot 0 is highlighted, slot 1 is plain;
  3. waits and asserts the redraw marker keeps growing with no input, the
     live refresh, driven by SYS_TASKS ticks;
  4. presses Down: asserts "activity: sel 1" and that the highlight moved;
  5. clicks slot 0 and presses k: the kernel must refuse to kill the shell
     (kill result 1, EPERM); selects a free slot and presses k: kill result
     2 (ENOENT). Both come back through the real syscall, not just the draw;
  6. presses Esc: asserts it exited 0, released its window, and the desktop
     is back (Mail opens from a dock click).

Every wait has a deadline. Discriminating: make SYS_TASKS skip the slot 0
guard and step 5 fails; make Esc a no-op and step 6 fails.

Usage: tools/checks/ring3activity-check.py   (from the repo root, after make kernel.elf)
"""
import json, os, socket, subprocess, sys, time
from PIL import Image
from freeport import free_port

LOG = "/tmp/jt-ring3activity-serial.log"
DUMP = "/tmp/jt-ring3activity.raw"
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
ROW_COLOR = (0xFA, 0xF8, 0xF6)
SEL_COLOR = (0xED, 0xE6, 0xDC)
TOP, ROW_H = 82, 22   # user/activity.c: first row and row pitch
PROBE_X = 700               # right of the text, inside the highlight bar

os.chdir(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
for f in (LOG, DUMP):
    try: os.remove(f)
    except FileNotFoundError: pass

q = subprocess.Popen(["qemu-system-i386", "-kernel", "kernel.elf", "-append", "open=activ",
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
    def row_pixel(i, img=None):
        return pixel(VIEW_X + PROBE_X, VIEW_Y + TOP + i * ROW_H + 6, img)

    # 1. the program is up, has its window and has drawn
    if not wait_serial("ring3app: launching ACTIVITY.BIN at ring 3", 40):
        fails.append("Activity was never launched as a ring-3 program (open=activ flag or ring3app.c broken)")
    if not wait_serial("syscall: window opened for ring-3 task", 10):
        fails.append("SYS_WINDOW_OPEN never succeeded from ring 3")
    if not wait_serial("activity: ring-3 window 804x345", 10):
        fails.append("the program did not report the app viewport's size (expected 804x345) through write()")
    if not wait_serial("activitycontent", 10):
        fails.append("no 'activitycontent' marker: the first draw never ran")
    time.sleep(0.5)

    # 2. slot 0 is the selected row, slot 1 is plain
    r0, r1 = row_pixel(0), row_pixel(1)
    print(f"row 0 (selected) {r0}, row 1 {r1}")
    if not near(r0, SEL_COLOR):
        fails.append(f"slot 0 did not draw the selected color (got {r0}, expected {SEL_COLOR})")
    if not near(r1, ROW_COLOR):
        fails.append(f"slot 1 did not draw the plain color (got {r1}, expected {ROW_COLOR})")

    # 3. the window refreshes on its own, no key pressed
    n0 = serial().count("activitycontent")
    grew = False
    for _ in range(60):
        time.sleep(0.1)
        if serial().count("activitycontent") >= n0 + 2: grew = True; break
    print(f"live refresh with no input: {'yes' if grew else 'NO'}")
    if not grew:
        fails.append("the redraw marker did not grow on its own: the ~1s refresh is not running")

    # 4. Down moves the selection through the real logic
    keys("down")
    if not wait_serial("activity: sel 1", 5):
        fails.append('pressing Down did not log "activity: sel 1"')
    time.sleep(0.3)
    r0, r1 = row_pixel(0), row_pixel(1)
    if not (near(r1, SEL_COLOR) and near(r0, ROW_COLOR)):
        fails.append(f"the highlight did not move to slot 1 after Down (row 0 {r0}, row 1 {r1})")
    if "activity: crashing" in serial():
        fails.append("the program crashed before any crash key was pressed")

    # 5. Kill goes through SYS_TASKS: the shell is refused, a free slot is ENOENT
    move(VIEW_X + PROBE_X, VIEW_Y + TOP + 0 * ROW_H + 6); time.sleep(0.3); click()
    if not wait_serial("activity: sel 0", 5):
        fails.append('clicking slot 0 did not log "activity: sel 0"')
    move(*PARK); time.sleep(0.2)
    keys("k")
    if not wait_serial("activity: kill 0", 5) or not wait_serial("activity: kill result 1\n", 5):
        fails.append("killing slot 0 was not refused with EPERM (kill result 1): the shell could be killed")
    move(VIEW_X + PROBE_X, VIEW_Y + TOP + 4 * ROW_H + 6); time.sleep(0.3); click()
    if not wait_serial("activity: sel 4", 5):
        fails.append('clicking slot 4 did not log "activity: sel 4"')
    move(*PARK); time.sleep(0.2)
    keys("k")
    if not wait_serial("activity: kill 4", 5) or not wait_serial("activity: kill result 2\n", 5):
        fails.append("killing a free slot did not report ENOENT (kill result 2)")

    # 6. Esc closes it cleanly and the desktop comes back
    exits = serial().count("ACTIVITY.BIN exited 0")
    keys("esc")
    if not wait_serial("activity: closed", 5):
        fails.append("Esc did not reach the program (no closed line)")
    if not wait_serial("syscall: window released, task gone", 5) or serial().count("ACTIVITY.BIN exited 0") <= exits:
        fails.append("Activity did not exit 0 and release its window on Esc")
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
    if not opened: fails.append("Mail did not open from a dock click after Activity closed: desktop not responsive")
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
print("PASS: Activity ran at ring 3 with its own window, drew the live task list, refreshed it on its own, moved the selection, had SYS_TASKS refuse the shell and a free slot, closed on Esc, and the desktop stayed alive")
