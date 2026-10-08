#!/usr/bin/env python3
"""Headless proof of the app switcher (roadmap.md: "an app switcher, a
screenshot key", modern-OS desktop basics). Opens two of the real
concurrent windows (Files and Notes, both wired to gui_multiwin_open --
see kernel/kernel.c's gui_multiwin_supported), sends Ctrl+Tab (kernel.c
treats Alt and Ctrl the same for this hotkey, since QEMU/v86's Alt
delivery to the guest is the less reliable one to bet on), and checks two
independent things actually changed:

  1. Serial markers: SWITCHER:tab when the panel opens/cycles, then
     SWITCHER:focus when Ctrl releases and the kernel calls
     gui_multiwin_focus -- proof the hotkey's own code path ran, not just
     that a keystroke was sent.
  2. Pixels: window 0 (Files, rect x70,y40,w820,h385) and window 1
     (Notes, opened second so it starts on top, rect x130,y100,w820,h385)
     overlap in x130..890,y100..425. Cropping that whole rect before and
     after the switch and counting changed pixels (real app content --
     icon grid vs. notes list/row band -- differs across many of them, not
     just one hand-picked spot) proves the actual z-order flipped, not
     just that the marker fired with no real effect.

Usage: tools/checks/appswitcher-check.py   (from the repo root, after make kernel.elf)
"""
import json, os, socket, subprocess, sys, time, tempfile

REPO = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..")
os.chdir(REPO)
ARTIFACTS = tempfile.mkdtemp(prefix="jt-appswitcher-")
LOG = os.path.join(ARTIFACTS, "serial.log")
DUMP = os.path.join(ARTIFACTS, "framebuffer.raw")
FB = 0xfd000000; W, H = 1920, 1080
SOCKET = os.path.join(ARTIFACTS, "qmp.sock")
LOGICAL_W, LOGICAL_H, SCALE = 960, 540, 2
DOCK_ICON, DOCK_GAP, SLOT0_X = 37, 6, 247
PITCH = DOCK_ICON + DOCK_GAP
ICON_ROW_Y = 487
FILES_SLOT, NOTES_SLOT = 1, 4  # GUI_DOCK_DEFAULT: slot0 is the Apps folder, then Files(icon0), Mail, Calendar, Notes(icon3) in order; 1.9.20 gave Notes compositor hooks so it replaces Weather as the second window
CLOSE_X, CLOSE_Y = 94, 56
CLOSE_RED = (0xFF, 0x5F, 0x57)
PARK = (480, 200)

for f in (LOG, DUMP):
    try: os.remove(f)
    except FileNotFoundError: pass

fails = []
q = subprocess.Popen(["qemu-system-i386", "-kernel", "kernel.elf", "-display", "none", "-vga", "std",
                      "-qmp", f"unix:{SOCKET},server,nowait", "-serial", "file:" + LOG],
                     stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
try:
    s = None
    for _ in range(50):
        time.sleep(0.2)
        candidate = socket.socket(socket.AF_UNIX)
        try:
            candidate.connect(SOCKET)
            candidate.settimeout(10)
            s = candidate
            break
        except OSError:
            candidate.close()
    if s is None: raise SystemExit("FAIL: QEMU's QMP socket never came up")
    f = s.makefile("rw")
    def cmd(o):
        f.write(json.dumps(o) + "\n"); f.flush()
        while True:
            line = f.readline()
            if not line:
                if o['execute'] == 'quit': return {}
                raise ConnectionError('QEMU disconnected before replying')
            r = json.loads(line)
            if "error" in r: raise RuntimeError(r["error"])
            if "return" in r: return r
    f.readline()
    cmd({"execute": "qmp_capabilities"})

    def wait_desktop_up(deadline=20.0, quiet_for=0.35):
        start = time.time(); last_count = -1; last_change = start
        while time.time() - start < deadline:
            try:
                count = open(LOG, "rb").read().count(b"present\r\n")
            except FileNotFoundError:
                count = 0
            now = time.time()
            if count != last_count:
                last_count = count; last_change = now
            elif count > 0 and now - last_change >= quiet_for:
                return
            time.sleep(0.05)
    wait_desktop_up()

    def move(x, y):
        cmd({"execute": "input-send-event", "arguments": {"events": [
            {"type": "abs", "data": {"axis": "x", "value": int(x * 32768 / LOGICAL_W)}},
            {"type": "abs", "data": {"axis": "y", "value": int(y * 32768 / LOGICAL_H)}}]}})
    def click():
        cmd({"execute": "input-send-event", "arguments": {"events": [{"type": "btn", "data": {"down": True, "button": "left"}}]}})
        time.sleep(0.1)
        cmd({"execute": "input-send-event", "arguments": {"events": [{"type": "btn", "data": {"down": False, "button": "left"}}]}})
    def framebuffer():
        cmd({"execute": "pmemsave", "arguments": {"val": FB, "size": W * H * 4, "filename": DUMP}})
        from PIL import Image
        return Image.frombytes("RGBA", (W, H), open(DUMP, "rb").read(), "raw", "BGRA").convert("RGB")
    def pixel(x, y):
        return framebuffer().getpixel((x * SCALE + 1, y * SCALE + 1))
    def overlap_region():
        """A logical-pixel crop of the whole Files/Notes overlap rect
        (x130..890,y100..425), not one guessed sample point: real app
        content (icon grid vs. notes list/row band) differs across many
        pixels in there, so counting how many actually changed proves a
        real content swap even if this or that single sampled point
        happens to land on shared cream background in both apps."""
        img = framebuffer()
        x0, y0, x1, y1 = 130 * SCALE, 100 * SCALE, 890 * SCALE, 425 * SCALE
        return img.crop((x0, y0, x1, y1))
    def is_red(p): return max(abs(p[i] - CLOSE_RED[i]) for i in range(3)) <= 12
    def window_open(): return is_red(pixel(CLOSE_X, CLOSE_Y))
    centre = lambda slot: SLOT0_X + slot * PITCH + DOCK_ICON // 2

    def keys(*qcodes):
        cmd({"execute": "send-key", "arguments": {"keys": [{"type": "qcode", "data": k} for k in qcodes], "hold-time": 30}})

    def poll(cond, timeout=30.0, step=0.1):
        """Re-check cond() until truthy or the deadline passes (shared
        runners are slow and jittery, so no fixed sleeps)."""
        end = time.time() + timeout
        while True:
            v = cond()
            if v or time.time() >= end: return v
            time.sleep(step)
    def settled(timeout=30.0):
        """Wait until two consecutive overlap captures are identical (the
        desktop finished repainting), then return that capture."""
        prev = [overlap_region()]
        def same():
            cur = overlap_region(); ok = cur.tobytes() == prev[0].tobytes(); prev[0] = cur; return ok
        poll(same, timeout, 0.2)
        return prev[0]
    def open_slot(slot, ready):
        move(centre(slot), ICON_ROW_Y); time.sleep(0.3)
        base = overlap_region()
        click()
        return poll(lambda: ready(base))  # true once the new window has painted

    def serial_text():
        with open(LOG, "rb") as fh:
            return fh.read().decode("latin1")
    def wait_marker(marker, timeout=15.0):
        deadline = time.time() + timeout
        while time.time() < deadline:
            log = serial_text()
            if marker in log: return log
            time.sleep(0.05)
        return serial_text()

    move(*PARK); time.sleep(0.5)

    if not open_slot(FILES_SLOT, lambda b: window_open()):
        fails.append("Files: dock click did not open a window")
    elif not open_slot(NOTES_SLOT, lambda b: overlap_region().tobytes() != b.tobytes()):
        fails.append("Notes: dock click did not open a second window")

    if not fails:
        before = settled()
        keys("ctrl", "tab")
        log = wait_marker("SWITCHER:tab", 30.0)
        if "SWITCHER:tab" not in log:
            fails.append("Ctrl+Tab: SWITCHER:tab marker not found -- the switcher hotkey never fired")
        log = wait_marker("SWITCHER:focus", 30.0)
        if "SWITCHER:focus" not in log:
            fails.append("Ctrl+Tab release: SWITCHER:focus marker not found -- focus was never committed")
        else:
            def ndiff():
                a = overlap_region()
                return sum(1 for x, y in zip(before.getdata(), a.getdata()) if x != y)
            poll(lambda: ndiff() >= 200)  # repaint can lag the serial marker
            diff = ndiff()
            if diff < 200:
                fails.append(f"Alt/Ctrl+Tab: only {diff} pixels changed across the whole Files/Notes overlap rect -- the focused window's z-order never actually flipped")
            else:
                print(f"App switcher: Ctrl+Tab cycled Files/Notes (serial-verified) and {diff} pixels in the overlap rect changed, proving real z-order focus change")

    cmd({"execute": "quit"})
except Exception as e:
    fails.append(f"exception: {e}")
finally:
    try: q.wait(timeout=5)
    except Exception: q.kill()

if fails:
    print("FAIL:")
    for msg in fails: print("  - " + msg)
    print(f"artifacts: {ARTIFACTS}")
    sys.exit(1)
print("PASS: Alt/Ctrl+Tab cycles open windows and focuses the highlighted one (serial marker + real pixel z-order change)")
