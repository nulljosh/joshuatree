#!/usr/bin/env python3
"""Sparkjar runs as a real ring-3 process, the fourteenth app out of the kernel
(roadmap 2.0, 1.9.8).

Boots headless with `open=spar`, which launches Sparkjar from the dock path the
moment the desktop is up. Sparkjar is user/sparkjar.c, a flat binary loaded off
the VFS by exec_user and run at CPL 3 through the table-driven launcher
(kernel/ring3app.c, RING3_APPS). Votes are session-only, so it needs no file
and no new syscall. The check then:

  1. asserts, off the serial log, that the program opened a window of the dock
     viewport's size (804x345) and drew the ranked list: row 0 is the selected
     color, row 1 is plain, and the top idea's name is inked;
  2. presses Down: "sparkjar: sel 1" and the highlight moves to row 1;
  3. presses u six times on that idea (19 votes, behind the 24 of the top
     one): each press logs its new count, and only the sixth (25 > 24) may
     re-sort, so "sparkjar: pos 0" must appear and the highlight must sit on
     row 0 with the same idea under it;
  4. closes on Esc and asserts a clean exit 0 and a window released;
  5. closes nothing else: the desktop must answer (Mail opens).

Every wait has a deadline. Discriminating: drop the sort call in the u handler
and step 3 never sees pos 0; make Down a no-op and step 2 fails.

Usage: tools/checks/ring3sparkjar-check.py   (from the repo root, after make kernel.elf)
"""
import json, os, socket, subprocess, sys, time
from PIL import Image
from freeport import free_port

LOG = "/tmp/jt-ring3sparkjar-serial.log"
DUMP = "/tmp/jt-ring3sparkjar.raw"
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
ROW_Y, ROW_H = 48, 28     # user/sparkjar.c: first row's top y, row pitch
PROBE_X = 200             # inside the row, right of the name and left of the vote count
SEL_COLOR, BG_COLOR = (0xE2, 0xD8, 0xCC), (0xF1, 0xED, 0xE7)

os.chdir(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
for f in (LOG, DUMP):
    try: os.remove(f)
    except FileNotFoundError: pass

q = subprocess.Popen(["qemu-system-i386", "-kernel", "kernel.elf", "-append", "open=spar",
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
    def typed(word):
        for ch in word: keys({".": "dot"}.get(ch, ch))
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
        return pixel(VIEW_X + PROBE_X, VIEW_Y + ROW_Y + 2 + i * ROW_H, img)
    def row_ink(i):
        img = frame(); x0, y0 = (VIEW_X + 50) * SCALE, (VIEW_Y + ROW_Y + 6 + i * ROW_H) * SCALE
        crop = img.crop((x0, y0, x0 + 110 * SCALE, y0 + 16 * SCALE)).tobytes()
        return sum(1 for k in range(0, len(crop), 3) if crop[k] < 0x60 and crop[k + 1] < 0x60 and crop[k + 2] < 0x60)

    # 1. the program is up and drew the ranked list
    if not wait_serial("ring3app: launching SPARKJAR.BIN at ring 3", 40):
        fails.append("Sparkjar was never launched as a ring-3 program (open=spar flag or ring3app.c broken)")
    if not wait_serial("syscall: window opened for ring-3 task", 10):
        fails.append("SYS_WINDOW_OPEN never succeeded from ring 3")
    if not wait_serial("sparkjar: ring-3 window 804x345", 10):
        fails.append("the program did not report the app viewport's size (expected 804x345) through write()")
    if not wait_serial("sparkjar: sel 0", 10):
        fails.append('the program did not log its first selection ("sparkjar: sel 0")')
    time.sleep(0.5)
    r0, r1 = row_pixel(0), row_pixel(1)
    print(f"row 0 (selected) {r0}, row 1 {r1}")
    if not near(r0, SEL_COLOR):
        fails.append(f"row 0 did not draw the selected color (got {r0}, expected {SEL_COLOR})")
    if not near(r1, BG_COLOR):
        fails.append(f"row 1 is not the plain row color (got {r1})")
    ink0 = row_ink(0)
    print(f"row 0 name ink: {ink0} pixels")
    if ink0 < 20: fails.append(f"the top idea's name was not drawn (only {ink0} dark pixels)")

    # 2. Down moves the selection
    keys("down")
    if not wait_serial("sparkjar: sel 1", 5):
        fails.append('pressing Down did not log "sparkjar: sel 1"')
    time.sleep(0.3)
    r0, r1 = row_pixel(0), row_pixel(1)
    if not (near(r1, SEL_COLOR) and near(r0, BG_COLOR)):
        fails.append(f"the highlight did not move to row 1 after Down (row 0 {r0}, row 1 {r1})")

    # 3. six upvotes take idea 1 from 19 to 25, past the 24 above it: the sixth re-sorts
    for n in range(20, 25):
        keys("u")
        if not wait_serial(f"sparkjar: vote 1 {n}", 5):
            fails.append(f'upvote to {n} did not log "sparkjar: vote 1 {n}"')
    if "sparkjar: pos 0" in serial():
        fails.append("the list re-sorted before the idea passed the one above it (pos 0 too early)")
    keys("u")
    if not wait_serial("sparkjar: vote 1 25", 5):
        fails.append('the sixth upvote did not log "sparkjar: vote 1 25"')
    if not wait_serial("sparkjar: pos 0", 5):
        fails.append("passing the top idea did not re-sort the list (no pos 0 line)")
    time.sleep(0.4)
    r0, r1 = row_pixel(0), row_pixel(1)
    if not (near(r0, SEL_COLOR) and near(r1, BG_COLOR)):
        fails.append(f"the highlight did not follow the upvoted idea to row 0 (row 0 {r0}, row 1 {r1})")

    # 4. Esc closes it cleanly
    keys("esc")
    if not wait_serial("sparkjar: closed", 5):
        fails.append("Esc did not reach the program (no closed line)")
    if not wait_serial("SPARKJAR.BIN exited 0", 5):
        fails.append("Sparkjar did not exit 0 on Esc")
    if not wait_serial("syscall: window released, task gone", 5):
        fails.append("Sparkjar did not release its window on Esc")
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
    if not opened: fails.append("Mail did not open from a dock click after Sparkjar closed: desktop not responsive")
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
print("PASS: Sparkjar ran at ring 3 with its own window, moved the selection, re-sorted on the upvote that passed the leader, closed on Esc, and the desktop stayed alive")
