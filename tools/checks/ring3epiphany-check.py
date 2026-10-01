#!/usr/bin/env python3
"""Epiphany runs as a real ring-3 process, the nineteenth app out of the
kernel (roadmap 2.0, 1.9.17), on SYS_HTTP_GET like Curbfind.

Boots headless with `open=epip`, which launches Epiphany from the dock path
the moment the desktop is up. Epiphany is user/epiphany.c, a flat binary run
at CPL 3 through the table-driven launcher (kernel/ring3app.c, RING3_APPS).
Headless QEMU has no NIC this kernel drives, so the /api/quotes fetch comes
back -ENODEV and the compiled-in prices show, the same offline fallback the
in-kernel copy had. The check then:

  1. asserts, off the serial log, the launch, a window of the dock viewport's
     size (804x345), the refused fetch ("epiphany: fetch -19") and the
     offline fallback, and that the Markets tab is the highlighted one;
  2. presses Right and checks the highlight moved to Portfolio, then clicks
     the Situation tab through the real pointer and checks it moved there;
  3. runs the command bar ("/", "aapl des", Enter) and asserts the program's
     "epicmd=run:AAPL DES" marker, then a bogus code and its
     "epicmd=unknown_code:ZZ" marker;
  4. closes on Esc (clears the view, then closes) with a clean exit 0 and a
     released window, and asserts the desktop is back (Mail opens).

Every wait has a deadline. Discriminating: drop the RING3_APPS row and step 1
never sees the launch; break the tab hit test and step 2 fails; drop the
epicmd write and step 3 fails.

Usage: tools/checks/ring3epiphany-check.py   (from the repo root, after make kernel.elf)
"""
import json, os, socket, subprocess, sys, time
from PIL import Image
from freeport import free_port

LOG = "/tmp/jt-ring3epiphany-serial.log"
DUMP = "/tmp/jt-ring3epiphany.raw"
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
ACCENT = (0x0A, 0x84, 0xFF)   # user/epiphany.c ACCENT, the active tab's fill
TAB_X, TAB_PITCH, TAB_TOP = 24 - 8 + 3, 110, 8 + 3   # tab i's fill starts at 16 + 110 * i, TOP + 3

os.chdir(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
for f in (LOG, DUMP):
    try: os.remove(f)
    except FileNotFoundError: pass

q = subprocess.Popen(["qemu-system-i386", "-kernel", "kernel.elf", "-append", "open=epip",
                      "-display", "none", "-vga", "std",
                      "-qmp", f"tcp:127.0.0.1:{PORT},server,nowait", "-serial", "file:" + LOG],
                     stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
fails = []
def serial():
    try: return open(LOG, errors="replace").read()
    except OSError: return ""
def wait_serial(needle, secs, count=1):
    for _ in range(int(secs * 10)):
        if serial().count(needle) >= count: return True
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
        time.sleep(0.35)  # faster than this drops scancodes on a loaded host
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
    def active_tab(img=None):
        img = img or frame()
        hits = [i for i in range(4) if near(pixel(VIEW_X + TAB_X + i * TAB_PITCH, VIEW_Y + TAB_TOP, img), ACCENT)]
        return hits[0] if len(hits) == 1 else -1
    def type_str(text):
        for ch in text: keys({" ": "spc"}.get(ch, ch))

    # 1. the program is up, the fetch fell back, Markets is the active tab
    if not wait_serial("ring3app: launching EPIPHANY.BIN at ring 3", 40):
        fails.append("Epiphany was never launched as a ring-3 program (open=epip flag or ring3app.c broken)")
    if not wait_serial("syscall: window opened for ring-3 task", 10):
        fails.append("SYS_WINDOW_OPEN never succeeded from ring 3")
    if not wait_serial("epiphany: ring-3 window 804x345", 10):
        fails.append("the program did not report the app viewport's size (expected 804x345) through write()")
    if not wait_serial("epiphany: fetch -19", 15):
        fails.append('SYS_HTTP_GET did not come back -ENODEV ("epiphany: fetch -19") on a guest with no NIC this kernel drives')
    if not wait_serial("epiphany: offline 0", 10):
        fails.append('the program did not fall back to the offline prices ("epiphany: offline 0")')
    time.sleep(0.5)
    t = active_tab()
    print(f"active tab after launch: {t}")
    if t != 0: fails.append(f"Markets was not the highlighted tab after launch (got {t})")

    # 2. Right moves to Portfolio, a click on Situation moves there
    keys("right"); time.sleep(0.3)
    t = active_tab()
    if t != 1: fails.append(f"Right did not move the highlight to Portfolio (got {t})")
    move(VIEW_X + 24 + 3 * TAB_PITCH + 20, VIEW_Y + 8 + 12); time.sleep(0.3); click(); time.sleep(0.4)
    t = active_tab()
    if t != 3: fails.append(f"clicking the Situation tab did not select it (got {t})")
    keys("1"); time.sleep(0.3)
    if active_tab() != 0: fails.append("the 1 key did not return to Markets")

    # 3. the command bar
    keys("slash"); type_str("aapl des"); keys("ret")
    if not wait_serial("epicmd=run:AAPL DES", 5):
        fails.append('the command bar did not log "epicmd=run:AAPL DES"')
    keys("esc")  # clears the DES view, does not close
    keys("slash"); type_str("aapl zz"); keys("ret")
    if not wait_serial("epicmd=unknown_code:ZZ", 5):
        fails.append('a bogus code did not log "epicmd=unknown_code:ZZ"')
    if "epiphany: closed" in serial():
        fails.append("Esc with a view up closed the app instead of clearing the view")

    # 4. Esc closes it cleanly
    exits = serial().count("EPIPHANY.BIN exited 0")
    keys("esc")
    if not wait_serial("epiphany: closed", 5):
        fails.append("Esc did not reach the program (no closed line)")
    if not wait_serial("EPIPHANY.BIN exited 0", 5, exits + 1):
        fails.append("Epiphany did not exit 0 on Esc")
    if not wait_serial("syscall: window released, task gone", 5):
        fails.append("Epiphany did not release its window on Esc")
    move(*PARK); time.sleep(0.5)
    for _ in range(100):
        if not near(pixel(CLOSE_X, CLOSE_Y), CLOSE_RED): break
        time.sleep(0.1)
    if near(pixel(CLOSE_X, CLOSE_Y), CLOSE_RED):
        fails.append("an app window is still open after Esc")

    # 5. the desktop must answer
    if "exception: ring-0" in serial() or "panic in" in serial() or "ring3app: BUG" in serial():
        fails.append("the kernel faulted or ring3app logged a BUG line")
    dock = pixel(480, 511)
    if dock != (0xEF, 0xEB, 0xE4):
        fails.append(f"desktop dock not on screen after the close (got {dock})")
    move(SLOT0_X + 2 * PITCH + DOCK_ICON // 2, ICON_ROW_Y); time.sleep(0.3); click()
    opened = False
    for _ in range(40):
        time.sleep(0.1)
        if near(pixel(CLOSE_X, CLOSE_Y), CLOSE_RED): opened = True; break
    print(f"Mail opens from the dock after the close: {'yes' if opened else 'NO'}")
    if not opened: fails.append("Mail did not open from a dock click after Epiphany closed: desktop not responsive")
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
print("PASS: Epiphany ran at ring 3 with its own window, fell back to the offline prices when SYS_HTTP_GET found no NIC, switched tabs by key and click, ran the command bar, closed on Esc, and the desktop stayed alive")
