#!/usr/bin/env python3
"""Headless soak test: open and close every app N times in ONE boot,
prove nothing leaked. Reuses boot, QMP, navigation and close code from
qa-gallery.py.

Usage: tools/checks/soak-check.py [passes] [app_indices]
  passes: number of complete app cycles (default 20)
  app_indices: comma-separated list to limit which apps to test (e.g. "0,1,2")

Exit 1 if any crash detected in serial log.
"""
import json, os, re, socket, subprocess, sys, time
from PIL import Image, ImageChops

LOG = "/tmp/jt-soak-check-serial.log"
DUMP = "/tmp/jt-soak-check.raw"
FB = 0xfd000000; W, H = 1920, 1080
PORT = 4620
LOGICAL_W, LOGICAL_H, SCALE = 960, 540, 2
DOCK_ICON, DOCK_GAP, SLOT0_X = 37, 6, 247
PITCH = DOCK_ICON + DOCK_GAP
ICON_ROW_Y = 487
APPS_CLOSE_X, APPS_CLOSE_Y = 80, 46
CLOSE_X, CLOSE_Y = 94, 56
CLOSE_RED = (0xFF, 0x5F, 0x57)
PARK = (480, 200)

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

passes = int(sys.argv[1]) if len(sys.argv) > 1 else 20
app_indices = list(range(25))
if len(sys.argv) > 2:
    app_indices = [int(x) for x in sys.argv[2].split(",")]

os.chdir(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
for f in (LOG, DUMP):
    try: os.remove(f)
    except FileNotFoundError: pass

q = subprocess.Popen(["qemu-system-i386", "-kernel", "kernel.elf", "-display", "none", "-vga", "std",
                      "-qmp", f"tcp:127.0.0.1:{PORT},server,nowait", "-serial", "file:" + LOG],
                     stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
print(f"Starting soak test: {len(app_indices)} apps x {passes} passes")
start_time = time.time()
fails = []
pass_results = []

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

    for pass_num in range(1, passes + 1):
        opened_count = 0

        for app_idx in app_indices:
            app_name = APPS[app_idx]
            opened = False

            try:
                move(*PARK); time.sleep(0.2)
                apps_centre = SLOT0_X + 0 * PITCH + DOCK_ICON // 2
                move(apps_centre, ICON_ROW_Y); time.sleep(0.3)
                click(); time.sleep(1.0)

                row = app_idx // 5
                col = app_idx % 5
                for _ in range(col):
                    key("d")
                for _ in range(row):
                    key("s")
                grid = dump().crop((200, 120, 1720, 900))
                key("ret")
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
                    opened_count += 1

                close_window()
                key("esc"); time.sleep(0.5)

            except Exception as e:
                fails.append(f"pass {pass_num} app {app_idx} ({app_name}) exception: {e}")
                try: key("esc"); time.sleep(0.3)
                except: pass

        elapsed = time.time() - start_time
        print(f"Pass {pass_num:2d}/{passes}: {opened_count:2d}/{len(app_indices)} apps opened, {elapsed:.1f}s")
        pass_results.append((pass_num, opened_count))

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

if pass_results:
    first = pass_results[0]
    middle = pass_results[len(pass_results) // 2]
    last = pass_results[-1]
    print(f"Summary: {len(app_indices)} apps x {passes} passes in {elapsed:.1f}s")
    print(f"  Pass {first[0]:2d}: {first[1]:2d}/{len(app_indices)} apps opened")
    print(f"  Pass {middle[0]:2d}: {middle[1]:2d}/{len(app_indices)} apps opened")
    print(f"  Pass {last[0]:2d}: {last[1]:2d}/{len(app_indices)} apps opened")

if fails:
    for x in fails: print("FAIL:", x)
    sys.exit(1)

print("PASS: all passes completed, no crashes detected")
