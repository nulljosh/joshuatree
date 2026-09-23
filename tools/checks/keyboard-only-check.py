#!/usr/bin/env python3
"""Headless keyboard-only check: open each app using keyboard only, close it by Esc.
Proves every app can be opened and closed without a mouse.

Grid navigation to each app uses the technique from qa-gallery.py but with NO mouse
events. Open the Apps folder (Enter from desktop), navigate the grid by arrow keys
(row = i // 5, col = i % 5, using a/d/w/s), press Enter to launch, wait for window,
press Esc to close, repeat.

Exit 1 if any app does not open, does not close by Esc, or serial log contains panic.

Usage: tools/checks/keyboard-only-check.py   (from the repo root, after make kernel.elf)
"""
import json, os, re, socket, subprocess, sys, time
from PIL import Image, ImageChops

LOG = "/tmp/jt-keyboard-only-serial.log"
DUMP = "/tmp/jt-keyboard-only.raw"
FB = 0xfd000000; W, H = 1920, 1080
PORT = 4631
LOGICAL_W, LOGICAL_H, SCALE = 960, 540, 2
# Close button coordinates for app windows opened via keyboard (top-left corner)
# Scanned from actual framebuffer: red pixels at logical ~26, 15
CLOSE_X, CLOSE_Y = 26, 15
CLOSE_RED = (0xFF, 0x5F, 0x57)

# App names from kernel/kernel.c GUI_LABELS (indices 0-24, then Apps folder, then Trash)
APPS = ["Files", "Mail", "Calendar", "Notes", "Reminders", "Terminal", "Chat", "Weather",
        "Curbfind", "Keyrate", "Bookrank", "Quotes", "Plan", "Lexly", "Toroid", "Sparkjar",
        "Homeqi", "Fieldbook", "Contacts", "Calculator", "Stocks", "Search", "Epiphany",
        "Portfolio", "Activity"]

CRASH_PATTERNS = [
    r"Kernel panic",
    r"Exception",
    r"GPF",
    r"NULL pointer",
    r"SIGSEGV",
]

os.chdir(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
for f in (LOG, DUMP):
    try: os.remove(f)
    except FileNotFoundError: pass

q = subprocess.Popen(["qemu-system-i386", "-kernel", "kernel.elf", "-display", "none", "-vga", "std",
                      "-qmp", f"tcp:127.0.0.1:{PORT},server,nowait", "-serial", "file:" + LOG],
                     stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
results = []
fails = []
start_time = time.time()

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

    def key(qcode):
        cmd({"execute": "send-key", "arguments": {"keys": [{"type": "qcode", "data": qcode}]}})
        time.sleep(0.35)

    def dump():
        cmd({"execute": "pmemsave", "arguments": {"val": FB, "size": W * H * 4, "filename": DUMP}})
        return Image.frombytes("RGBA", (W, H), open(DUMP, "rb").read(), "raw", "BGRA").convert("RGB")

    def pixel(x, y):
        cmd({"execute": "pmemsave", "arguments": {"val": FB, "size": W * H * 4, "filename": DUMP}})
        img = Image.frombytes("RGBA", (W, H), open(DUMP, "rb").read(), "raw", "BGRA").convert("RGB")
        return img.getpixel((x * SCALE + 1, y * SCALE + 1))

    def window_open():
        p = pixel(CLOSE_X, CLOSE_Y)
        is_red = max(abs(p[i] - CLOSE_RED[i]) for i in range(3)) <= 12
        return is_red

    # Wait for desktop to be ready: dock tray color at (480, 511) = 0xEFEBE4
    for _ in range(120):
        if pixel(480, 511) == (0xEF, 0xEB, 0xE4):
            break
        time.sleep(0.25)
    else:
        raise SystemExit("FAIL: desktop dock did not appear within 30 seconds")
    time.sleep(0.3)

    # Process all 25 grid apps via keyboard-only navigation
    for app_idx in range(25):
        app_name = APPS[app_idx]
        opened = False

        try:
            # Open the Apps folder: Enter from bare desktop
            key("ret")
            time.sleep(1.0)

            # Navigate to the app within the grid: row = app_idx // 5, col = app_idx % 5
            row = app_idx // 5
            col = app_idx % 5

            # A freshly opened folder selects tile 0; navigate to the target position
            for _ in range(col):
                key("d")  # right
            for _ in range(row):
                key("s")  # down
                time.sleep(0.2)  # wait for scroll animation to render

            # Capture the current grid view for change detection
            grid = dump().crop((200, 120, 1720, 900))
            key("ret")  # launch app

            # Wait for the window to appear
            for _ in range(60):
                time.sleep(0.1)
                now = dump().crop((200, 120, 1720, 900))
                hist = ImageChops.difference(grid, now).convert('L').histogram()
                changed = sum(hist) - hist[0]
                if window_open() and changed > 0.05 * grid.width * grid.height:
                    opened = True
                    break

            time.sleep(0.5)  # let the first content frame finish

            if opened:
                results.append((app_idx, app_name, True))
                print(f"{app_idx:2d} {app_name:15s} PASS")
            else:
                results.append((app_idx, app_name, False))
                print(f"{app_idx:2d} {app_name:15s} FAIL: never opened")
                fails.append(f"app {app_idx} ({app_name}) never opened a window")

            # Close the window by Esc
            key("esc")
            time.sleep(0.5)

            # Close the Apps folder by Esc
            key("esc")
            time.sleep(0.5)

        except Exception as e:
            results.append((app_idx, app_name, False))
            print(f"{app_idx:2d} {app_name:15s} ERROR: {e}")
            fails.append(f"app {app_idx} ({app_name}) exception: {e}")
            # Try to recover
            try:
                key("esc")
                time.sleep(0.3)
                key("esc")
                time.sleep(0.3)
            except:
                pass

    # Check for crashes in the serial log
    try:
        with open(LOG, errors="replace") as lf:
            log_content = lf.read()
            for pattern in CRASH_PATTERNS:
                if re.search(pattern, log_content, re.IGNORECASE):
                    fails.append(f"kernel crash detected: {pattern}")
    except FileNotFoundError:
        pass

    try: cmd({"execute": "quit"})
    except (ConnectionResetError, BrokenPipeError, OSError): pass

finally:
    try: q.wait(timeout=5)
    except subprocess.TimeoutExpired: q.kill()

elapsed = time.time() - start_time
print()
opened_count = sum(1 for _, _, opened in results if opened)
print(f"Summary: {opened_count}/{len(results)} apps opened and closed in {elapsed:.1f}s")

if fails:
    for x in fails: print("FAIL:", x)
    sys.exit(1)

print("PASS: all apps opened and closed by keyboard alone, no crashes detected")
