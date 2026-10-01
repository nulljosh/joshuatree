#!/usr/bin/env python3
"""Weather runs as a real ring-3 process, the twentieth app out of the
kernel (roadmap 2.0, 1.9.22).

Boots headless with no NIC and `open=weat`, which launches Weather from the
dock path the moment the desktop is up. Weather is user/weather.c, a flat
binary run at CPL 3 through the table-driven launcher (kernel/ring3app.c,
RING3_APPS). The kernel's weather_fetch still owns the network and leaves
WEATHER.TXT for the app; with no NIC the file says "offline", so the window
must show the honest offline face over labelled sample data. The check:

  1. asserts the launch, SYS_WINDOW_OPEN, the app's "wxwin=offline sample"
     and the five sample forecast days, and that the window is the cream
     Weather surface;
  2. presses R: the app draws "Fetching..." (wxwin=fetching), exits 7, the
     kernel fetches exactly once more and starts Weather again, which shows
     the offline face again;
  3. closes on Esc with a clean exit 0 and a released window, and asserts
     the desktop is back (Mail opens).

Every wait has a deadline. Discriminating: drop the RING3_APPS row and step 1
never sees the launch; stop the kernel writing WEATHER.TXT and the face
reads "Not fetched yet" with no wxwin=offline; drop the exit-7 retry and
step 2 sees no second fetch.
(tools/checks/weather-app-check.sh covers the live, stale, bad and timeout faces.)

Usage: tools/checks/ring3weather-check.py   (from the repo root, after make kernel.elf)
"""
import json, os, socket, subprocess, sys, time
from PIL import Image
from freeport import free_port

LOG = "/tmp/jt-ring3weather-serial.log"
DUMP = "/tmp/jt-ring3weather.raw"
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

os.chdir(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
for f in (LOG, DUMP):
    try: os.remove(f)
    except FileNotFoundError: pass

q = subprocess.Popen(["qemu-system-i386", "-kernel", "kernel.elf", "-append", "open=weat",
                      "-nic", "none", "-display", "none", "-vga", "std",
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
    # 1. the program is up and shows the offline face over labelled sample data
    if not wait_serial("ring3app: launching WEATHER.BIN at ring 3", 40):
        fails.append("Weather was never launched as a ring-3 program (open=weat flag or ring3app.c broken)")
    if not wait_serial("syscall: window opened for ring-3 task", 10):
        fails.append("SYS_WINDOW_OPEN never succeeded from ring 3")
    if not wait_serial("wxwin=offline sample", 15):
        fails.append("the app did not read the kernel's WEATHER.TXT and show the offline face (wxwin=offline sample)")
    if not wait_serial("wxrow=5 Mon,Tue,Wed,Thu,Fri facts=yes", 5):
        fails.append("the sample face did not draw its five labelled forecast days")
    time.sleep(0.5)
    bg = pixel(VIEW_X + 5, VIEW_Y + 5)
    print(f"surface pixel: {bg}")
    if not near(bg, (0xF5, 0xF0, 0xEB), 6): fails.append(f"the window is not Weather's cream surface (got {bg})")

    # 2. R: Fetching..., one more kernel fetch, a fresh run of the app
    fetches, launches = serial().count("wxfetch"), serial().count("launching WEATHER.BIN")
    keys("r")
    if not wait_serial("wxwin=fetching", 10): fails.append("R did not show the fetching state")
    if not wait_serial("launching WEATHER.BIN", 30, launches + 1): fails.append("the kernel did not start Weather again after R")
    if not wait_serial("WEATHER.BIN exited 7", 5): fails.append("R did not make the app exit 7")
    n = serial().count("wxfetch")
    if n != fetches + 1: fails.append(f"R did not cause exactly one more fetch (had {fetches}, now {n})")
    if serial().count("wxwin=offline sample") < 2: fails.append("the relaunched app did not draw the offline face again")

    # 3. Esc closes it cleanly
    exits = serial().count("WEATHER.BIN exited 0")
    keys("esc")
    if not wait_serial("WEATHER.BIN exited 0", 8, exits + 1):
        fails.append("Weather did not exit 0 on Esc")
    if not wait_serial("syscall: window released, task gone", 5):
        fails.append("Weather did not release its window on Esc")
    move(*PARK); time.sleep(0.5)
    for _ in range(100):
        if not near(pixel(CLOSE_X, CLOSE_Y), CLOSE_RED): break
        time.sleep(0.1)
    if near(pixel(CLOSE_X, CLOSE_Y), CLOSE_RED):
        fails.append("an app window is still open after Esc")

    # 5. the desktop must answer
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
    if not opened: fails.append("Mail did not open from a dock click after Weather closed: desktop not responsive")
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
print("PASS: Weather ran at ring 3 with its own window, showed the offline face from the kernel's WEATHER.TXT, R refetched once and reopened it, Esc closed it, and the desktop stayed alive")
