#!/usr/bin/env python3
"""Calculator runs as a real ring-3 process, the third app out of the
kernel, and crashing it does not take the desktop with it (roadmap 2.0,
1.7.12).

Boots headless with `open=calc`, which launches Calculator from the dock
path the moment the desktop is up. Calculator is user/calculator.c, a flat
binary loaded off the VFS by exec_user and run at CPL 3 through the same
table-driven launcher Keyrate and Toroid use (kernel/ring3app.c,
RING3_APPS). The parser is the same recursive-descent grammar
drivers/app_calculator.c ran in ring 0 (still evaluated the same way for
the same cases: '*'/'/' bind tighter, parens override, unary minus, and
dividing by zero yields 0 rather than a fault), just folded straight into
a double instead of building an expr_node tree with kmalloc, because a
flat user binary has no .bss and no heap (user/note.ld). The check then:

  1. asserts, off the serial log, that the program opened a window of the
     dock viewport's size (804x345) and the kernel saw the open come from
     ring 3;
  2. types "12*3", presses enter, and reads the result off the serial line
     the program itself prints (not pixels: there is no font syscall, and
     the program draws its own glyphs into a framebuffer the check would
     otherwise have to OCR) -- asserts it says "36", proving precedence
     (multiply before the implicit end) round-trips through the real
     ring-3 parser;
  3. types "5/0", presses enter, and asserts the result is "0": the
     in-kernel version's divide-by-zero behavior (calc_eval's
     `b != 0 ? a / b : 0`), preserved exactly rather than turned into a
     crash or a NaN;
  4. presses the backquote, the deliberate crash key: a null write, a page
     fault at ring 3. Asserts the kernel reaped the task, released the
     window, the launcher logged the crash by name, and the desktop is
     back: the dock is on screen and Mail opens from a dock click;
  5. opens Calculator from the Apps folder grid (row 3, col 3, the
     832x450 folder viewport) and closes it with Esc, then again with the
     red close dot; after each it must have exited 0, released its
     window, and Mail must open from the dock;
  6. opens the Apps folder by keyboard (Enter on a bare desktop), launches
     Calculator from the grid, confirms it got a real window and no BUG
     line, backs out with two Esc, and confirms the desktop still takes a
     click.

Discriminating: replace the null write in user/calculator.c with
jt_exit(0) and step 4 fails; break calc_term's precedence (fold + and *
the same way) and step 2 reads a wrong number; drop the `right != 0 ? ... : 0`
guard and step 3 either hangs or reads garbage; break gui_apps_launch's
viewport setup and steps 5 and 6 fail.

Usage: tools/checks/ring3calc-check.py   (from the repo root, after make kernel.elf)
"""
import json, os, socket, subprocess, sys, time
from PIL import Image
from freeport import free_port

LOG = "/tmp/jt-ring3calc-serial.log"
DUMP = "/tmp/jt-ring3calc.raw"
FB = 0xfd000000; W, H = 1920, 1080
PORT = free_port()
LOGICAL_W, LOGICAL_H, SCALE = 960, 540, 2
DOCK_ICON, DOCK_GAP, SLOT0_X = 37, 6, 247
PITCH = DOCK_ICON + DOCK_GAP
ICON_ROW_Y = 487
CLOSE_X, CLOSE_Y = 94, 56
CLOSE_RED = (0xFF, 0x5F, 0x57)
PARK = (480, 200)

os.chdir(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
for f in (LOG, DUMP):
    try: os.remove(f)
    except FileNotFoundError: pass

q = subprocess.Popen(["qemu-system-i386", "-kernel", "kernel.elf", "-append", "open=calc",
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

    DIGIT_QCODE = {"0": "0", "1": "1", "2": "2", "3": "3", "4": "4", "5": "5",
                   "6": "6", "7": "7", "8": "8", "9": "9",
                   "*": "shift-8", "/": "slash"}
    SCI_QCODE = {"^": "shift-6", "!": "shift-1", "(": "shift-9", ")": "shift-0", ".": "dot"}
    def type_expr(expr):
        for c in expr:
            codes = (DIGIT_QCODE.get(c) or SCI_QCODE.get(c) or c).split('-')
            cmd({"execute": "send-key", "arguments": {"keys": [{"type": "qcode", "data": code} for code in codes], "hold-time": 30}})
            time.sleep(0.15)

    # 1. the program is up and has its window
    if not wait_serial("ring3app: launching CALC.BIN at ring 3", 40):
        fails.append("Calculator was never launched as a ring-3 program (open=calc flag or ring3app.c broken)")
    if not wait_serial("syscall: window opened for ring-3 task", 10):
        fails.append("SYS_WINDOW_OPEN never succeeded from ring 3")
    if not wait_serial("calculator: ring-3 window 804x345", 10):
        fails.append("the program did not report the app viewport's size (expected 804x345) through write()")
    time.sleep(0.5)

    # 2. "12*3" -> 36, precedence and multiplication through the real parser
    seen = serial().count("calculator: ")
    type_expr("12*3")
    keys("ret"); time.sleep(0.3)
    if not wait_serial("calculator: 12*3 = 36", 5):
        fails.append("12*3 did not evaluate to 36 (got: " + serial().split("calculator: 12*3")[-1][:20] + ")" if "calculator: 12*3" in serial() else "12*3 never printed a result line")

    # 3. "5/0" -> 0, the in-kernel version's divide-by-zero behavior
    for _ in range(5): keys("backspace"); time.sleep(0.05)  # clear "12*3" from the input box
    type_expr("5/0")
    keys("ret"); time.sleep(0.3)
    if not wait_serial("calculator: 5/0 = 0", 5):
        fails.append("5/0 did not evaluate to 0 (the in-kernel divide-by-zero behavior)")
    # 3b. scientific: Tab shows the keys, and the math is real (x87, no libm)
    keys("tab")
    if not wait_serial("calculator: scientific", 5):
        fails.append("Tab did not switch Calculator to scientific")
    for expr, want in (("2^10", "1024"), ("sqrt(2)", "1.4142"), ("sin(pi/2)", "1"), ("5!", "120"), ("ln(e^3)", "3")):
        for _ in range(12): keys("backspace"); time.sleep(0.03)
        type_expr(expr)
        keys("ret"); time.sleep(0.3)
        if not wait_serial("calculator: %s = %s\n" % (expr, want), 5):
            got = serial().split("calculator: %s = " % expr)[-1][:12] if ("calculator: %s = " % expr) in serial() else "nothing"
            fails.append("scientific %s should be %s, got %s" % (expr, want, got))
    if "syscall: write(1) from ring 3: calculator: crashing" in serial():
        fails.append("the program crashed before the crash key was pressed")

    # 4. the deliberate crash, and the supervisor's answer to it
    keys("grave_accent"); time.sleep(0.2)
    if not wait_serial("calculator: crashing on purpose", 5):
        fails.append("the crash key did not reach the program")
    if not wait_serial("exception: ring-3 task hit page-fault, reaped", 5):
        fails.append("the kernel did not reap the ring-3 task on its page fault")
    if not wait_serial("syscall: window released, task gone", 5):
        fails.append("the window was not released when the task died")
    if not wait_serial("ring3app: CALC.BIN crashed (page-fault), window torn down, desktop alive", 5):
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

    # 5. a normal close, both ways, from the Apps folder grid: Calculator
    #    is APPS[] index 17 = row 3, col 2 (5 columns wide), whose
    #    viewport is the folder's 832x450, not the dock's 804x345.
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
    def open_calc_from_grid(tag):
        seen = serial().count("calculator: ring-3 window")
        move(*PARK); time.sleep(0.2)
        move(SLOT0_X + DOCK_ICON // 2, ICON_ROW_Y); time.sleep(0.3); click(); time.sleep(1.0)
        for _ in range(2): keys("d"); time.sleep(0.35)  # right x2
        for _ in range(3): keys("s"); time.sleep(0.35)  # down x3 -> index 17
        keys("ret")
        for _ in range(60):
            time.sleep(0.1)
            if serial().count("calculator: ring-3 window") > seen: break
        else:
            fails.append(f"{tag}: Calculator did not open a ring-3 window from the Apps folder grid"); return False
        if "calculator: ring-3 window 832x450" not in serial():
            fails.append(f"{tag}: the folder-launched window is not the folder viewport's 832x450")
        if "ring3app: BUG" in serial():
            fails.append(f"{tag}: ring3app logged a BUG line")
        time.sleep(0.5)
        return True
    def assert_closed(tag, exits_before):
        if not wait_serial("syscall: window released, task gone", 5) or serial().count("CALC.BIN exited 0") <= exits_before:
            fails.append(f"{tag}: Calculator did not exit 0 and release its window on a normal close")
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
    exits = serial().count("CALC.BIN exited 0")
    if open_calc_from_grid("esc-close"):
        keys("esc"); time.sleep(0.5)
        assert_closed("esc-close", exits)
    exits = serial().count("CALC.BIN exited 0")
    if open_calc_from_grid("dot-close"):
        move(APPS_CLOSE_X, APPS_CLOSE_Y); time.sleep(0.3); click(); time.sleep(0.5)
        assert_closed("dot-close", exits)

    # 6. the keyboard path into the Apps folder, not the dock click.
    seen = serial().count("calculator: ring-3 window")
    move(*PARK); time.sleep(0.3)
    keys("ret"); time.sleep(1.0)  # bare desktop -> Apps folder, by keyboard
    for _ in range(3): keys("d"); time.sleep(0.35)
    for _ in range(3): keys("s"); time.sleep(0.35)
    keys("ret")  # launch Calculator from the grid selection
    for _ in range(60):
        time.sleep(0.1)
        if serial().count("calculator: ring-3 window") > seen: break
    else:
        fails.append("keyboard-open: Calculator did not open a ring-3 window after Enter opened the Apps folder by keyboard")
    if "ring3app: BUG" in serial():
        fails.append("keyboard-open: ring3app logged a BUG line launching Calculator from a keyboard-opened Apps folder")
    keys("esc"); time.sleep(0.5)  # closes Calculator
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
print("PASS: Calculator ran at ring 3 with its own window, evaluated 12*3=36 and 5/0=0 through the real parser, crashed on demand, closed normally both ways from the Apps folder, and the desktop stayed alive")
