#!/usr/bin/env python3
"""Keyrate runs as a real ring-3 process, and crashing it does not take the
desktop with it. Step one of roadmap 2.0 ("apps leave the kernel").

Boots headless with `open=keyrate`, which launches Keyrate from the dock
path the moment the desktop is up. Keyrate is now user/keyrate.c, a flat
binary loaded off the VFS by exec_user and run at CPL 3; it gets its
window through SYS_WINDOW_OPEN and its keys through SYS_WINDOW_POLL
(docs/SYSCALL-ABI.md, v3). The check then:

  1. asserts, off the serial log, that the program opened a window of the
     app viewport's size and that the kernel saw the open come from ring 3;
  2. types a..z once each. The target text is random, but exactly one of
     those letters is its first character, so after the sweep at least one
     glyph must have turned the "typed" brown (0x884B16) in the real
     framebuffer, which was not there before. That is the program drawing
     into its framebuffer and the kernel copying it to the screen, driven
     by real keys through the poll syscall;
  3. presses the backquote, Keyrate's deliberate crash key: a write through
     a null pointer, a page fault at ring 3. Asserts the kernel reaped the
     task (idt.c's ring-3 path), released the window, and that the launcher
     logged the crash by name and came back to the desktop;
  4. proves the desktop is alive and responsive afterwards: the dock is
     on screen, Mail opens from a dock click (red close button present),
     and Esc closes it.

Discriminating: replace the null write in user/keyrate.c with a plain
jt_exit(0) and step 3 fails ("crashed (page-fault)" never logged); make
idt.c halt on a ring-3 fault the way it does for ring 0 and step 4 fails.

Usage: tools/checks/ring3app-check.py   (from the repo root, after make kernel.elf)
"""
import json, os, socket, subprocess, sys, time
from PIL import Image
from freeport import free_port

LOG = "/tmp/jt-ring3app-serial.log"
DUMP = "/tmp/jt-ring3app.raw"
FB = 0xfd000000; W, H = 1920, 1080
PORT = free_port()
LOGICAL_W, LOGICAL_H, SCALE = 960, 540, 2
DOCK_ICON, DOCK_GAP, SLOT0_X = 37, 6, 247
PITCH = DOCK_ICON + DOCK_GAP
ICON_ROW_Y = 487
CLOSE_X, CLOSE_Y = 94, 56
CLOSE_RED = (0xFF, 0x5F, 0x57)
DONE = (0x88, 0x4B, 0x16)
VIEW_X, VIEW_Y = 78, 72   # gui_launch_from_dock: viewport at (x+8, y+32) for x=70, y=40
PARK = (480, 200)

os.chdir(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
for f in (LOG, DUMP):
    try: os.remove(f)
    except FileNotFoundError: pass

q = subprocess.Popen(["qemu-system-i386", "-kernel", "kernel.elf", "-append", "open=keyrate",
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
    def done_glyph_pixels(img):
        # The word block starts 20px in and 44px down inside the viewport and
        # wraps over up to three 24px lines, all above the wpm readout (which
        # is the same brown, so it is left out). Physical coordinates.
        x0, y0 = (VIEW_X + 20) * SCALE, (VIEW_Y + 40) * SCALE
        crop = img.crop((x0, y0, x0 + 764 * SCALE, y0 + 229 * SCALE))
        data = crop.get_flattened_data() if hasattr(crop, 'get_flattened_data') else crop.getdata()
        return sum(1 for p in data if near(p, DONE, 10))

    # 1. the program is up and has its window
    if not wait_serial("ring3app: launching KEYRATE.BIN at ring 3", 40):
        fails.append("Keyrate was never launched as a ring-3 program (open=keyrate flag or ring3app.c broken)")
    if not wait_serial("syscall: window opened for ring-3 task", 10):
        fails.append("SYS_WINDOW_OPEN never succeeded from ring 3")
    if not wait_serial("keyrate: ring-3 window 804x345", 10):
        fails.append("the program did not report the app viewport's size (expected 804x345) through write()")
    time.sleep(1.0)
    before = done_glyph_pixels(frame())
    print(f"window up; typed-brown pixels before typing: {before}")
    if before:
        fails.append("typed-colour pixels present before any key was pressed")

    # 2. real keys through the poll syscall, real pixels through the framebuffer
    for k in "abcdefghijklmnopqrstuvwxyz":
        keys(k); time.sleep(0.08)
    # CI is slow: poll for the typed glyphs (up to 15s) instead of sampling once after 1s
    for _ in range(30):
        time.sleep(0.5)
        after = done_glyph_pixels(frame())
        if after: break
    print(f"typed a..z; typed-brown pixels after: {after}")
    if after == 0:
        fails.append("no glyph turned the typed colour after a..z: keys did not reach the program or its frame never reached the screen")
    if "syscall: write(1) from ring 3: keyrate: crashing" in serial():
        fails.append("the program crashed before the crash key was pressed")

    # 3. the deliberate crash, and the supervisor's answer to it
    keys("grave_accent"); time.sleep(0.2)
    if not wait_serial("keyrate: crashing on purpose", 5):
        fails.append("the crash key did not reach the program")
    if not wait_serial("exception: ring-3 task hit page-fault, reaped", 5):
        fails.append("the kernel did not reap the ring-3 task on its page fault")
    if not wait_serial("syscall: window released, task gone", 5):
        fails.append("the window was not released when the task died")
    if not wait_serial("ring3app: KEYRATE.BIN crashed (page-fault), window torn down, desktop alive", 5):
        fails.append("the launcher did not log the crash by name and return")
    if not wait_serial("autoopen: back on the desktop", 5):
        fails.append("the desktop loop was never re-entered after the crash")
    if "exception: ring-0" in serial() or "panic in" in serial():
        fails.append("the KERNEL faulted: the crash was not contained to the ring-3 task")

    # 4. the desktop is alive and takes input
    move(*PARK); time.sleep(0.5)
    dock = pixel(480, 511)
    print(f"dock tray after crash: {dock}")
    if dock != (0xEF, 0xEB, 0xE4):
        fails.append(f"desktop dock not on screen after the crash (got {dock})")
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

    # 5. a normal close, both ways, from the path qa-gallery.py takes: the
    #    Apps folder grid (Keyrate is row 1, col 4), which in 2.0 opens the app
    #    as its own ring-3 window (796x345 here, red dot at 34,56) over the
    #    folder. Esc, then the red close dot; after each the program must have
    #    exited 0, the window must be released, and Mail must open from the
    #    dock. This is the case that left 17 apps "never opened" in 1.7.7: a
    #    folder-launched viewport did not fit JT_USER_FB, SYS_WINDOW_OPEN
    #    failed, and the desktop hung.
    APPS_CLOSE_X, APPS_CLOSE_Y = 80, 46            # the Apps window's own red dot
    GRID_APP_CLOSE_X, GRID_APP_CLOSE_Y = 34, 56    # a window launched from the grid sits further left than a dock launch
    def open_keyrate_from_grid(tag):
        seen = serial().count("keyrate: ring-3 window")
        move(*PARK); time.sleep(0.2)
        move(SLOT0_X + DOCK_ICON // 2, ICON_ROW_Y); time.sleep(0.3); click(); time.sleep(1.0)
        for _ in range(4): keys("d"); time.sleep(0.35)  # the grid moves on wasd, as qa-gallery.py does
        keys("s"); time.sleep(0.35)
        keys("ret")
        for _ in range(60):
            time.sleep(0.1)
            if serial().count("keyrate: ring-3 window") > seen: break
        else:
            fails.append(f"{tag}: Keyrate did not open a ring-3 window from the Apps folder grid"); return False
        last = [l for l in serial().splitlines() if "keyrate: ring-3 window " in l][-1]
        if not last.endswith("keyrate: ring-3 window 796x345"):
            fails.append(f"{tag}: the folder-launched window is not the 2.0 app viewport's 796x345 (got: {last})")
        if "ring3app: BUG" in serial():
            fails.append(f"{tag}: ring3app logged a BUG line")
        time.sleep(0.5)
        return True
    def assert_closed(tag, exits_before):
        if not wait_serial("syscall: window released, task gone", 5) or serial().count("KEYRATE.BIN exited 0") <= exits_before:
            fails.append(f"{tag}: Keyrate did not exit 0 and release its window on a normal close")
        keys("esc"); time.sleep(0.8)  # the Apps folder itself
        move(*PARK); time.sleep(0.3)
        if near(pixel(CLOSE_X, CLOSE_Y), CLOSE_RED) or near(pixel(APPS_CLOSE_X, APPS_CLOSE_Y), CLOSE_RED):
            fails.append(f"{tag}: a window is still open after the close")
        move(SLOT0_X + 2 * PITCH + DOCK_ICON // 2, ICON_ROW_Y); time.sleep(0.3); click()
        ok = False
        for _ in range(40):
            time.sleep(0.1)
            if near(pixel(CLOSE_X, CLOSE_Y), CLOSE_RED): ok = True; break
        print(f"{tag}: Mail opens from the dock afterwards: {'yes' if ok else 'NO'}")
        if not ok: fails.append(f"{tag}: Mail did not open from a dock click after the close: desktop stuck")
        keys("esc"); time.sleep(1.0)
    exits = serial().count("KEYRATE.BIN exited 0")
    if open_keyrate_from_grid("esc-close"):
        keys("esc"); time.sleep(0.5)
        assert_closed("esc-close", exits)
    exits = serial().count("KEYRATE.BIN exited 0")
    if open_keyrate_from_grid("dot-close"):
        move(GRID_APP_CLOSE_X, GRID_APP_CLOSE_Y); time.sleep(0.3); click(); time.sleep(0.5)
        assert_closed("dot-close", exits)

    # 6. the keyboard path into the Apps folder, not the dock click: Enter on
    #    a bare desktop used to call gui_launch_apps() directly, skipping the
    #    gui_launch_from_dock windowed setup, so gui_app_view_size() returned
    #    0 and Keyrate's SYS_WINDOW_OPEN failed ENODEV before it drew a thing
    #    (1.7.7 desktop-hang root cause). Open the folder with Enter, launch
    #    Keyrate, confirm it got a real window, back out with two Esc, then
    #    confirm the desktop still answers a dock click.
    seen = serial().count("keyrate: ring-3 window")
    move(*PARK); time.sleep(0.3)
    keys("ret"); time.sleep(1.0)  # bare desktop -> Apps folder, by keyboard
    for _ in range(4): keys("d"); time.sleep(0.35)
    keys("s"); time.sleep(0.35)
    keys("ret")  # launch Keyrate from the grid selection
    for _ in range(60):
        time.sleep(0.1)
        if serial().count("keyrate: ring-3 window") > seen: break
    else:
        fails.append("keyboard-open: Keyrate did not open a ring-3 window after Enter opened the Apps folder by keyboard")
    if "ring3app: BUG" in serial():
        fails.append("keyboard-open: ring3app logged a BUG line launching Keyrate from a keyboard-opened Apps folder")
    keys("esc"); time.sleep(0.5)  # closes Keyrate
    keys("esc"); time.sleep(0.5)  # closes the Apps folder
    move(*PARK); time.sleep(0.3)
    if near(pixel(CLOSE_X, CLOSE_Y), CLOSE_RED) or near(pixel(APPS_CLOSE_X, APPS_CLOSE_Y), CLOSE_RED):
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
print("PASS: Keyrate ran at ring 3 with its own window, took keys, crashed on demand, closed normally both ways from the Apps folder, and the desktop stayed alive")
