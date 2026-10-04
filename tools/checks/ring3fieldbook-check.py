#!/usr/bin/env python3
"""Fieldbook runs as a real ring-3 process, the ninth app out of the kernel,
and crashing it does not take the desktop with it (roadmap 2.0, 1.9.3).

Boots headless with `open=field`, which launches Fieldbook from the dock path
the moment the desktop is up. Fieldbook is user/fieldbook.c, a flat binary loaded off
the VFS by exec_user and run at CPL 3 through the same table-driven
launcher the other ring-3 apps use (kernel/ring3app.c, RING3_APPS). The
check then:

  1. asserts, off the serial log, that the program opened a window of the
     dock viewport's size (804x345) and the kernel saw the open come from
     ring 3;
  2. reads the field rows' pixels: row 0 selected, row 1 plain;
  3. presses Down: asserts the program's own "fieldbook: sel 1" line and that
     the highlight pixel moved to row 1;
  4. clicks row 3: asserts "fieldbook: sel 3" and the highlight moved;
  5. presses the backquote, the deliberate crash key: a null write, a page
     fault at ring 3. Asserts the kernel reaped the task, released the
     window, the launcher logged the crash by name, and the desktop is
     back: the dock is on screen and Mail opens from a dock click;
  6. opens Fieldbook from the Apps folder grid (row 3, col 1, the 832x450
     folder viewport) and closes it with Esc, then again with the red
     close dot; after each it must have exited 0, released its window, and
     Mail must open from the dock;
  7. opens the Apps folder by keyboard (Enter on a bare desktop), launches
     Fieldbook from the grid, confirms it got a real window and no BUG line,
     backs out with two Esc, and confirms the desktop still takes a click.

Discriminating: replace the null write in user/fieldbook.c with jt_exit(0) and
step 5 fails; break the Down handling and step 3 fails.

Usage: tools/checks/ring3fieldbook-check.py   (from the repo root, after make kernel.elf)
"""
from appsgeom import FOLDER_CLOSE_X, FOLDER_CLOSE_Y  # one source for the Launchpad window's red dot
import json, os, socket, subprocess, sys, time
from PIL import Image
from freeport import free_port

LOG = "/tmp/jt-ring3fieldbook-serial.log"
DUMP = "/tmp/jt-ring3fieldbook.raw"
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
ROW_COLOR = (0xF1, 0xED, 0xE7)
SEL_COLOR = (0xE2, 0xD8, 0xCC)
FB_LIST_X, FB_TOP = 20, 56

os.chdir(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
for f in (LOG, DUMP):
    try: os.remove(f)
    except FileNotFoundError: pass

q = subprocess.Popen(["qemu-system-i386", "-kernel", "kernel.elf", "-append", "open=field",
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

    # 1. the program is up and has its window
    if not wait_serial("ring3app: launching FIELDBOOK.BIN at ring 3", 40):
        fails.append("Fieldbook was never launched as a ring-3 program (open=field flag or ring3app.c broken)")
    if not wait_serial("syscall: window opened for ring-3 task", 10):
        fails.append("SYS_WINDOW_OPEN never succeeded from ring 3")
    if not wait_serial("fieldbook: ring-3 window 804x345", 10):
        fails.append("the program did not report the app viewport's size (expected 804x345) through write()")
    time.sleep(0.5)

    # 2. content drawn: row 0 is the selected color, row 1 the plain one
    def row_pixel(i, img=None):
        return pixel(VIEW_X + FB_LIST_X, VIEW_Y + FB_TOP + i * 28, img)
    r0, r1 = row_pixel(0), row_pixel(1)
    print(f"row 0 (selected) {r0}, row 1 {r1}")
    if not near(r0, SEL_COLOR):
        fails.append(f"row 0 did not draw the selected color (got {r0}, expected {SEL_COLOR})")
    if not near(r1, ROW_COLOR):
        fails.append(f"row 1 did not draw the plain color (got {r1}, expected {ROW_COLOR})")

    # 3. Down moves the selection through the real logic, not just the draw
    keys("down"); time.sleep(0.3)
    if not wait_serial("fieldbook: sel 1", 5):
        fails.append('pressing Down did not log "fieldbook: sel 1"')
    r0, r1 = row_pixel(0), row_pixel(1)
    if not (near(r1, SEL_COLOR) and near(r0, ROW_COLOR)):
        fails.append(f"the highlight did not move to row 1 after Down (row 0 {r0}, row 1 {r1})")
    if "syscall: write(1) from ring 3: fieldbook: crashing" in serial():
        fails.append("the program crashed before the crash key was pressed")

    # 4. a click on row 3 selects it; Up at the top clamps
    move(VIEW_X + FB_LIST_X + 100, VIEW_Y + FB_TOP + 3 * 28 + 10); time.sleep(0.3); click(); time.sleep(0.3)
    if not wait_serial("fieldbook: sel 3", 5):
        fails.append('clicking row 3 did not log "fieldbook: sel 3"')
    if not near(row_pixel(3), SEL_COLOR):
        fails.append(f"row 3 did not flip to the selected color after the click (got {row_pixel(3)})")
    move(*PARK); time.sleep(0.2)

    # 5. the deliberate crash, and the supervisor's answer to it
    keys("grave_accent"); time.sleep(0.2)
    if not wait_serial("fieldbook: crashing on purpose", 5):
        fails.append("the crash key did not reach the program")
    if not wait_serial("exception: ring-3 task hit page-fault, reaped", 5):
        fails.append("the kernel did not reap the ring-3 task on its page fault")
    if not wait_serial("syscall: window released, task gone", 5):
        fails.append("the window was not released when the task died")
    if not wait_serial("ring3app: FIELDBOOK.BIN crashed (page-fault), window torn down, desktop alive", 5):
        fails.append("the launcher did not log the crash by name and return")
    if not wait_serial("autoopen: back on the desktop", 5):
        fails.append("the desktop loop was never re-entered after the crash")
    if "exception: ring-0" in serial() or "panic in" in serial():
        fails.append("the KERNEL faulted: the crash was not contained to the ring-3 task")

    # the desktop is alive and takes input
    move(*PARK); time.sleep(0.5)
    dock = pixel(480, 511)
    print(f"dock tray after crash: {dock}")
    if dock != (0xEF, 0xEB, 0xE4):
        fails.append(f"desktop dock not on screen after the crash (got {dock})")
    for _ in range(100):
        if not near(pixel(CLOSE_X, CLOSE_Y), CLOSE_RED): break
        time.sleep(0.1)
    if near(pixel(CLOSE_X, CLOSE_Y), CLOSE_RED):
        fails.append("an app window is still open after the crash; the dead app's window was not torn down")
    move(SLOT0_X + 2 * PITCH + DOCK_ICON // 2, ICON_ROW_Y); time.sleep(0.3); click()
    opened = False
    for _ in range(40):
        time.sleep(0.1)
        if near(pixel(CLOSE_X, CLOSE_Y), CLOSE_RED): opened = True; break
    print(f"Mail opens from the dock after the crash: {'yes' if opened else 'NO'}")
    if not opened: fails.append("Mail did not open from a dock click after the crash: desktop not responsive")
    keys("esc"); time.sleep(1.0)
    if near(pixel(CLOSE_X, CLOSE_Y), CLOSE_RED):
        fails.append("Mail did not close on Esc after the crash")

    # 6. a normal close, both ways, from the Apps folder grid: Fieldbook is
    #    APPS[] index 15 = row 3, col 0 (5 columns wide), whose viewport is
    #    the folder's 832x450, not the dock's 804x345.
    APPS_CLOSE_X, APPS_CLOSE_Y = FOLDER_CLOSE_X, FOLDER_CLOSE_Y
    def wait_closed(resend=True):
        # Poll the screen (10s) instead of reading it once: on a slow runner
        # the post-Esc repaint lands after a fixed sleep. One resend of Esc
        # at 4s covers an Esc lost mid-repaint.
        for i in range(100):
            img = frame()
            if not near(pixel(CLOSE_X, CLOSE_Y, img), CLOSE_RED) and not near(pixel(APPS_CLOSE_X, APPS_CLOSE_Y, img), CLOSE_RED):
                return True
            if resend and i == 40: keys("esc")
            time.sleep(0.1)
        return False
    def open_fieldbook_from_grid(tag):
        seen = serial().count("fieldbook: ring-3 window")
        move(*PARK); time.sleep(0.2)
        move(SLOT0_X + DOCK_ICON // 2, ICON_ROW_Y); time.sleep(0.3); click(); time.sleep(1.0)
        for _ in range(3): keys("s"); time.sleep(0.35)  # down x3 -> index 15 (row 3, col 0)
        keys("ret")
        for _ in range(60):
            time.sleep(0.1)
            if serial().count("fieldbook: ring-3 window") > seen: break
        else:
            fails.append(f"{tag}: Fieldbook did not open a ring-3 window from the Apps folder grid"); return False
        if "fieldbook: ring-3 window 804x345" not in serial():
            fails.append(f"{tag}: the folder-launched window is not 804x345")
        if "ring3app: BUG" in serial():
            fails.append(f"{tag}: ring3app logged a BUG line")
        time.sleep(0.5)
        return True
    def assert_closed(tag, exits_before):
        if not wait_serial("syscall: window released, task gone", 5) or serial().count("FIELDBOOK.BIN exited 0") <= exits_before:
            fails.append(f"{tag}: Fieldbook did not exit 0 and release its window on a normal close")
        keys("esc"); time.sleep(0.8)  # the Apps folder itself
        move(*PARK); time.sleep(0.3)
        if not wait_closed():
            fails.append(f"{tag}: a window is still open after the close")
        move(SLOT0_X + 2 * PITCH + DOCK_ICON // 2, ICON_ROW_Y); time.sleep(0.3); click()
        ok = False
        for _ in range(40):
            time.sleep(0.1)
            if near(pixel(CLOSE_X, CLOSE_Y), CLOSE_RED): ok = True; break
        print(f"{tag}: Mail opens from the dock afterwards: {'yes' if ok else 'NO'}")
        if not ok: fails.append(f"{tag}: Mail did not open from a dock click after the close: desktop stuck")
        keys("esc"); time.sleep(1.0)
    exits = serial().count("FIELDBOOK.BIN exited 0")
    if open_fieldbook_from_grid("esc-close"):
        keys("esc"); time.sleep(0.5)
        assert_closed("esc-close", exits)
    exits = serial().count("FIELDBOOK.BIN exited 0")
    if open_fieldbook_from_grid("dot-close"):
        move(APPS_CLOSE_X, APPS_CLOSE_Y); time.sleep(0.3); click(); time.sleep(0.5)
        assert_closed("dot-close", exits)

    # 7. the keyboard path into the Apps folder, not the dock click.
    seen = serial().count("fieldbook: ring-3 window")
    move(*PARK); time.sleep(0.3)
    keys("ret"); time.sleep(1.0)  # bare desktop -> Apps folder, by keyboard
    for _ in range(3): keys("s"); time.sleep(0.35)  # down x3 -> index 15 (row 3, col 0)
    keys("ret")  # launch Fieldbook from the grid selection
    for _ in range(60):
        time.sleep(0.1)
        if serial().count("fieldbook: ring-3 window") > seen: break
    else:
        fails.append("keyboard-open: Fieldbook did not open a ring-3 window after Enter opened the Apps folder by keyboard")
    if "ring3app: BUG" in serial():
        fails.append("keyboard-open: ring3app logged a BUG line launching Fieldbook from a keyboard-opened Apps folder")
    keys("esc"); time.sleep(0.5)  # closes Fieldbook
    keys("esc"); time.sleep(0.5)  # closes the Apps folder
    move(*PARK); time.sleep(0.3)
    if not wait_closed():
        fails.append("keyboard-open: a window is still open after the two Esc presses")
    move(SLOT0_X + 2 * PITCH + DOCK_ICON // 2, ICON_ROW_Y); time.sleep(0.3); click()
    opened = False
    for _ in range(40):
        time.sleep(0.1)
        if near(pixel(CLOSE_X, CLOSE_Y), CLOSE_RED): opened = True; break
    print(f"keyboard-open: Mail opens from the dock afterwards: {'yes' if opened else 'NO'}")
    if not opened: fails.append("keyboard-open: Mail did not open from a dock click after the keyboard-opened Apps folder closed: desktop stuck")
    keys("esc"); time.sleep(1.0)
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
print("PASS: Fieldbook ran at ring 3 with its own window, drew the ranked field list, moved the selection by key and click through the real logic, crashed on demand, closed normally both ways from the Apps folder, and the desktop stayed alive")
