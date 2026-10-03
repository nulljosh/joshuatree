#!/usr/bin/env python3
"""Reminders runs as a real ring-3 process, the fifteenth app out of the kernel
(roadmap 2.0, 1.9.9).

Boots headless with `open=remi`, which launches Reminders from the dock path
the moment the desktop is up. Reminders is user/reminders.c, a flat binary
loaded off the VFS by exec_user and run at CPL 3 through the table-driven
launcher (kernel/ring3app.c, RING3_APPS). It keeps REMINDERS.TXT through the
ordinary file calls, no new syscall. The check then:

  1. asserts, off the serial log, that the program opened a window of the dock
     viewport's size (804x345), found no file and drew an empty list;
  2. presses a, types "milk" through the real prompt (the "remindersprompt"
     marker must grow once per keystroke), adds "eggs" the same way, and
     waits for "reminders: count 2" plus "reminders: saved 2": the adds hit
     REMINDERS.TXT, and both items are on screen with row 0 selected;
  3. moves down (highlight moves) and ticks the second item off with space
     ("reminders: done 1", another write);
  4. closes on Esc and asserts a clean exit 0 and a window released;
  5. reopens Reminders through the Apps folder: "reminders: loaded 2" and
     "reminders: checked 1" prove a fresh process read the items and the
     tick back off the file;
  6. deletes the ticked item ("reminders: count 1", "saved 1"), closes, opens
     a third time and asserts "loaded 1" and "checked 0";
  7. closes the folder and asserts the desktop is back (Mail opens).

Every wait has a deadline. Discriminating: skip the save in toggle_sel and
step 5's "checked 1" fails; skip it in delete_sel and step 6 fails.

Usage: tools/checks/ring3reminders-check.py   (from the repo root, after make kernel.elf)
"""
import json, os, socket, subprocess, sys, time
from PIL import Image
from freeport import free_port

LOG = "/tmp/jt-ring3reminders-serial.log"
DUMP = "/tmp/jt-ring3reminders.raw"
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
ROW_Y, ROW_H = 54, 22     # user/reminders.c: first row's text y, row pitch
PROBE_X = 600             # right of the phone column: only the highlight or the page color
SEL_COLOR, BG_COLOR = (0xED, 0xE6, 0xDC), (0xFA, 0xF8, 0xF6)

os.chdir(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
for f in (LOG, DUMP):
    try: os.remove(f)
    except FileNotFoundError: pass

q = subprocess.Popen(["qemu-system-i386", "-kernel", "kernel.elf", "-append", "open=remi",
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
        return pixel(VIEW_X + PROBE_X, VIEW_Y + ROW_Y - 4 + i * ROW_H + 8, img)
    def row_ink(i):
        img = frame(); x0, y0 = (VIEW_X + 28) * SCALE, (VIEW_Y + ROW_Y + i * ROW_H) * SCALE
        crop = img.crop((x0, y0, x0 + 120 * SCALE, y0 + 16 * SCALE)).tobytes()
        return sum(1 for k in range(0, len(crop), 3) if crop[k] < 0x60 and crop[k + 1] < 0x60 and crop[k + 2] < 0x60)

    # 1. the program is up, found no file and drew the empty state
    if not wait_serial("ring3app: launching REMINDERS.BIN at ring 3", 40):
        fails.append("Reminders was never launched as a ring-3 program (open=remi flag or ring3app.c broken)")
    if not wait_serial("syscall: window opened for ring-3 task", 10):
        fails.append("SYS_WINDOW_OPEN never succeeded from ring 3")
    if not wait_serial("reminders: ring-3 window 804x345", 10):
        fails.append("the program did not report the app viewport's size (expected 804x345) through write()")
    if not wait_serial("reminders: loaded 0", 10):
        fails.append("expected \"reminders: loaded 0\" on a disk with no REMINDERS.TXT")
    time.sleep(0.5)
    r0 = row_pixel(0)
    print(f"row 0 with an empty list {r0}")
    if near(r0, SEL_COLOR):
        fails.append("an empty list drew a selected row")

    # 2. add two items through the real prompt
    keys("a")
    if not wait_serial("remindersprompt\n", 5):
        fails.append("pressing a did not open the add prompt (no remindersprompt marker)")
    p0 = serial().count("remindersprompt\n")
    typed("milk")
    if not wait_serial("remindersprompt\n", 5, p0 + 4):
        fails.append("typing four letters did not redraw the prompt once per keystroke")
    keys("ret")
    if not wait_serial("reminders: count 1", 5):
        fails.append('the add did not log "reminders: count 1"')
    if not wait_serial("reminders: saved 1", 5):
        fails.append('the add did not write REMINDERS.TXT ("reminders: saved 1")')
    keys("a"); typed("eggs"); keys("ret")
    if not wait_serial("reminders: count 2", 5):
        fails.append('the second add did not log "reminders: count 2"')
    if not wait_serial("reminders: saved 2", 5):
        fails.append('the second add did not write REMINDERS.TXT ("reminders: saved 2")')
    time.sleep(0.4)
    r0, r1 = row_pixel(0), row_pixel(1)
    print(f"row 0 (selected) {r0}, row 1 {r1}")
    if not near(r0, SEL_COLOR): fails.append(f"row 0 did not draw the selected color (got {r0})")
    if not near(r1, BG_COLOR): fails.append(f"row 1 is not the page color (got {r1})")
    ink0, ink1 = row_ink(0), row_ink(1)
    print(f"row ink after the adds: {ink0} / {ink1} pixels")
    if ink0 < 20 or ink1 < 20: fails.append(f"the new items are not both on screen (ink {ink0} / {ink1})")

    # 3. move down and tick the second item off
    keys("down")
    if not wait_serial("reminders: sel 1", 5):
        fails.append('pressing Down did not log "reminders: sel 1"')
    time.sleep(0.3)
    r0, r1 = row_pixel(0), row_pixel(1)
    if not (near(r1, SEL_COLOR) and near(r0, BG_COLOR)):
        fails.append(f"the highlight did not move to row 1 after Down (row 0 {r0}, row 1 {r1})")
    keys("spc")
    if not wait_serial("reminders: done 1", 5):
        fails.append('space did not log "reminders: done 1"')
    if not wait_serial("reminders: saved 2", 5, 2):
        fails.append("the toggle did not write REMINDERS.TXT")

    # 4. Esc closes it cleanly
    exits = serial().count("REMINDERS.BIN exited 0")
    keys("esc")
    if not wait_serial("reminders: closed", 5):
        fails.append("Esc did not reach the program (no closed line)")
    if not wait_serial("REMINDERS.BIN exited 0", 5, exits + 1):
        fails.append("Reminders did not exit 0 on Esc")
    if not wait_serial("syscall: window released, task gone", 5):
        fails.append("Reminders did not release its window on Esc")
    move(*PARK); time.sleep(0.5)
    for _ in range(100):
        if not near(pixel(CLOSE_X, CLOSE_Y), CLOSE_RED): break
        time.sleep(0.1)
    if near(pixel(CLOSE_X, CLOSE_Y), CLOSE_RED):
        fails.append("an app window is still open after Esc")

    # 5. reopen through the Apps folder: a fresh process must read REMINDERS.TXT back
    def open_from_folder():
        n = serial().count("appsfullrepaint")
        move(SLOT0_X + DOCK_ICON // 2, ICON_ROW_Y); time.sleep(0.3); click()
        if not wait_serial("appsfullrepaint", 10, n + 1):
            fails.append("the Apps folder did not open from the dock"); return False
        time.sleep(0.6)
        for c in ("d", "d", "d", "d"): keys(c)
        keys("ret")
        return True
    launches = serial().count("ring3app: launching REMINDERS.BIN")
    if open_from_folder():
        if not wait_serial("ring3app: launching REMINDERS.BIN", 10, launches + 1):
            fails.append("Reminders did not launch from the Apps folder grid (icon 4)")
        if not wait_serial("reminders: loaded 2", 10):
            fails.append('the reopened app did not report "reminders: loaded 2": the adds did not persist in REMINDERS.TXT')
        if not wait_serial("reminders: checked 1", 10):
            fails.append('the reopened app did not report "reminders: checked 1": the toggle did not persist')
        time.sleep(0.5)

        # 6. delete the ticked item, close, open a third time
        keys("down")
        if not wait_serial("reminders: sel 1", 5, 2):
            fails.append("Down did not move the selection in the reopened app")
        keys("d")
        if not wait_serial("reminders: count 1", 5, 2):
            fails.append('deleting an item did not log "reminders: count 1"')
        if not wait_serial("reminders: saved 1", 5, 2):
            fails.append('the delete did not write REMINDERS.TXT ("reminders: saved 1")')
        keys("esc"); time.sleep(0.8)   # 2.0: closing a folder-launched app returns to the desktop, not the folder
        open_from_folder()
        if not wait_serial("reminders: loaded 1", 10):
            fails.append('the third open did not report "reminders: loaded 1": the delete did not persist')
        if not wait_serial("reminders: checked 0", 10, 2):
            fails.append('the third open did not report "reminders: checked 0": the ticked item should be the one deleted')
        time.sleep(0.4)
        keys("esc"); time.sleep(0.8)

    # 7. close the folder; the desktop must answer
    keys("esc"); time.sleep(0.8)
    if "exception: ring-0" in serial() or "panic in" in serial() or "ring3app: BUG" in serial():
        fails.append("the kernel faulted or ring3app logged a BUG line")
    move(*PARK); time.sleep(0.5)
    dock = pixel(480, 511)
    if dock != (0xEF, 0xEB, 0xE4):
        fails.append(f"desktop dock not on screen after the close (got {dock})")
    move(SLOT0_X + 2 * PITCH + DOCK_ICON // 2, ICON_ROW_Y); time.sleep(0.3); click()
    opened = False
    for _ in range(40):
        time.sleep(0.1)
        if near(pixel(CLOSE_X, CLOSE_Y), CLOSE_RED): opened = True; break
    print(f"Mail opens from the dock after the close: {'yes' if opened else 'NO'}")
    if not opened: fails.append("Mail did not open from a dock click after Reminders closed: desktop not responsive")
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
print("PASS: Reminders ran at ring 3 with its own window, added, ticked and deleted items through the real prompt, kept REMINDERS.TXT across three fresh runs, closed on Esc, and the desktop stayed alive")
