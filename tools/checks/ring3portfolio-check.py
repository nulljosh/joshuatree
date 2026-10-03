#!/usr/bin/env python3
"""Portfolio runs as a real ring-3 process, the eleventh app out of the kernel
(roadmap 2.0, 1.9.5).

Boots headless with `open=portf`, which launches Portfolio from the dock path
the moment the desktop is up. Portfolio is user/portfolio.c, a flat binary
loaded off the VFS by exec_user and run at CPL 3 through the table-driven
launcher (kernel/ring3app.c, RING3_APPS). The check then:

  1. asserts, off the serial log, that the program opened a window of the
     dock viewport's size (804x345);
  2. reads the catalog rows' pixels: the first app row (Epiphany) is
     highlighted, the one below it is plain;
  3. presses Down: asserts the program's own "portfolio: sel 7" line and that
     the highlight moved one row;
  4. clicks the row below: asserts "portfolio: sel 8";
  5. presses Esc: asserts it exited 0, released its window, and the desktop
     is back (Mail opens from a dock click).

Every wait has a deadline. Discriminating: break the header skip and step 3
fails; make Esc a no-op and step 5 fails.

Usage: tools/checks/ring3portfolio-check.py   (from the repo root, after make kernel.elf)
"""
import json, os, socket, subprocess, sys, time
from PIL import Image
from freeport import free_port

LOG = "/tmp/jt-ring3portfolio-serial.log"
DUMP = "/tmp/jt-ring3portfolio.raw"
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
PF_TOP, PF_ROW_H = 40, 22   # user/portfolio.c: list top and row pitch (was 68/20 before the antialiased-type pass)
PROBE_X = 700               # right of the text, inside the highlight bar

os.chdir(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
for f in (LOG, DUMP):
    try: os.remove(f)
    except FileNotFoundError: pass

q = subprocess.Popen(["qemu-system-i386", "-kernel", "kernel.elf", "-append", "open=portf",
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
        return pixel(VIEW_X + PROBE_X, VIEW_Y + PF_TOP + i * PF_ROW_H + 6, img)

    # 1. the program is up and has its window
    if not wait_serial("ring3app: launching PORTFOLIO.BIN at ring 3", 40):
        fails.append("Portfolio was never launched as a ring-3 program (open=portf flag or ring3app.c broken)")
    if not wait_serial("syscall: window opened for ring-3 task", 10):
        fails.append("SYS_WINDOW_OPEN never succeeded from ring 3")
    if not wait_serial("portfolio: ring-3 window 804x345", 10):
        fails.append("the program did not report the app viewport's size (expected 804x345) through write()")
    time.sleep(0.5)

    # 2. the About block is above, Epiphany (row 6) is the selected app row
    r6, r7 = row_pixel(6), row_pixel(7)
    print(f"row 6 (selected) {r6}, row 7 {r7}")
    if not near(r6, SEL_COLOR):
        fails.append(f"the first app row did not draw the selected color (got {r6}, expected {SEL_COLOR})")
    if not near(r7, ROW_COLOR):
        fails.append(f"the next app row did not draw the plain color (got {r7}, expected {ROW_COLOR})")

    # 3. Down moves the selection through the real logic, not just the draw
    keys("down")
    if not wait_serial("portfolio: sel 7", 5):
        fails.append('pressing Down did not log "portfolio: sel 7"')
    time.sleep(0.3)
    r6, r7 = row_pixel(6), row_pixel(7)
    if not (near(r7, SEL_COLOR) and near(r6, ROW_COLOR)):
        fails.append(f"the highlight did not move to row 7 after Down (row 6 {r6}, row 7 {r7})")
    if "portfolio: crashing" in serial():
        fails.append("the program crashed before the crash key was pressed")

    # a click on the row below selects it
    move(VIEW_X + PROBE_X, VIEW_Y + PF_TOP + 8 * PF_ROW_H + 6); time.sleep(0.3); click()
    if not wait_serial("portfolio: sel 8", 5):
        fails.append('clicking the next row did not log "portfolio: sel 8"')
    move(*PARK); time.sleep(0.2)

    # 5. Esc closes it cleanly and the desktop comes back
    exits = serial().count("PORTFOLIO.BIN exited 0")
    keys("esc")
    if not wait_serial("portfolio: closed", 5):
        fails.append("Esc did not reach the program (no closed line)")
    released = wait_serial("syscall: window released, task gone", 5)
    # The "exited 0" line is logged a beat AFTER "window released" (the reaper
    # runs on a later tick), so read it with its own deadline: a single read right
    # after the release line lost that race on a slow CI runner.
    for _ in range(50):
        if serial().count("PORTFOLIO.BIN exited 0") > exits: break
        time.sleep(0.1)
    if not released or serial().count("PORTFOLIO.BIN exited 0") <= exits:
        fails.append("Portfolio did not exit 0 and release its window on Esc")
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
    if not opened: fails.append("Mail did not open from a dock click after Portfolio closed: desktop not responsive")
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
print("PASS: Portfolio ran at ring 3 with its own window, drew the catalog, moved the selection by key and click through the real logic, closed on Esc, and the desktop stayed alive")
