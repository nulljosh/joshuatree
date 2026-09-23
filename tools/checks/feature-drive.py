#!/usr/bin/env python3
"""Headless feature-drive: every app's main action, headless.

Opens each app and performs its MAIN ACTION (not just opens it), saves a PNG
after the action, closes it, and fails on any crash. The point is exercising
each feature once.

Per-app actions:
- Notes: type "soak" then Ctrl+S (saves to disk)
- Reminders: press a, type "milk", Enter (adds a reminder)
- Calendar: press right (next tab, Week view)
- Terminal: type "help" Enter
- Chat: type "hi" Enter (offline, just proving no crash)
- Search: type "read" (filters the file list)
- Calculator: type "2+2" Enter
- Stocks: press right (next tab)
- Epiphany: press right (next tab)
- Contacts: press a then Esc (add prompt then cancel)
- Lexly: press 1 (pick first answer)
- Quotes: press 1
- Homeqi: press 1
- Sparkjar: press u (upvote)
- Toroid: press space (pause/unpause)
- Keyrate: type "the"
- Bookrank, Fieldbook, Plan, Curbfind, Portfolio, Activity: press down twice
- Files, Mail, Weather, Trash: open only (already covered by gallery)

Before/after pixel comparison with PIL ImageChops to detect screen changes.

Usage: tools/checks/feature-drive.py [outdir]   (from the repo root, after make kernel.elf)
"""
import json, os, re, socket, subprocess, sys, time
from PIL import Image, ImageChops

LOG = "/tmp/jt-feature-drive-serial.log"
DUMP = "/tmp/jt-feature-drive.raw"
FB = 0xfd000000; W, H = 1920, 1080
PORT = 4621  # unique port for this script
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

# Apps to test with actions: (index, name, action_keys_or_string)
# action_keys: list of qcode strings or special commands like ("type", "text")
ACTIONS = [
    (3, "Notes", [("type", "soak"), "ctrl-s"]),
    (4, "Reminders", ["a", ("type", "milk"), "ret"]),
    (2, "Calendar", ["right"]),
    (5, "Terminal", [("type", "help"), "ret"]),
    (6, "Chat", [("type", "hi"), "ret"]),
    (21, "Search", [("type", "read")]),
    (19, "Calculator", [("type", "2+2"), "ret"]),
    (20, "Stocks", ["right"]),
    (22, "Epiphany", ["right"]),
    (18, "Contacts", ["a", "esc"]),
    (13, "Lexly", ["1"]),
    (11, "Quotes", ["1"]),
    (16, "Homeqi", ["1"]),
    (15, "Sparkjar", ["u"]),
    (14, "Toroid", ["space"]),
    (9, "Keyrate", [("type", "the")]),
    (10, "Bookrank", ["down", "down"]),
    (17, "Fieldbook", ["down", "down"]),
    (12, "Plan", ["down", "down"]),
    (8, "Curbfind", ["down", "down"]),
    (23, "Portfolio", ["down", "down"]),
    (24, "Activity", ["down", "down"]),
    (0, "Files", []),  # open only
    (1, "Mail", []),   # open only
    (7, "Weather", []),  # open only
    (26, "Trash", []),  # open only
]

CRASH_PATTERNS = [
    r"Kernel panic",
    r"Exception",
    r"GPF",
    r"NULL pointer",
    r"SIGSEGV",
]

outdir = sys.argv[1] if len(sys.argv) > 1 else "/tmp/jt-feature-drive"
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
    def type_text(text):
        """Type a text string character by character."""
        for c in text:
            if c.isupper():
                qcode = "shift-" + c.lower()
            elif c == ' ':
                qcode = "spc"
            elif c == '.':
                qcode = "dot"
            elif c == ',':
                qcode = "comma"
            else:
                qcode = c.lower()
            key(qcode)
    def dump():
        cmd({"execute": "pmemsave", "arguments": {"val": FB, "size": W * H * 4, "filename": DUMP}})
        return Image.frombytes("RGBA", (W, H), open(DUMP, "rb").read(), "raw", "BGRA").convert("RGB")
    def pixel(x, y):
        cmd({"execute": "pmemsave", "arguments": {"val": FB, "size": W * H * 4, "filename": DUMP}})
        img = Image.frombytes("RGBA", (W, H), open(DUMP, "rb").read(), "raw", "BGRA").convert("RGB")
        return img.getpixel((x * SCALE + 1, y * SCALE + 1))
    def window_open():
        p1 = pixel(CLOSE_X, CLOSE_Y)
        p2 = pixel(APPS_CLOSE_X, APPS_CLOSE_Y)
        is_red_1 = max(abs(p1[i] - CLOSE_RED[i]) for i in range(3)) <= 12
        is_red_2 = max(abs(p2[i] - CLOSE_RED[i]) for i in range(3)) <= 12
        return is_red_1 or is_red_2

    # Wait for desktop to be ready
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

    # Process each action
    for app_idx, app_name, action_keys in ACTIONS:
        opened = False
        action_changed = False
        png_path = None

        try:
            # Open the Apps folder if not already open
            move(*PARK); time.sleep(0.2)
            apps_centre = SLOT0_X + 0 * PITCH + DOCK_ICON // 2
            move(apps_centre, ICON_ROW_Y); time.sleep(0.3)
            click(); time.sleep(1.0)

            # Navigate to the app within the grid
            row = app_idx // 5
            col = app_idx % 5
            for _ in range(col):
                key("d")  # right
            for _ in range(row):
                key("s")  # down

            # Capture the grid state before launch
            grid = dump().crop((200, 120, 1720, 900))
            key("ret")  # launch app

            # Wait for window to open
            for _ in range(60):
                time.sleep(0.1)
                now = dump().crop((200, 120, 1720, 900))
                hist = ImageChops.difference(grid, now).convert('L').histogram()
                changed = sum(hist) - hist[0]
                if window_open() and changed > 0.05 * grid.width * grid.height:
                    opened = True
                    break
            time.sleep(0.5)

            if opened:
                # Capture before-action state
                before_action = dump().crop((200, 120, 1720, 900))

                # Perform the action
                if action_keys:
                    for item in action_keys:
                        if isinstance(item, tuple) and item[0] == "type":
                            type_text(item[1])
                        else:
                            key(item)
                    time.sleep(0.5)  # wait for action to settle

                # Capture after-action state
                after_action = dump().crop((200, 120, 1720, 900))

                # Check if the screen changed
                hist = ImageChops.difference(before_action, after_action).convert('L').histogram()
                diff_pixels = sum(hist) - hist[0]
                action_changed = diff_pixels > 0.02 * before_action.width * before_action.height

                # Dump framebuffer and save PNG
                img = dump()
                png_path = os.path.join(outdir, f"{app_idx:02d}-{app_name}-action.png")
                img.save(png_path)

                action_str = "yes" if action_changed else "NO"
                results.append((app_idx, app_name, True, action_changed, png_path))
                print(f"{app_idx:2d} {app_name:15s} opened yes    action {action_str:3s}  {png_path}")
            else:
                results.append((app_idx, app_name, False, False, None))
                print(f"{app_idx:2d} {app_name:15s} opened NO")
                fails.append(f"app {app_idx} ({app_name}) never opened a window")

            # Close the window
            close_window()
            # Close the Apps folder
            key("esc"); time.sleep(0.5)

        except Exception as e:
            results.append((app_idx, app_name, False, False, None))
            print(f"{app_idx:2d} {app_name:15s} ERROR: {e}")
            fails.append(f"app {app_idx} ({app_name}) exception: {e}")
            try: key("esc"); time.sleep(0.3)
            except: pass

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
opened_count = sum(1 for _, _, opened, _, _ in results if opened)
action_count = sum(1 for _, _, opened, changed, _ in results if opened and changed)
print(f"Summary: {opened_count}/{len(results)} apps opened in {elapsed:.1f}s, {action_count} actions changed the screen")
print(f"Output: {outdir}")

if fails:
    for x in fails: print("FAIL:", x)
    sys.exit(1)

print("PASS: every app opened and action executed, no crashes detected")
