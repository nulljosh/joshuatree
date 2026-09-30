#!/usr/bin/env python3
"""Curbfind runs as a real ring-3 process, the sixteenth app out of the kernel
(roadmap 2.0, 1.9.10), and the one syscall it needed refuses what it must.

Boots headless with `open=curb`, which launches Curbfind from the dock path
the moment the desktop is up. Curbfind is user/curbfind.c, a flat binary
loaded off the VFS by exec_user and run at CPL 3 through the table-driven
launcher (kernel/ring3app.c, RING3_APPS). Its live rows come through
SYS_HTTP_GET (387), the first syscall that puts the network stack behind a
ring-3 caller; headless QEMU has no NIC this kernel drives, so the fetch
comes back -ENODEV and the compiled-in samples show, the same fallback the
in-kernel copy had. The check then:

  1. asserts, off the serial log, that the program opened a window of the dock
     viewport's size (804x345), that the fetch was refused cleanly ("curbfind:
     fetch -19") and that it fell back to the ten samples with row 0 selected
     and drawn in the selected color;
  2. presses Down: "curbfind: sel 1" and the highlight moves to row 1;
  3. clicks row 3 through the real pointer: "curbfind: sel 3";
  4. presses p, the probe: the program hands SYS_HTTP_GET a relative path, a
     path with CR LF in it (a header injection), a path with a space, an
     empty path, a path over the 128-byte cap, a buffer in kernel text, a
     null buffer and a path pointer in kernel text, and every one must come
     back -EINVAL (-22) or -EFAULT (-14) with the buffer untouched;
  5. closes on Esc and asserts a clean exit 0 and a window released;
  6. asserts the desktop is back (Mail opens from the dock).

Every wait has a deadline. Discriminating: drop the '/' or printable-ASCII
rule from sys_http_get and step 4's relative/crlf/space lines change (the
call goes on to -ENODEV, -19); drop the paging_user_range_ok check on the
buffer and the kbuf/nullbuf lines change; skip the launcher row and step 1
never sees the launch.

Usage: tools/checks/ring3curbfind-check.py   (from the repo root, after make kernel.elf)
"""
import json, os, socket, subprocess, sys, time
from PIL import Image
from freeport import free_port

LOG = "/tmp/jt-ring3curbfind-serial.log"
DUMP = "/tmp/jt-ring3curbfind.raw"
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
LIST_X, ROW_Y, ROW_H = 20, 48, 28   # user/curbfind.c: CF_LIST_X, CF_TOP, CF_ROW_H
SEL_COLOR, ROW_COLOR = (0xE2, 0xD8, 0xCC), (0xF1, 0xED, 0xE7)
EINVAL, EFAULT = -22, -14
PROBES = {"relative": EINVAL, "crlf": EINVAL, "space": EINVAL, "empty": EINVAL, "long": EINVAL,
          "kbuf": EFAULT, "nullbuf": EFAULT, "kpath": EFAULT, "untouched": 1}

os.chdir(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
for f in (LOG, DUMP):
    try: os.remove(f)
    except FileNotFoundError: pass

q = subprocess.Popen(["qemu-system-i386", "-kernel", "kernel.elf", "-append", "open=curb",
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
    def row_pixel(i, img=None):
        # two pixels into the row rectangle, left of the rank text
        return pixel(VIEW_X + LIST_X + 2, VIEW_Y + ROW_Y + i * ROW_H + 2, img)

    # 1. the program is up, the fetch fell back, the samples are drawn
    if not wait_serial("ring3app: launching CURBFIND.BIN at ring 3", 40):
        fails.append("Curbfind was never launched as a ring-3 program (open=curb flag or ring3app.c broken)")
    if not wait_serial("syscall: window opened for ring-3 task", 10):
        fails.append("SYS_WINDOW_OPEN never succeeded from ring 3")
    if not wait_serial("curbfind: ring-3 window 804x345", 10):
        fails.append("the program did not report the app viewport's size (expected 804x345) through write()")
    if not wait_serial("curbfind: fetch -19", 15):
        fails.append('SYS_HTTP_GET did not come back -ENODEV ("curbfind: fetch -19") on a guest with no NIC this kernel drives')
    if not wait_serial("curbfind: samples 10", 10):
        fails.append('the program did not fall back to the ten samples ("curbfind: samples 10")')
    if not wait_serial("curbfind: sel 0", 10):
        fails.append('no "curbfind: sel 0" after the first draw')
    time.sleep(0.5)
    img = frame()
    r0, r1 = row_pixel(0, img), row_pixel(1, img)
    print(f"row 0 (selected) {r0}, row 1 {r1}")
    if not near(r0, SEL_COLOR): fails.append(f"row 0 did not draw the selected color (got {r0})")
    if not near(r1, ROW_COLOR): fails.append(f"row 1 did not draw the plain row color (got {r1})")

    # 2. Down moves the selection
    keys("down")
    if not wait_serial("curbfind: sel 1", 5):
        fails.append('pressing Down did not log "curbfind: sel 1"')
    time.sleep(0.3)
    img = frame()
    r0, r1 = row_pixel(0, img), row_pixel(1, img)
    if not (near(r1, SEL_COLOR) and near(r0, ROW_COLOR)):
        fails.append(f"the highlight did not move to row 1 after Down (row 0 {r0}, row 1 {r1})")

    # 3. a click on row 3 selects it
    move(VIEW_X + LIST_X + 100, VIEW_Y + ROW_Y + 3 * ROW_H + 10); time.sleep(0.3); click()
    if not wait_serial("curbfind: sel 3", 5):
        fails.append('clicking row 3 did not log "curbfind: sel 3"')
    time.sleep(0.3)
    r3 = row_pixel(3)
    if not near(r3, SEL_COLOR): fails.append(f"row 3 did not draw selected after the click (got {r3})")

    # 4. the probe: every bad path and pointer must be refused by the kernel
    keys("p")
    if not wait_serial("curbfind: probe untouched ", 10):
        fails.append("the p key did not run the SYS_HTTP_GET probe to its end")
    log = serial()
    for name, want in PROBES.items():
        needle = f"curbfind: probe {name} "
        i = log.find(needle)
        if i < 0: fails.append(f"probe line missing: {needle.strip()}"); continue
        got = log[i + len(needle):].split("\n", 1)[0].strip()
        print(f"probe {name}: {got} (want {want})")
        if got != str(want):
            fails.append(f"SYS_HTTP_GET probe {name} returned {got}, want {want}")

    # 5. Esc closes it cleanly
    exits = serial().count("CURBFIND.BIN exited 0")
    keys("esc")
    if not wait_serial("curbfind: closed", 5):
        fails.append("Esc did not reach the program (no closed line)")
    if not wait_serial("CURBFIND.BIN exited 0", 5, exits + 1):
        fails.append("Curbfind did not exit 0 on Esc")
    if not wait_serial("syscall: window released, task gone", 5):
        fails.append("Curbfind did not release its window on Esc")
    move(*PARK); time.sleep(0.5)
    for _ in range(100):
        if not near(pixel(CLOSE_X, CLOSE_Y), CLOSE_RED): break
        time.sleep(0.1)
    if near(pixel(CLOSE_X, CLOSE_Y), CLOSE_RED):
        fails.append("an app window is still open after Esc")

    # 6. the desktop must answer
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
    if not opened: fails.append("Mail did not open from a dock click after Curbfind closed: desktop not responsive")
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
print("PASS: Curbfind ran at ring 3 with its own window, fell back to the samples when SYS_HTTP_GET found no NIC, selected by key and click, had every bad path and pointer refused by the kernel, closed on Esc, and the desktop stayed alive")
