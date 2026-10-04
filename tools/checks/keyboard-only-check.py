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
from freeport import free_port

LOG = "/tmp/jt-keyboard-only-serial.log"
DUMP = "/tmp/jt-keyboard-only.raw"
FB = 0xfd000000; W, H = 1920, 1080
PORT = free_port()
LOGICAL_W, LOGICAL_H, SCALE = 960, 540, 2
# Red close light of an open window, in logical pixels. 2.0 apps are ring-3
# compositor windows; the Apps folder under them keeps its own light at (80, 46).
# A launch from the bare desktop lands at window 0's frame (x=70), whose light
# is at (94, 56); a launch over the dock-opened folder lands at (10,40), light
# at (34, 56). Either one counts as the app being up.
CLOSE_SPOTS = ((94, 56), (34, 56))
FOLDER_CLOSE_X, FOLDER_CLOSE_Y = 80, 46
CLOSE_RED = (0xFF, 0x5F, 0x57)

# App names from kernel/kernel.c APPS[].name (indices 0-23, then Apps folder, then Trash)
APPS = ["Burrow", "Mail", "Calendar", "Notes", "Reminders", "Terminal", "Samantha", "Weather",
        "Curbfind", "Keyrate", "Bookrank", "Quotes", "Tonchi", "Toroid", "Hikko",
        "Fieldbook", "Contacts", "Calculator", "Stocks", "Search", "Epiphany",
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

    def red_at(img, x, y):
        p = img.getpixel((x * SCALE + 1, y * SCALE + 1))
        return max(abs(p[i] - CLOSE_RED[i]) for i in range(3)) <= 12

    def window_open():
        img = dump()
        return any(red_at(img, x, y) for x, y in CLOSE_SPOTS)

    # Wait for desktop to be ready: dock tray color at (480, 511) = 0xEFEBE4
    for _ in range(120):
        if pixel(480, 511) == (0xEF, 0xEB, 0xE4):
            break
        time.sleep(0.25)
    else:
        raise SystemExit("FAIL: desktop dock did not appear within 30 seconds")
    time.sleep(0.3)

    # Process all 24 grid apps via keyboard-only navigation
    for app_idx in range(23):
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

            # Close the window by Esc: its red light must be gone afterwards.
            # A lost scancode gets exactly one retry.
            key("esc")
            for attempt in range(2):
                for _ in range(20):
                    time.sleep(0.2)
                    if not window_open(): break
                if not window_open() or attempt: break
                key("esc")
            if opened and window_open():
                fails.append(f"app {app_idx} ({app_name}) did not close on Esc")
            # The Apps folder is a window too; if the launch left it open,
            # Esc closes it. On a bare desktop a second Esc would quit the
            # GUI to the text shell, so only send it while its light shows.
            time.sleep(0.5)
            if red_at(dump(), FOLDER_CLOSE_X, FOLDER_CLOSE_Y):
                key("esc")
                time.sleep(0.5)

        except Exception as e:
            results.append((app_idx, app_name, False))
            print(f"{app_idx:2d} {app_name:15s} ERROR: {e}")
            fails.append(f"app {app_idx} ({app_name}) exception: {e}")
            # Try to recover
            try:
                if window_open(): key("esc")
                time.sleep(0.3)
                if red_at(dump(), FOLDER_CLOSE_X, FOLDER_CLOSE_Y): key("esc")
                time.sleep(0.3)
            except Exception:
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
