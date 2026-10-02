#!/usr/bin/env python3
"""SYS_BRK hands every frame back when the program dies. The 1.9.27 heap slice.

kernel/brk.c gives each ring-3 task a heap at JT_BRK_BASE that grows a page
at a time from the PMM, zeroed, mapped into that task's directory only, and
freed from syscall_release_task on exit or crash. This check drives the real
sequence under the `brkpoke` boot flag (kernel/ring3app.c):

  1. boots headless with `open=keyrate brkpoke`; Keyrate opens and Esc
     closes it, the same door userfb-release-check.py uses, so brkpoke
     runs on a desktop that already ran one app;
  2. the launcher logs the PMM free-frame count and the brk live-page
     count as the baseline, then execs user/brkpoke.c with no window;
  3. brkpoke asserts a fresh brk is JT_BRK_BASE, that a top below the base
     and one past the cap are both -EINVAL (22), grows 768 pages (3MB),
     touches every page, checks each came back zero and reads back its
     own writes, shrinks by half, then stores to address 0 on purpose;
  4. the kernel reaps the page fault, brk_release logs "live=0", and the
     launcher's after line must show free frames and live pages equal to
     the baseline;
  5. the desktop is alive afterwards: the dock is on screen, Mail opens
     from a dock click, Esc closes it.

Discriminating: drop the brk_release call from syscall_release_task and the
after line keeps 384 fewer free frames and live=384; drop the zeroing loop
in map_one and "dirty pages seen" can go non-zero on a reused frame.

Usage: tools/checks/ring3brk-check.py   (from the repo root, after make kernel.elf)
"""
import json, os, re, socket, subprocess, sys, time
from PIL import Image
from freeport import free_port

LOG = "/tmp/jt-ring3brk-serial.log"
DUMP = "/tmp/jt-ring3brk.raw"
FB = 0xfd000000; W, H = 1920, 1080
PORT = free_port()
LOGICAL_W, LOGICAL_H, SCALE = 960, 540, 2
DOCK_ICON, DOCK_GAP, SLOT0_X = 37, 6, 247
PITCH = DOCK_ICON + DOCK_GAP
ICON_ROW_Y = 487
CLOSE_X, CLOSE_Y = 94, 56
CLOSE_RED = (0xFF, 0x5F, 0x57)
PARK = (480, 200)

os.chdir(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
for f in (LOG, DUMP):
    try: os.remove(f)
    except FileNotFoundError: pass

q = subprocess.Popen(["qemu-system-i386", "-kernel", "kernel.elf", "-append", "open=keyrate brkpoke",
                      "-display", "none", "-vga", "std",
                      "-qmp", f"tcp:127.0.0.1:{PORT},server,nowait", "-serial", "file:" + LOG],
                     stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
fails = []
def serial():
    try: return open(LOG, errors="replace").read()
    except OSError: return ""
def wait_serial(needle, secs):
    for _ in range(int(secs * 10)):
        if needle in serial(): return True
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

    # 1. one app opens and closes normally first
    if not wait_serial("brkpoke armed", 40):
        fails.append("the brkpoke boot flag was not seen by ring3app_autoopen_arm")
    if not wait_serial("syscall: window opened for ring-3 task", 40):
        fails.append("Keyrate never opened")
    time.sleep(1.0)
    keys("esc"); time.sleep(0.3)
    if not wait_serial("ring3app: KEYRATE.BIN exited 0", 5):
        fails.append("Keyrate did not exit 0 on Esc")

    # 2. baseline and launch
    if not wait_serial("ring3app: launching BRKPOKE.BIN at ring 3, no window", 5):
        fails.append("ring3app did not launch brkpoke after Keyrate exited")
    # 3. the program's own assertions
    if not wait_serial("brkpoke: crashing on purpose", 15):
        fails.append("brkpoke never reached its crash: " + "; ".join(l for l in serial().splitlines() if l.startswith("brkpoke:"))[:400])
    log = serial()
    for needle, why in (("brkpoke: below base returned -22", "a top below JT_BRK_BASE was not -EINVAL"),
                        ("brkpoke: past cap returned -22", "a top past the per-task cap was not -EINVAL"),
                        ("brkpoke: touched pages 768", "the heap did not grow to 768 pages"),
                        ("brkpoke: dirty pages seen 0", "a freshly mapped heap page was not zero: another task's bytes leaked"),
                        ("brkpoke: readback mismatches 0", "heap pages did not hold their writes"),
                        ("brkpoke: shrink returned ", "shrinking the heap failed")):
        if needle not in log: fails.append(why)
    # 4. the crash is reaped and every frame comes back
    if not wait_serial("exception: ring-3 task hit page-fault, reaped", 5):
        fails.append("the null store did not page-fault and reap")
    if not wait_serial("ring3app: brkpoke after free=", 5):
        fails.append("the launcher never logged the after line")
    if not wait_serial("autoopen: back on the desktop", 5):
        fails.append("the desktop loop was never re-entered")
    log = serial()
    before = re.search(r"brkpoke baseline free=(\d+) live=(\d+)", log)
    after = re.search(r"brkpoke after free=(\d+) live=(\d+)", log)
    if not before or not after:
        fails.append("baseline or after line missing")
    else:
        print(f"pmm free before={before.group(1)} after={after.group(1)}; brk live before={before.group(2)} after={after.group(2)}")
        if after.group(2) != "0": fails.append(f"brk live pages after the crash is {after.group(2)}, not 0: heap pages leaked")
        if before.group(1) != after.group(1): fails.append(f"pmm free frames moved {before.group(1)} to {after.group(1)}: frames leaked (or were reclaimed) across the crash")
    if "brk: released 384 pages, live=0" not in log:
        fails.append("brk_release did not log the 384 remaining pages going back with live=0")
    if "brkpoke: BUG" in log or "ring3app: BUG" in log:
        fails.append("a BUG line was logged: " + [l for l in log.splitlines() if "BUG" in l][0])
    if "exception: ring-0" in log or "panic in" in log:
        fails.append("the KERNEL faulted: the crash was not contained to the ring-3 task")

    # 5. the desktop is alive and takes input
    move(*PARK); time.sleep(0.5)
    dock = pixel(480, 511)
    print(f"dock tray after brkpoke: {dock}")
    if dock != (0xEF, 0xEB, 0xE4):
        fails.append(f"desktop dock not on screen afterwards (got {dock})")
    move(SLOT0_X + 2 * PITCH + DOCK_ICON // 2, ICON_ROW_Y); time.sleep(0.3); click()
    opened = False
    for _ in range(40):
        time.sleep(0.1)
        if near(pixel(CLOSE_X, CLOSE_Y), CLOSE_RED): opened = True; break
    print(f"Mail opens from the dock afterwards: {'yes' if opened else 'NO'}")
    if not opened: fails.append("Mail did not open from a dock click afterwards: desktop not responsive")
    keys("esc"); time.sleep(1.0)
    if near(pixel(CLOSE_X, CLOSE_Y), CLOSE_RED):
        fails.append("Mail did not close on Esc")
finally:
    q.terminate()
    try: q.wait(5)
    except subprocess.TimeoutExpired: q.kill()

if fails:
    print("FAIL:")
    for x in fails: print("  - " + x)
    print("--- serial tail ---")
    print(serial()[-2500:])
    sys.exit(1)
print("PASS: SYS_BRK grew 3MB of zeroed pages, refused both bad tops, and the crash gave every frame back (live=0, pmm free unchanged); desktop alive")
