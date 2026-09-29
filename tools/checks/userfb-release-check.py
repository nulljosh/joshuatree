#!/usr/bin/env python3
"""A closed window's framebuffer is the kernel's again, and a bad pointer
is an error, not a read. The 1.7.8 security slice toward 2.0.

1.7.7 mapped the ring-3 framebuffer (JT_USER_FB, boot/linker.ld's .userfb)
user-accessible in SYS_WINDOW_OPEN and never flipped it back when the
window went away: it only zeroed the pixels. So the next ring-3 program,
with no window of its own, could still write those pages, and a syscall
handed a pointer into them passed paging_user_range_ok. 1.7.8 adds
paging_clear_user() and calls it from window_release and from every task
teardown (exit, idt.c's fault reap). This check drives the real sequence:

  1. boots headless with `open=keyrate fbpoke`. Keyrate opens its window
     through SYS_WINDOW_OPEN (the pages go user-accessible), then Esc makes
     it exit 0, the normal close: the window is released;
  2. kernel/ring3app.c then execs user/fbpoke.c with no window. It hands
     write() a pointer into kernel text, which must return -14 (EFAULT),
     then a pointer into the released framebuffer, also -14. Either
     accepted is a "BUG" line from the program and a failed check;
  3. fbpoke then stores straight into the released framebuffer. That must
     be a ring-3 page fault the kernel reaps (idt.c names it on serial),
     and the launcher must log the crash by name. A program that exits
     instead means the pages were still writable: the 1.7.7 hole;
  4. the desktop is alive afterwards: the dock is on screen, Mail opens
     from a dock click, Esc closes it.

Discriminating: drop the paging_clear_user() call from window_release in
kernel/syscall.c and steps 2 and 3 both fail (write() returns 8, the store
succeeds, fbpoke exits 3). Drop the paging_user_range_ok() check from
console_write and step 2 fails on the kernel pointer.

Usage: tools/checks/userfb-release-check.py   (from the repo root, after make kernel.elf)
"""
import json, os, socket, subprocess, sys, time
from PIL import Image
from freeport import free_port

LOG = "/tmp/jt-userfb-release-serial.log"
DUMP = "/tmp/jt-userfb-release.raw"
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

q = subprocess.Popen(["qemu-system-i386", "-kernel", "kernel.elf", "-append", "open=keyrate fbpoke",
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

    # 1. a window opens (pages go user-accessible), then closes normally
    if not wait_serial("fbpoke armed", 40):
        fails.append("the fbpoke boot flag was not seen by ring3app_autoopen_arm")
    if not wait_serial("syscall: window opened for ring-3 task", 40):
        fails.append("SYS_WINDOW_OPEN never succeeded: nothing was ever mapped user-accessible, the check cannot prove anything")
    time.sleep(1.0)
    keys("esc"); time.sleep(0.3)
    if not wait_serial("syscall: window released, task gone", 5):
        fails.append("Keyrate did not release its window on Esc")
    if not wait_serial("ring3app: KEYRATE.BIN exited 0", 5):
        fails.append("Keyrate did not exit 0 on Esc")

    # 2. the next program, no window: both bad pointers refused
    if not wait_serial("ring3app: launching FBPOKE.BIN at ring 3, no window", 5):
        fails.append("ring3app did not launch fbpoke after Keyrate exited")
    if not wait_serial("fbpoke: kernel pointer write returned -14", 5):
        fails.append("write() with a kernel pointer did not return -EFAULT")
    if not wait_serial("fbpoke: released framebuffer pointer returned -14", 5):
        fails.append("write() with a pointer into the released framebuffer did not return -EFAULT: paging_user_range_ok still sees it as user memory")

    # 3. the store into the released framebuffer is a fault, not a write
    if not wait_serial("fbpoke: storing into the released framebuffer", 5):
        fails.append("fbpoke never reached its store")
    if not wait_serial("exception: ring-3 task hit page-fault, reaped", 5):
        fails.append("storing into the released framebuffer did not page-fault: the pages were still user-accessible (the 1.7.7 hole)")
    if not wait_serial("ring3app: FBPOKE.BIN crashed (page-fault), released framebuffer stayed supervisor-only", 5):
        fails.append("the launcher did not log fbpoke's page fault by name")
    if not wait_serial("autoopen: back on the desktop", 5):
        fails.append("the desktop loop was never re-entered")
    log = serial()
    if "fbpoke: BUG" in log or "ring3app: BUG" in log:
        fails.append("a BUG line was logged: " + [l for l in log.splitlines() if "BUG" in l][0])
    if "exception: ring-0" in log or "panic in" in log:
        fails.append("the KERNEL faulted: the crash was not contained to the ring-3 task")

    # 4. the desktop is alive and takes input
    move(*PARK); time.sleep(0.5)
    dock = pixel(480, 511)
    print(f"dock tray after fbpoke: {dock}")
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
    print(serial()[-1500:])
    sys.exit(1)
print("PASS: the released window framebuffer is supervisor-only again (store faults, pointer refused), a kernel pointer is -EFAULT, and the desktop stayed alive")
