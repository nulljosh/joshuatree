#!/usr/bin/env python3
"""Headless proof of Epiphany's command bar (user/epiphany.c, "AAPL GP" /
"AAPL DES"): opens Epiphany from the Apps folder (grid index 20, same nav
as feature-drive.py), presses `/` to focus the bar, types "aapl gp" and
Enter, and checks two things: the GP chart panel actually drew (a real
line, not a blank pane, at the panel's chart row) and the kernel logged
the real discriminating marker (epi_cmd_run's serial_puts, user/epiphany.c)
rather than a screenshot coincidence. Then it clears with Esc, types a
bogus code ("aapl zz") and asserts the one-line error lands in the bar
and its own serial marker fires, with no crash either time.

Usage: tools/checks/epiphany-cmdbar-check.py   (from the repo root, after make kernel.elf)
"""
import json, os, socket, subprocess, sys, time
from PIL import Image
from freeport import free_port

LOG = "/tmp/jt-epicmdbar-serial.log"
DUMP = "/tmp/jt-epicmdbar.raw"
FB = 0xfd000000; W, H = 1920, 1080
PORT = free_port()
LOGICAL_W, LOGICAL_H, SCALE = 960, 540, 2
DOCK_ICON, DOCK_GAP, SLOT0_X = 37, 6, 247
PITCH = DOCK_ICON + DOCK_GAP
ICON_ROW_Y = 487
APPS_CLOSE_X, APPS_CLOSE_Y = 34, 56  # 2.0: Epiphany is a ring-3 window at the full-pane frame, its red light sits here
CLOSE_RED = (0xFF, 0x5F, 0x57)
EPI_IDX = 20  # GUI_LABELS index for Epiphany, same table feature-drive.py uses
# 2.0: Epiphany is a ring-3 window at the full-pane frame (x=10,y=40,
# 812x385), viewport (18,72) 796x345. GP draws a dark green line from about
# (50,341) up to (782,211) in logical pixels; the command bar's own line
# (the typed text, or the red one-line error) sits at logical y 368..384.
LINE_Y0, LINE_Y1 = 200, 350
LINE_X0, LINE_X1 = 60, 770
ERR_Y0, ERR_Y1, ERR_X0, ERR_X1 = 366, 388, 38, 300

os.chdir(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
for f in (LOG, DUMP):
    try: os.remove(f)
    except FileNotFoundError: pass

q = subprocess.Popen(["qemu-system-i386", "-kernel", "kernel.elf", "-display", "none", "-vga", "std",
                      "-qmp", f"tcp:127.0.0.1:{PORT},server,nowait", "-serial", "file:" + LOG],
                     stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
fails = []
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
    time.sleep(5.0)  # desktop up

    def move(x, y):
        cmd({"execute": "input-send-event", "arguments": {"events": [
            {"type": "abs", "data": {"axis": "x", "value": int(x * 32768 / LOGICAL_W)}},
            {"type": "abs", "data": {"axis": "y", "value": int(y * 32768 / LOGICAL_H)}}]}})
    def click():
        cmd({"execute": "input-send-event", "arguments": {"events": [{"type": "btn", "data": {"down": True, "button": "left"}}]}})
        time.sleep(0.1)
        cmd({"execute": "input-send-event", "arguments": {"events": [{"type": "btn", "data": {"down": False, "button": "left"}}]}})
    QCODE = {" ": "spc", "/": "slash", "\n": "ret"}
    def key(c):
        codes = QCODE.get(c, 'shift-' + c.lower() if c.isupper() else c).split('-')
        cmd({"execute": "send-key", "arguments": {"keys": [{"type": "qcode", "data": code} for code in codes], "hold-time": 30}})
        time.sleep(0.08)
    def keyname(qcode):
        cmd({"execute": "send-key", "arguments": {"keys": [{"type": "qcode", "data": qcode}]}})
        time.sleep(0.15)
    def type_str(s):
        for c in s: key(c)
    def dump():
        cmd({"execute": "pmemsave", "arguments": {"val": FB, "size": W * H * 4, "filename": DUMP}})
        return Image.frombytes("RGBA", (W, H), open(DUMP, "rb").read(), "raw", "BGRA").convert("RGB")
    def pixel(img, x, y): return img.getpixel((x * SCALE + 1, y * SCALE + 1))
    def is_red(p): return max(abs(p[i] - CLOSE_RED[i]) for i in range(3)) <= 12
    def window_open(): return is_red(pixel(dump(), APPS_CLOSE_X, APPS_CLOSE_Y))
    def green(p): return p[1] > p[0] + 25 and p[1] > p[2] + 25
    def red_ink(p): return p[0] > 0xA0 and p[1] < 0x60 and p[2] < 0x60
    def chart_columns(img):
        """How many logical columns hold a green pixel in the chart band: a
        drawn GP line crosses all of them, a blank pane or the watchlist
        (green only in a few percent cells) crosses almost none."""
        n = 0
        for x in range(LINE_X0, LINE_X1):
            if any(green(img.getpixel((x * SCALE + 1, y * SCALE + 1))) for y in range(LINE_Y0, LINE_Y1)): n += 1
        return n
    def error_px(img):
        return sum(1 for y in range(ERR_Y0, ERR_Y1) for x in range(ERR_X0, ERR_X1) if red_ink(img.getpixel((x * SCALE + 1, y * SCALE + 1))))

    # desktop ready
    for _ in range(120):
        if pixel(dump(), 480, 511) == (0xEF, 0xEB, 0xE4): break
        time.sleep(0.25)
    else:
        raise SystemExit("FAIL: desktop dock did not appear within 30 seconds")
    time.sleep(0.3)

    # Apps folder -> grid nav to Epiphany (index 20: 0 right, 4 down) -> Enter
    apps_centre = SLOT0_X + 0 * PITCH + DOCK_ICON // 2
    move(apps_centre, ICON_ROW_Y); time.sleep(0.3)
    click(); time.sleep(1.0)
    for _ in range(EPI_IDX % 5): keyname("d")
    for _ in range(EPI_IDX // 5): keyname("s")
    keyname("ret")
    for _ in range(40):  # poll for the red light; a loaded runner is slow to draw the ring-3 window
        time.sleep(0.25)
        if window_open(): break
    if not window_open():
        dump().save("/tmp/jt-epicmdbar-noopen.png")
        raise SystemExit("FAIL: Epiphany never opened a window")

    # Focus the bar, type "aapl gp", Enter. Wait for Epiphany to finish its
    # first draw and fetch before typing (CI runners are slower than the Mac;
    # fixed short sleeps dropped the keys there), then poll the log with a
    # deadline instead of a fixed sleep.
    time.sleep(2.5)
    keyname("slash"); time.sleep(0.5)
    type_str("aapl gp")
    keyname("ret")
    log = ""
    for _ in range(40):
        time.sleep(0.25)
        log = open(LOG, errors="replace").read() if os.path.exists(LOG) else ""
        if "epicmd=run:AAPL GP" in log:
            break
    time.sleep(0.6)
    if "epicmd=run:AAPL GP" not in log:
        fails.append(f"serial log missing 'epicmd=run:AAPL GP' marker; got tail: {log[-400:]!r}")

    img = dump(); img.save("/tmp/jt-epicmdbar-gp.png")
    cols = chart_columns(img)
    print(f"GP chart: green line in {cols} of {LINE_X1 - LINE_X0} columns")
    if cols < (LINE_X1 - LINE_X0) // 2:
        fails.append(f"GP chart shows no drawn line after 'aapl gp' + Enter ({cols} columns)")
    if not window_open():
        fails.append("Epiphany crashed or closed after running a valid GP command")

    # Esc clears, then a bogus code shows a one-line error and its own marker
    keyname("esc"); time.sleep(0.5)
    keyname("slash"); time.sleep(0.8)
    type_str("aapl zz")
    keyname("ret")
    # poll with a deadline, same as above: a fixed 0.4s missed the marker on CI
    for _ in range(40):
        time.sleep(0.25)
        log = open(LOG, errors="replace").read() if os.path.exists(LOG) else ""
        if "epicmd=unknown_code:ZZ" in log:
            break
    if "epicmd=unknown_code:ZZ" not in log:
        fails.append(f"serial log missing 'epicmd=unknown_code:ZZ' marker; got tail: {log[-400:]!r}")
    # The marker prints as the command is parsed; the error line is drawn on
    # the frame after. Poll for it rather than reading the same instant.
    for _ in range(25):
        time.sleep(0.2)
        img = dump()
        if error_px(img) >= 100: break
    img.save("/tmp/jt-epicmdbar-err.png")
    errpx = error_px(img)
    print(f"Unknown code: {errpx} red error px in the command bar")
    if errpx < 100:
        fails.append("Unknown-code error line did not draw in the command bar")
    if not window_open():
        fails.append("Epiphany crashed or closed after an unknown command code")

    if fails:
        for msg in fails: print("FAIL:", msg)
        sys.exit(1)
    print("PASS: command bar runs AAPL GP (chart drawn, real serial marker) and rejects an unknown code cleanly")

finally:
    try: cmd({"execute": "quit"})
    except (ConnectionResetError, BrokenPipeError, OSError): pass
    try: q.wait(timeout=5)
    except subprocess.TimeoutExpired: q.kill()
