#!/usr/bin/env python3
"""Headless proof of the screenshot hotkey (roadmap.md: "an app switcher,
a screenshot key", modern-OS desktop basics). Sends Ctrl+Shift+3 from the
idle desktop (real PrintScreen's E0-prefixed make sequence collides with
arrow-key extended sequences other in-window readers are already
mid-decoding -- see kernel/kernel.c's own comment at the hotkey -- so
Ctrl+Shift+3 is the one this ships) and checks:

  1. Serial marker: "SHOT:<name>:<bytes>\\n", written by kernel.c the
     moment gui_screenshot_save() actually wrote the file -- the exact
     filename and byte count the kernel itself computed, not a guess.
  2. The byte count is sane for an uncompressed 24-bit BMP of the real
     screen size: 54-byte header plus width*3 (padded to 4) per row,
     times height -- proof this is a real full-framebuffer dump, not an
     empty or truncated file.
  3. A second Ctrl+Shift+3 produces the next filename in sequence
     (SHOT0001.BMP, then SHOT0002.BMP), proving the naming genuinely
     avoids overwriting the first shot.

Usage: tools/checks/screenshot-check.py   (from the repo root, after make kernel.elf)
"""
import json, os, re, socket, subprocess, sys, time, tempfile

REPO = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..")
os.chdir(REPO)
ARTIFACTS = tempfile.mkdtemp(prefix="jt-screenshot-")
LOG = os.path.join(ARTIFACTS, "serial.log")
SOCKET = os.path.join(ARTIFACTS, "qmp.sock")
LOGICAL_W, LOGICAL_H, WIN_SCALE = 960, 540, 2  # window_open(800,600,32)'s logical size and the fixed 2x window_scale() every real dock-launched app runs at

try: os.remove(LOG)
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

    def keys(*qcodes):
        cmd({"execute": "send-key", "arguments": {"keys": [{"type": "qcode", "data": k} for k in qcodes], "hold-time": 30}})
        time.sleep(0.2)
    def screenshot_hotkey():
        keys("ctrl", "shift", "3")

    def serial_text():
        with open(LOG, "rb") as fh:
            return fh.read().decode("latin1")
    def wait_marker(pattern, timeout=15.0):
        deadline = time.time() + timeout
        while time.time() < deadline:
            m = re.search(pattern, serial_text())
            if m: return m
            time.sleep(0.05)
        return None

    w, h = LOGICAL_W * WIN_SCALE, LOGICAL_H * WIN_SCALE
    row_bytes = w * 3
    pad = (4 - (row_bytes % 4)) % 4
    expected_bytes = 54 + (row_bytes + pad) * h

    screenshot_hotkey()
    m1 = wait_marker(r"SHOT:(SHOT0001\.BMP):(\d+)")
    if not m1:
        fails.append("Ctrl+Shift+3: no SHOT:SHOT0001.BMP:<bytes> marker in serial log -- hotkey never saved a file")
    else:
        name1, bytes1 = m1.group(1), int(m1.group(2))
        if bytes1 != expected_bytes:
            fails.append(f"SHOT0001.BMP: kernel reported {bytes1} bytes, expected {expected_bytes} for a real {w}x{h} 24-bit BMP")
        else:
            print(f"Screenshot: Ctrl+Shift+3 saved {name1}, {bytes1} bytes (serial-verified, matches a real {w}x{h} 24-bit BMP exactly)")

        screenshot_hotkey()
        m2 = wait_marker(r"SHOT:(SHOT0002\.BMP):(\d+)")
        if not m2:
            fails.append("A second Ctrl+Shift+3: no SHOT:SHOT0002.BMP marker -- next-filename numbering did not advance (would overwrite the first shot)")
        else:
            print(f"Screenshot: a second Ctrl+Shift+3 saved {m2.group(1)} (numbering advances, no overwrite)")

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
print("PASS: Ctrl+Shift+3 saves a real, correctly-sized framebuffer BMP under an auto-incrementing SHOTNNNN.BMP name")
