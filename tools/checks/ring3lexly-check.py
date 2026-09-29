#!/usr/bin/env python3
"""Lexly runs as a real ring-3 process, the seventh app out of the kernel,
and crashing it does not take the desktop with it (roadmap 2.0, 1.9.1).

Boots headless with `open=lexly`, which launches Lexly from the dock path
the moment the desktop is up. Lexly is user/lexly.c, a flat binary
loaded off the VFS by exec_user and run at CPL 3 through the same
table-driven launcher Keyrate, Toroid, Calculator, Quotes, Bookrank and Homeqi use (kernel/ring3app.c,
RING3_APPS). The deck and answer-rotation are the same drivers/
app_lexlytreak.c ran in ring 0 (lx_cur/lx_option are deterministic off
lx_round, so round 0's right answer is always slot 0). The check then:

  1. asserts, off the serial log, that the program opened a window of the
     dock viewport's size (804x345) and the kernel saw the open come from
     ring 3;
  2. reads the option row's background pixel before any pick, asserting it
     is the undecided option color: the grid actually drew;
  3. presses "1" (round 0's slot 0 is always the right answer): asserts
     the program's own serial line says "pick 1 right" and the streak went
     up, and that slot 0's pixel flipped to the right-answer color;
  4. presses any key to advance ("lexly: next"), then on round 1 (the
     scorer's right slot is 1) presses "1": asserts "pick 1 miss" and that
     the missed slot's pixel is the miss color;
  5. presses the backquote, the deliberate crash key: a null write, a page
     fault at ring 3. Asserts the kernel reaped the task, released the
     window, the launcher logged the crash by name, and the desktop is
     back: the dock is on screen and Mail opens from a dock click;
  6. opens Lexly from the Apps folder grid (row 2, col 3, the 832x450
     folder viewport) and closes it with Esc, then again with the red
     close dot; after each it must have exited 0, released its window, and
     Mail must open from the dock;
  7. opens the Apps folder by keyboard (Enter on a bare desktop), launches
     Lexly from the grid, confirms it got a real window and no BUG line,
     backs out with two Esc, and confirms the desktop still takes a click.

Discriminating: replace the null write in user/lexly.c with jt_exit(0)
and step 5 fails; break lx_option's slot math and step 3 or 4 reads the
wrong pick/miss line or the wrong pixel; break gui_apps_launch's viewport
setup and steps 6 and 7 fail.

Usage: tools/checks/ring3lexly-check.py   (from the repo root, after make kernel.elf)
"""
import json, os, socket, subprocess, sys, time
from PIL import Image
from freeport import free_port

LOG = "/tmp/jt-ring3lexly-serial.log"
DUMP = "/tmp/jt-ring3lexly.raw"
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
OPT_COLOR = (0xF1, 0xED, 0xE7)
RIGHT_COLOR = (0xCF, 0xE8, 0xD2)
MISS_COLOR = (0xF0, 0xD0, 0xCC)
LX_OPT_Y0 = 150

os.chdir(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
for f in (LOG, DUMP):
    try: os.remove(f)
    except FileNotFoundError: pass

q = subprocess.Popen(["qemu-system-i386", "-kernel", "kernel.elf", "-append", "open=lexly",
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
    def option_pixel(slot, img=None):
        # Mid-row, well inside the option box (user/lexly.c LX_OPT_Y0/LX_OPT_H).
        y = LX_OPT_Y0 + slot * 34 + 10
        return pixel(VIEW_X + 30, VIEW_Y + y, img)

    # 1. the program is up and has its window
    if not wait_serial("ring3app: launching LEXLY.BIN at ring 3", 40):
        fails.append("Lexly was never launched as a ring-3 program (open=lexly flag or ring3app.c broken)")
    if not wait_serial("syscall: window opened for ring-3 task", 10):
        fails.append("SYS_WINDOW_OPEN never succeeded from ring 3")
    if not wait_serial("lexly: ring-3 window 804x345", 10):
        fails.append("the program did not report the app viewport's size (expected 804x345) through write()")
    time.sleep(0.5)

    # 2. content drawn: the undecided option row is the undecided color
    before = option_pixel(0)
    print(f"option row before any pick: {before}")
    if not near(before, OPT_COLOR):
        fails.append(f"option row did not draw the undecided color (got {before}, expected {OPT_COLOR})")

    # 3. round 0's right answer is always slot 0 (lx_cur/lx_option are
    #    deterministic off lx_round). Press "1" and check the real
    #    answer-checking logic ran, not just the draw.
    keys("1"); time.sleep(0.3)
    if not wait_serial("lexly: pick 1 right", 5):
        fails.append('pressing "1" on round 0 (always the right slot) did not log "pick 1 right"')
    after = option_pixel(0)
    print(f"slot 0 after the right pick: {after}")
    if not near(after, RIGHT_COLOR):
        fails.append(f"slot 0 did not flip to the right-answer color after a correct pick (got {after})")
    if "syscall: write(1) from ring 3: lexly: crashing" in serial():
        fails.append("the program crashed before the crash key was pressed")

    # 4. any key advances; round 1 scores slot 1 as right. Miss it on
    #    purpose with "1" and check the miss is drawn.
    keys("1"); time.sleep(0.3)  # "any key for the next one"
    if not wait_serial("lexly: next", 5):
        fails.append('advancing past a picked answer did not log "lexly: next"')
    keys("1"); time.sleep(0.3)
    if not wait_serial("lexly: pick 1 miss", 5):
        fails.append('pressing "1" on round 1 (the scorer\'s right slot is 1) did not log "pick 1 miss"')
    miss_px = option_pixel(0)
    print(f"slot 0 (missed) {miss_px}")
    if not near(miss_px, MISS_COLOR):
        fails.append(f"the missed slot did not flip to the miss color (got {miss_px})")
    keys("3"); time.sleep(0.3)  # advance again, back to a clean state before the crash
    if not wait_serial("lexly: next", 5):
        fails.append('advancing past the missed answer did not log a second "lexly: next"')

    # 5. the deliberate crash, and the supervisor's answer to it
    keys("grave_accent"); time.sleep(0.2)
    if not wait_serial("lexly: crashing on purpose", 5):
        fails.append("the crash key did not reach the program")
    if not wait_serial("exception: ring-3 task hit page-fault, reaped", 5):
        fails.append("the kernel did not reap the ring-3 task on its page fault")
    if not wait_serial("syscall: window released, task gone", 5):
        fails.append("the window was not released when the task died")
    if not wait_serial("ring3app: LEXLY.BIN crashed (page-fault), window torn down, desktop alive", 5):
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

    # 6. a normal close, both ways, from the Apps folder grid: Lexly is
    #    APPS[] index 11 = row 2, col 3 (5 columns wide), whose viewport is
    #    the folder's 832x450, not the dock's 804x345.
    APPS_CLOSE_X, APPS_CLOSE_Y = 80, 46
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
    def open_lexly_from_grid(tag):
        seen = serial().count("lexly: ring-3 window")
        move(*PARK); time.sleep(0.2)
        move(SLOT0_X + DOCK_ICON // 2, ICON_ROW_Y); time.sleep(0.3); click(); time.sleep(1.0)
        for _ in range(3): keys("d"); time.sleep(0.35)   # right x3
        for _ in range(2): keys("s"); time.sleep(0.35)  # down x2 -> index 13
        keys("ret")
        for _ in range(60):
            time.sleep(0.1)
            if serial().count("lexly: ring-3 window") > seen: break
        else:
            fails.append(f"{tag}: Lexly did not open a ring-3 window from the Apps folder grid"); return False
        if "lexly: ring-3 window 832x450" not in serial():
            fails.append(f"{tag}: the folder-launched window is not the folder viewport's 832x450")
        if "ring3app: BUG" in serial():
            fails.append(f"{tag}: ring3app logged a BUG line")
        time.sleep(0.5)
        return True
    def assert_closed(tag, exits_before):
        if not wait_serial("syscall: window released, task gone", 5) or serial().count("LEXLY.BIN exited 0") <= exits_before:
            fails.append(f"{tag}: Lexly did not exit 0 and release its window on a normal close")
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
    exits = serial().count("LEXLY.BIN exited 0")
    if open_lexly_from_grid("esc-close"):
        keys("esc"); time.sleep(0.5)
        assert_closed("esc-close", exits)
    exits = serial().count("LEXLY.BIN exited 0")
    if open_lexly_from_grid("dot-close"):
        move(APPS_CLOSE_X, APPS_CLOSE_Y); time.sleep(0.3); click(); time.sleep(0.5)
        assert_closed("dot-close", exits)

    # 7. the keyboard path into the Apps folder, not the dock click.
    seen = serial().count("lexly: ring-3 window")
    move(*PARK); time.sleep(0.3)
    keys("ret"); time.sleep(1.0)  # bare desktop -> Apps folder, by keyboard
    for _ in range(3): keys("d"); time.sleep(0.35)
    for _ in range(2): keys("s"); time.sleep(0.35)
    keys("ret")  # launch Lexly from the grid selection
    for _ in range(60):
        time.sleep(0.1)
        if serial().count("lexly: ring-3 window") > seen: break
    else:
        fails.append("keyboard-open: Lexly did not open a ring-3 window after Enter opened the Apps folder by keyboard")
    if "ring3app: BUG" in serial():
        fails.append("keyboard-open: ring3app logged a BUG line launching Lexly from a keyboard-opened Apps folder")
    keys("esc"); time.sleep(0.5)  # closes Lexly
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
    q.terminate()
    try: q.wait(5)
    except subprocess.TimeoutExpired: q.kill()

if fails:
    print("FAIL:")
    for x in fails: print("  - " + x)
    print("--- serial tail ---")
    print(serial()[-1500:])
    sys.exit(1)
print("PASS: Lexly ran at ring 3 with its own window, drew the word and option grid, answered right and wrong through the real logic, crashed on demand, closed normally both ways from the Apps folder, and the desktop stayed alive")
