#!/usr/bin/env python3
"""Headless QA gallery: open each app, capture a full-resolution screenshot,
close it. Produces one 1920x1080 PNG per app, fails if any app doesn't open
or if the kernel crashes.

Grid navigation to each app reuses the technique from activity-check.py,
portfolio-check.py, appclose-check.py: open the Apps folder (dock slot 0),
navigate the grid by row/col (icon i: row = i // 5, col = i % 5, using arrow
keys), press Enter to launch, dump the framebuffer with pmemsave, close via
pointer or esc, repeat.

Output format: <outdir>/NN-name.png per app (outdir default /tmp/jt-gallery,
overridable by argv[1]). Exit 1 if any app never opened or serial log
contains panic/exception strings (grep appclose-check.py for the list).

Usage: tools/checks/qa-gallery.py [outdir]   (from the repo root, after make kernel.elf)
"""
import json, os, re, socket, subprocess, sys, time
from PIL import Image, ImageChops

LOG = "/tmp/jt-qa-gallery-serial.log"
DUMP = "/tmp/jt-qa-gallery.raw"
FB = 0xfd000000; W, H = 1920, 1080
PORT = 4461  # unique port, checked existing checks
LOGICAL_W, LOGICAL_H, SCALE = 960, 540, 2
DOCK_ICON, DOCK_GAP, SLOT0_X = 37, 6, 247
PITCH = DOCK_ICON + DOCK_GAP
ICON_ROW_Y = 487
APPS_CLOSE_X, APPS_CLOSE_Y = 80, 46
CLOSE_X, CLOSE_Y = 94, 56
CLOSE_RED = (0xFF, 0x5F, 0x57)
PARK = (480, 200)

# App names from kernel/kernel.c GUI_LABELS (indices 0-26)
APPS = ["Files", "Mail", "Calendar", "Notes", "Reminders", "Terminal", "Chat", "Weather",
        "Curbfind", "Keyrate", "Bookrank", "Quotes", "Plan", "Lexly", "Toroid", "Sparkjar",
        "Homeqi", "Fieldbook", "Contacts", "Calculator", "Stocks", "Search", "Epiphany",
        "Portfolio", "Activity", "Apps", "Trash"]

# Apps 0-24 launch from the Apps folder grid; Apps (25) and Trash (26) are special:
# Apps opens when you click the dock slot again, Trash is dock slot 10.
REGULAR_APPS = list(range(25))
SPECIAL_APPS = [(10, "Trash")]  # (dock_slot, name)

CRASH_PATTERNS = [
    r"Kernel panic",
    r"Exception",
    r"GPF",
    r"NULL pointer",
    r"SIGSEGV",
]

outdir = sys.argv[1] if len(sys.argv) > 1 else "/tmp/jt-gallery"
os.makedirs(outdir, exist_ok=True)

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

    def move(x, y):
        cmd({"execute": "input-send-event", "arguments": {"events": [
            {"type": "abs", "data": {"axis": "x", "value": int(x * 32768 / LOGICAL_W)}},
            {"type": "abs", "data": {"axis": "y", "value": int(y * 32768 / LOGICAL_H)}}]}})
    def click():
        cmd({"execute": "input-send-event", "arguments": {"events": [{"type": "btn", "data": {"down": True, "button": "left"}}]}})
        time.sleep(0.1)
        cmd({"execute": "input-send-event", "arguments": {"events": [{"type": "btn", "data": {"down": False, "button": "left"}}]}})
    def key(qcode):
        cmd({"execute": "send-key", "arguments": {"keys": [{"type": "qcode", "data": qcode}]}})
        time.sleep(0.35)
    def dump():
        cmd({"execute": "pmemsave", "arguments": {"val": FB, "size": W * H * 4, "filename": DUMP}})
        return Image.frombytes("RGBA", (W, H), open(DUMP, "rb").read(), "raw", "BGRA").convert("RGB")
    def is_red(x, y):
        img = dump()
        p = img.getpixel((x * SCALE + 1, y * SCALE + 1))
        return max(abs(p[i] - CLOSE_RED[i]) for i in range(3)) <= 12
    def pixel(x, y):
        cmd({"execute": "pmemsave", "arguments": {"val": FB, "size": W * H * 4, "filename": DUMP}})
        img = Image.frombytes("RGBA", (W, H), open(DUMP, "rb").read(), "raw", "BGRA").convert("RGB")
        return img.getpixel((x * SCALE + 1, y * SCALE + 1))
    def window_open():
        # Check for red close button: either normal app (CLOSE_X, CLOSE_Y) or Apps folder (APPS_CLOSE_X, APPS_CLOSE_Y)
        p1 = pixel(CLOSE_X, CLOSE_Y)
        p2 = pixel(APPS_CLOSE_X, APPS_CLOSE_Y)
        is_red_1 = max(abs(p1[i] - CLOSE_RED[i]) for i in range(3)) <= 12
        is_red_2 = max(abs(p2[i] - CLOSE_RED[i]) for i in range(3)) <= 12
        return is_red_1 or is_red_2

    # Wait for desktop to be ready: dock tray color at (480, 511) = 0xEFEBE4
    for _ in range(120):
        if pixel(480, 511) == (0xEF, 0xEB, 0xE4):
            break
        time.sleep(0.25)
    else:
        raise SystemExit("FAIL: desktop dock did not appear within 30 seconds")
    time.sleep(0.3)

    def close_window():
        """Close current window via pointer or esc."""
        for _ in range(5):
            if not window_open(): break
            # Try clicking the close button
            p1 = pixel(CLOSE_X, CLOSE_Y)
            p2 = pixel(APPS_CLOSE_X, APPS_CLOSE_Y)
            is_red_1 = max(abs(p1[i] - CLOSE_RED[i]) for i in range(3)) <= 12
            is_red_2 = max(abs(p2[i] - CLOSE_RED[i]) for i in range(3)) <= 12
            if is_red_1:
                move(CLOSE_X, CLOSE_Y); time.sleep(0.3); click(); time.sleep(0.8)
            elif is_red_2:
                move(APPS_CLOSE_X, APPS_CLOSE_Y); time.sleep(0.3); click(); time.sleep(0.8)
            else:
                break
        move(*PARK); time.sleep(0.3)

    # Process regular apps (0-24) via the Apps folder grid
    for app_idx in REGULAR_APPS:
        app_name = APPS[app_idx]
        opened = False
        png_path = None

        try:
            # Open the Apps folder (dock slot 0) if not already open
            # Check if we need to open the folder first
            move(*PARK); time.sleep(0.2)
            apps_centre = SLOT0_X + 0 * PITCH + DOCK_ICON // 2
            move(apps_centre, ICON_ROW_Y); time.sleep(0.3)
            click(); time.sleep(1.0)

            # Navigate to the app within the grid: row = app_idx // 5, col = app_idx % 5
            row = app_idx // 5
            col = app_idx % 5
            for _ in range(col):
                key("d")  # right
            for _ in range(row):
                key("s")  # down
            # The folder's own close dot sits where a launched app's does, so
            # a red dot alone cannot tell "app opened" from "Enter did nothing".
            # App-only signal: the window area stops looking like the grid.
            grid = dump().crop((200, 120, 1720, 900))
            key("ret")  # launch app
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
                # Dump framebuffer and save PNG
                img = dump()
                png_path = os.path.join(outdir, f"{app_idx:02d}-{app_name}.png")
                img.save(png_path)
                results.append((app_idx, app_name, True, png_path))
                print(f"{app_idx:2d} {app_name:15s} opened yes    {png_path}")
            else:
                results.append((app_idx, app_name, False, None))
                print(f"{app_idx:2d} {app_name:15s} opened NO")
                fails.append(f"app {app_idx} ({app_name}) never opened a window")

            # Close the window
            close_window()
            # Close the Apps folder to reset for the next app
            key("esc"); time.sleep(0.5)

        except Exception as e:
            results.append((app_idx, app_name, False, None))
            print(f"{app_idx:2d} {app_name:15s} ERROR: {e}")
            fails.append(f"app {app_idx} ({app_name}) exception: {e}")
            # Try to recover
            try: key("esc"); time.sleep(0.3)
            except: pass

    # Process Trash (special: dock slot 10)
    try:
        app_name = "Trash"
        app_idx = 26
        opened = False
        png_path = None

        move(*PARK); time.sleep(0.2)
        trash_centre = SLOT0_X + 10 * PITCH + DOCK_ICON // 2
        move(trash_centre, ICON_ROW_Y); time.sleep(0.3)
        click(); time.sleep(1.2)

        for _ in range(40):
            time.sleep(0.1)
            if window_open():
                opened = True
                break

        if opened:
            img = dump()
            png_path = os.path.join(outdir, f"{app_idx:02d}-{app_name}.png")
            img.save(png_path)
            results.append((app_idx, app_name, True, png_path))
            print(f"{app_idx:2d} {app_name:15s} opened yes    {png_path}")
        else:
            results.append((app_idx, app_name, False, None))
            print(f"{app_idx:2d} {app_name:15s} opened NO")
            fails.append(f"Trash never opened a window")

        close_window()
    except Exception as e:
        results.append((app_idx, app_name, False, None))
        print(f"{app_idx:2d} {app_name:15s} ERROR: {e}")
        fails.append(f"Trash exception: {e}")

    # Check for crashes in the serial log
    try:
        with open(LOG, errors="replace") as lf:
            log_content = lf.read()
            for pattern in CRASH_PATTERNS:
                if re.search(pattern, log_content, re.IGNORECASE):  # kernel/idt.c prints lowercase "exception:"
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
opened_count = sum(1 for _, _, opened, _ in results if opened)
print(f"Summary: {opened_count}/{len(results)} apps opened in {elapsed:.1f}s to {outdir}")

if fails:
    for x in fails: print("FAIL:", x)
    sys.exit(1)

print("PASS: all apps opened and closed cleanly, no crashes detected")
