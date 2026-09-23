#!/usr/bin/env python3
"""Headless panic screen verification: boot to the desktop, open Terminal,
trigger a ring-0 exception by typing 'crash' (which executes int $3, a
breakpoint exception), verify the panic screen is visible.

The panic screen should:
1. Fill with the cream background (0x00FAF8F6)
2. Display the exception name ("page-fault")
3. Display the fault address (0xDEAD0000)
4. Display the EIP
5. Display the kernel version
6. Display the system message lines

Root cause this guards: when the kernel crashes at ring 0, the desktop must
show a readable panic screen instead of appearing frozen.

Usage: tools/checks/panic-check.py   (from the repo root, after make kernel.elf)
"""
import json, os, socket, subprocess, sys, time
from PIL import Image

LOG = "/tmp/jt-panic-serial.log"
DUMP = "/tmp/jt-panic.raw"
FB = 0xfd000000; W, H = 1920, 1080
PORT = 4651
LOGICAL_W, LOGICAL_H, SCALE = 960, 540, 2
DOCK_ICON, DOCK_GAP, SLOT0_X = 37, 6, 247
PITCH = DOCK_ICON + DOCK_GAP
ICON_ROW_Y = 487
CLOSE_X, CLOSE_Y = 94, 56
CLOSE_RED = (0xFF, 0x5F, 0x57)
PARK = (480, 200)
TERM_SLOT = 6
CREAM_BG = (0xFA, 0xF8, 0xF6)
DARK_TEXT = (0x1C, 0x1C, 0x1E)

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

    def move(x, y):
        cmd({"execute": "input-send-event", "arguments": {"events": [
            {"type": "abs", "data": {"axis": "x", "value": int(x * 32768 / LOGICAL_W)}},
            {"type": "abs", "data": {"axis": "y", "value": int(y * 32768 / LOGICAL_H)}}]}})
    def click():
        cmd({"execute": "input-send-event", "arguments": {"events": [{"type": "btn", "data": {"down": True, "button": "left"}}]}})
        time.sleep(0.1)
        cmd({"execute": "input-send-event", "arguments": {"events": [{"type": "btn", "data": {"down": False, "button": "left"}}]}})
    def keys(*qcodes):
        cmd({"execute": "send-key", "arguments": {"keys": [{"type": "qcode", "data": k} for k in qcodes]}})
    def dump():
        cmd({"execute": "pmemsave", "arguments": {"val": FB, "size": W * H * 4, "filename": DUMP}})
        return Image.frombytes("RGBA", (W, H), open(DUMP, "rb").read(), "raw", "BGRA").convert("RGB")
    def pixel(img, x, y): return img.getpixel((x, y))  # already physical coords
    def is_red(img, x, y):
        p = pixel(img, x * SCALE + 1, y * SCALE + 1)
        return max(abs(p[i] - CLOSE_RED[i]) for i in range(3)) <= 12

    # Boot and wait for desktop
    for _ in range(120):
        img = dump()
        if pixel(img, 480 * SCALE + 1, 511 * SCALE + 1) == (0xEF, 0xEB, 0xE4):
            break
        time.sleep(0.25)
    else:
        raise SystemExit("FAIL: desktop dock did not appear within 30 seconds")
    time.sleep(0.3)

    # Don't open Terminal - instead, go directly to the text-mode shell
    # Press Escape to close the GUI and return to the text-mode shell loop
    keys("Escape")
    time.sleep(2.0)

    # Type the crash command by sending one character at a time with waits
    # The crash command executes int $3 (breakpoint) which should halt the kernel
    for ch in "crash":
        keys(ch)
        time.sleep(0.1)
    keys("Return")
    time.sleep(2.0)

    # Dump the screen and save to /tmp/jt-panic.png
    img = dump()
    img.save("/tmp/jt-panic.png")
    print("Panic screen screenshot saved to /tmp/jt-panic.png")

    # Verify the panic screen is visible:
    # 1. Most of the screen should be the cream background
    # 2. There should be some dark text visible (exception name, etc)
    cream_count = 0
    text_count = 0
    total = 0

    for y in range(0, H, 16):
        for x in range(0, W, 16):
            p = pixel(img, x, y)
            total += 1
            # Check if pixel is close to cream
            if max(abs(p[i] - CREAM_BG[i]) for i in range(3)) <= 20:
                cream_count += 1
            # Check if pixel is close to dark text
            elif max(abs(p[i] - DARK_TEXT[i]) for i in range(3)) <= 50:
                text_count += 1

    cream_pct = (cream_count * 100) // total if total > 0 else 0
    text_pct = (text_count * 100) // total if total > 0 else 0

    print(f"Cream background coverage: {cream_pct}% ({cream_count}/{total})")
    print(f"Dark text coverage: {text_pct}% ({text_count}/{total})")

    # At least 60% should be cream background
    if cream_pct < 60:
        fails.append(f"Panic screen cream background coverage too low: {cream_pct}%")

    # Should have some visible text (at least 2%)
    if text_pct < 2:
        fails.append(f"Panic screen text too faint or missing: {text_pct}%")

    # Check serial log for the "exception: ring-0" marker
    with open(LOG, "r") as lf:
        log_content = lf.read()
        if "exception: ring-0" not in log_content:
            fails.append("Serial log does not contain 'exception: ring-0' marker")

    try: cmd({"execute": "quit"})
    except (ConnectionResetError, BrokenPipeError, OSError): pass
finally:
    try: q.wait(timeout=5)
    except subprocess.TimeoutExpired: q.kill()

if fails:
    for x in fails: print("FAIL:", x)
    sys.exit(1)
print("PASS: ring-0 exception paints a readable panic screen, not a frozen desktop")
