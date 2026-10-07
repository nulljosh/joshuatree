#!/usr/bin/env python3
"""Every ring-3 app can crash on purpose and the desktop survives it
(2.0 gate item: "a check crashes each app on purpose and proves the desktop
is still alive after every one").

The app list is parsed out of RING3_APPS in kernel/ring3app.c, so a new app
is covered the day it is added. For each app the check boots a fresh headless
kernel with that app's `open=` flag (read from ring3app_autoopen_arm in the
same file), waits for its window, then presses the backquote: the deliberate
crash key every ring-3 app has (a write through a null pointer, a page fault
at CPL 3). After each crash it asserts:

  1. the app announced "<name>: crashing on purpose", so the fault came from
     the program and not from some earlier bug;
  2. the kernel reaped the task ("exception: ring-3 task hit page-fault,
     reaped"), released the window, and the launcher logged
     "<FILE>.BIN crashed (page-fault), window torn down, desktop alive";
  3. no ring-0 exception, panic or ring3app BUG line;
  4. the screen shows the desktop: the app window's red close dot is gone
     and the dock tray is drawn (the frame changed from the app to the
     desktop);
  5. a different app works: Mail opens from a dock click and closes on Esc.

It fails loudly if the table holds an app with no `open=` flag, or whose
source has no crash key, rather than skipping it.

Discriminating: make idt.c halt on a ring-3 fault, or leave the task
un-reaped (skip syscall_release_task in task_exit_with), and every app fails
steps 2 to 5.

Usage: tools/checks/ring3crash-all-check.py [app ...]  (from the repo root,
after make kernel.elf; with names, only those apps)
"""
import json, os, re, socket, subprocess, sys, time
from PIL import Image
from freeport import free_port
from scratch import scratch_dir

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..")
os.chdir(ROOT)
TMP = scratch_dir("ring3crashall")  # private per run, see scratch.py
LOG = os.path.join(TMP, "serial.log")
DUMP = os.path.join(TMP, "fb.raw")
FB = 0xfd000000; W, H = 1920, 1080
LOGICAL_W, LOGICAL_H, SCALE = 960, 540, 2
DOCK_ICON, DOCK_GAP, SLOT0_X = 37, 6, 247
PITCH = DOCK_ICON + DOCK_GAP
ICON_ROW_Y = 487
CLOSE_X, CLOSE_Y = 94, 56
CLOSE_RED = (0xFF, 0x5F, 0x57)
DOCK_TRAY = (0xEF, 0xEB, 0xE4)
PARK = (480, 200)


# Hamurapi is shown under its product name but its files, flag letters and log lines kept the old spelling Hamurabi.
SOURCE_NAME = {"hamurapi": "hamurabi"}


def parse_apps():
    src = open("kernel/ring3app.c").read()
    table = re.search(r"RING3_APPS\[\]\s*=\s*\{(.*?)\n\};", src, re.S)
    if not table: sys.exit("FAIL: could not find the RING3_APPS table in kernel/ring3app.c")
    apps = re.findall(r'\{\s*"([^"]+)"\s*,\s*\w+\s*,\s*\w+\s*,\s*\w+\s*,\s*\w+\s*,\s*"([^"]+)"\s*\}', table.group(1))
    if not apps: sys.exit("FAIL: RING3_APPS parsed to zero apps")
    # open=<prefix> flags: `pc[5]=='k' && pc[6]=='e' ...` then serial_puts("autoopen=<name>")
    flags = {}
    for line in src.splitlines():
        m = re.search(r'serial_puts\("autoopen=(\w+)\\n"\)', line)
        if not m or "pc[" not in line: continue
        chars = sorted((int(i), c) for i, c in re.findall(r"pc\[(\d+)\]\s*==\s*'(.)'", line))
        flags[m.group(1)] = "".join(c for i, c in chars if i >= 5)
    out = []
    for name, binf in apps:
        key = name.lower()
        if key not in flags:
            sys.exit(f"FAIL: {name} is in RING3_APPS but ring3app_autoopen_arm has no open= flag for it; the crash check cannot open it")
        src = SOURCE_NAME.get(key, key)   # the row's shown name can differ from the source's spelling
        path = f"user/{src}.c"
        if not os.path.exists(path) or f"{src}: crashing on purpose" not in open(path).read():
            sys.exit(f"FAIL: {name} ({path}) has no `{src}: crashing on purpose` crash key; the crash check cannot crash it")
        out.append((name, key, binf, flags[key], src))
    return out


def check_app(name, key, binf, flag, src):
    fails = []
    for f in (LOG, DUMP):
        try: os.remove(f)
        except FileNotFoundError: pass
    port = free_port()
    q = subprocess.Popen(["qemu-system-i386", "-kernel", "kernel.elf", "-append", f"open={flag}",
                          "-display", "none", "-vga", "std",
                          "-qmp", f"tcp:127.0.0.1:{port},server,nowait", "-serial", "file:" + LOG],
                         stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    def serial():
        try: return open(LOG, errors="replace").read()
        except OSError: return ""
    def wait_serial(needle, secs):
        for _ in range(int(secs * 10)):
            if needle in serial(): return True
            time.sleep(0.1)
        return False
    cmd = None
    try:
        s = None
        for _ in range(50):
            time.sleep(0.2)
            try: s = socket.create_connection(("127.0.0.1", port)); break
            except OSError: pass
        if s is None: return ["QEMU's QMP socket never came up"]
        f = s.makefile("rw")
        def cmd(o):
            f.write(json.dumps(o) + "\n"); f.flush()
            while True:
                r = json.loads(f.readline())
                if "return" in r or "error" in r: return r
        f.readline()
        cmd({"execute": "qmp_capabilities"})
        def press(k):
            r = cmd({"execute": "send-key", "arguments": {"keys": [{"type": "qcode", "data": k}]}})
            if "error" in r: raise RuntimeError(f"QMP rejected send-key {k}: {r['error']}")
            time.sleep(0.3)
        def move(x, y):
            cmd({"execute": "input-send-event", "arguments": {"events": [
                {"type": "abs", "data": {"axis": "x", "value": int(x * 32768 / LOGICAL_W)}},
                {"type": "abs", "data": {"axis": "y", "value": int(y * 32768 / LOGICAL_H)}}]}})
        def click():
            cmd({"execute": "input-send-event", "arguments": {"events": [{"type": "btn", "data": {"down": True, "button": "left"}}]}})
            time.sleep(0.1)
            cmd({"execute": "input-send-event", "arguments": {"events": [{"type": "btn", "data": {"down": False, "button": "left"}}]}})
        def pixel(x, y):
            cmd({"execute": "pmemsave", "arguments": {"val": FB, "size": W * H * 4, "filename": DUMP}})
            img = Image.frombytes("RGBA", (W, H), open(DUMP, "rb").read(), "raw", "BGRA").convert("RGB")
            return img.getpixel((x * SCALE + 1, y * SCALE + 1))
        def near(p, c, tol=12): return max(abs(p[i] - c[i]) for i in range(3)) <= tol

        if not wait_serial(f"autoopen={key}", 40): return [f"open={flag} was not recognised"]
        if not wait_serial(f"ring3app: launching {binf} at ring 3", 40): return [f"{binf} was never launched"]
        if not wait_serial("syscall: window opened for ring-3 task", 15): return ["the app never opened its window"]
        time.sleep(1.0)
        up = False
        for _ in range(50):
            if (near(pixel(CLOSE_X, CLOSE_Y), CLOSE_RED) or near(pixel(24, 24), CLOSE_RED)): up = True; break
            time.sleep(0.1)
        if not up: fails.append("the app window (red close dot) was not on screen before the crash")

        crash_line = f"{src}: crashing on purpose"
        for _ in range(4):  # the app may still be settling; retry the key
            press("grave_accent")
            if wait_serial(crash_line, 3): break
        else:
            return fails + ["the crash key never reached the program"]

        if not wait_serial("exception: ring-3 task hit page-fault, reaped", 8):
            fails.append("the kernel did not reap the ring-3 task on its page fault")
        if not wait_serial("syscall: window released, task gone", 8):
            fails.append("the window was not released when the task died")
        if not wait_serial(f"ring3app: {binf} crashed (page-fault), window torn down, desktop alive", 8):
            fails.append("the launcher did not log the crash by name and return")
        if not wait_serial("autoopen: back on the desktop", 8):
            fails.append("the desktop loop was never re-entered")
        log = serial()
        if "exception: ring-0" in log or "panic in" in log or "ring3app: BUG" in log:
            fails.append("the KERNEL faulted or ring3app logged a BUG line")

        move(*PARK); time.sleep(0.6)
        gone = False
        for _ in range(50):
            if not (near(pixel(CLOSE_X, CLOSE_Y), CLOSE_RED) or near(pixel(24, 24), CLOSE_RED)): gone = True; break
            time.sleep(0.1)
        if not gone: fails.append("the dead app's window is still on screen")
        dock = pixel(480, 511)
        if dock != DOCK_TRAY: fails.append(f"the dock tray is not drawn after the crash (got {dock})")

        # a different app still opens and closes
        move(SLOT0_X + 2 * PITCH + DOCK_ICON // 2, ICON_ROW_Y); time.sleep(0.3); click()
        opened = False
        for _ in range(40):
            time.sleep(0.1)
            if (near(pixel(CLOSE_X, CLOSE_Y), CLOSE_RED) or near(pixel(24, 24), CLOSE_RED)): opened = True; break
        if not opened: fails.append("Mail did not open from a dock click after the crash: desktop not responsive")
        else:
            press("esc"); time.sleep(0.8)
            if (near(pixel(CLOSE_X, CLOSE_Y), CLOSE_RED) or near(pixel(24, 24), CLOSE_RED)): fails.append("Mail did not close on Esc after the crash")
        return fails
    except (RuntimeError, OSError, ValueError) as e:
        return fails + [f"harness error: {e}"]
    finally:
        try:
            if cmd: cmd({"execute": "quit"})
        except Exception: pass
        q.terminate()
        try: q.wait(5)
        except subprocess.TimeoutExpired: q.kill()


apps = parse_apps()
want = [a.lower() for a in sys.argv[1:]]
if want:
    bad = [w for w in want if w not in [a[1] for a in apps]]
    if bad: sys.exit(f"FAIL: unknown app(s) {bad}")
    apps = [a for a in apps if a[1] in want]
failed = {}
t0 = time.time()
for name, key, binf, flag, src in apps:
    t = time.time()
    fails = check_app(name, key, binf, flag, src)
    print(f"{name:11s} open={flag:7s} {'PASS' if not fails else 'FAIL'} ({time.time() - t:.0f}s)", flush=True)
    if fails:
        failed[name] = fails
if failed:
    print("FAIL:")
    for n, fl in failed.items():
        for x in fl: print(f"  - {n}: {x}")
    print("--- last serial tail ---")
    try: print(open(LOG, errors="replace").read()[-1200:])
    except OSError: pass
    sys.exit(1)
print(f"PASS: all {len(apps)} of {len(apps)} ring-3 apps, crashed on purpose, were reaped, the desktop kept drawing, and a different app opened afterwards ({time.time() - t0:.0f}s)")
