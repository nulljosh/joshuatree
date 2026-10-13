#!/usr/bin/env python3
"""ARM64 app windows at 1080p (scale 2): a normal size.

Board feedback: the ring-3 apps looked too big and pixelated beside the Terminal and Browser. The size half of that is fixed in
arch/arm64/main.c: at scale 2 the window is 520x368 logical (the Console is 500x350), the same as it was at 3.7.1. The pixelated
half moved on in 3.9.0: the apps now draw at device size and arm64-sharp2x-check.py proves their edges are crisp, so the old
"no hard 2x2 blocks" test that lived here is gone (a sharp rectangle edge is exactly two flat 2x2 blocks side by side).

For each of calendar, notes, music and books this builds the apptest kernel (`make APP_NAME=x apptest`: that app opened at
boot, since QEMU's raspi4b has no USB to click with), boots it on raspi4b at 1920x1080, waits for the first frame and takes a
screendump through QMP. Passes when, per app:
  - the UART says "M3c window x WxH at X,Y" and the frame (content plus its 8 px sides, 32 px title and 8 px foot) is between
    20% and 60% of the screen wide and 30% and 70% tall, and no more than 15% wider or taller than the Console's frame;
  - the screendump is the 1920x1080 screen and the frame's content area is not blank (it holds at least 200 colour changes).
Skips (exit 0) when clang, ld.lld, qemu-system-aarch64 or its raspi4b model is missing.
Usage: tools/checks/arm64-appsize-check.py   (from the repo root)
"""
import json, os, re, shutil, socket, subprocess, sys, time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from scratch import scratch_dir

root = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..")
arch = os.path.join(root, "arch/arm64")
if not all(shutil.which(t) for t in ("clang", "ld.lld", "qemu-system-aarch64")):
    print("SKIP: clang, ld.lld or qemu-system-aarch64 not installed"); sys.exit(0)
if "raspi4b" not in subprocess.run(["qemu-system-aarch64", "-machine", "help"], capture_output=True, text=True).stdout:
    print("SKIP: this QEMU has no raspi4b model (needs QEMU 9 or newer)"); sys.exit(0)

SW, SH = 1920, 1080
CONSOLE_W, CONSOLE_H = 500 * 2, 350 * 2   # the Terminal's frame at scale 2 (main.c win_w, win_h)
fails = []
def check(name, ok, detail=""):
    print(("  ok: " if ok else "  FAIL: ") + name + ("" if ok or not detail else " -- " + str(detail)[-400:]))
    if not ok: fails.append(name)

def transitions(img, x0, y0, w, h):
    """(all, hard): colour changes between aligned 2x2 blocks, and the ones where both blocks are flat."""
    rows = [img[((y0 + j) * SW + x0) * 3:((y0 + j) * SW + x0 + w) * 3] for j in range(h)]
    def block(bx, by):
        a = rows[by * 2][bx * 6:bx * 6 + 3], rows[by * 2][bx * 6 + 3:bx * 6 + 6], rows[by * 2 + 1][bx * 6:bx * 6 + 3], rows[by * 2 + 1][bx * 6 + 3:bx * 6 + 6]
        return a[0] if a[0] == a[1] == a[2] == a[3] else None
    bw, bh = w // 2, h // 2
    flat = [[block(x, y) for x in range(bw)] for y in range(bh)]
    allt = hard = 0
    for y in range(bh - 1):
        for x in range(bw - 1):
            c = flat[y][x]
            for n in (flat[y][x + 1], flat[y + 1][x]):
                if c is not None and n is not None and c != n: allt += 1; hard += 1
                elif (c is None) != (n is None) or (c is not None and c != n): allt += 1
    return allt, hard

def run(app):
    for f in os.listdir(arch):
        if f.startswith("app-"): os.remove(os.path.join(arch, f))
    if subprocess.run(["make", "-C", arch, "APP_NAME=" + app, "apptest"], capture_output=True).returncode:
        check(app + ": `make apptest` builds", False); return
    tmp = scratch_dir("arm64-appsize-" + app)
    log, sock, shot = tmp + "/uart", tmp + "/qmp", tmp + "/shot.ppm"
    q = subprocess.Popen(["qemu-system-aarch64", "-machine", "raspi4b", "-serial", "null", "-display", "none", "-serial", "file:" + log,
                          "-global", "bcm2835-fb.xres=%d" % SW, "-global", "bcm2835-fb.yres=%d" % SH, "-qmp", "unix:%s,server,nowait" % sock,
                          "-kernel", os.path.join(arch, "app-kernel8.img")], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    def uart(): return open(log, errors="replace").read() if os.path.exists(log) else ""
    try:
        end = time.time() + 40
        while time.time() < end and (app + " first frame") not in uart(): time.sleep(0.1)
        out = uart()
        m = re.search(r"M3c window %s (\d+)x(\d+) at (\d+),(\d+)" % app, out)
        check(app + ": the window opens and shows a first frame", bool(m) and (app + " first frame") in out, out[-300:])
        if not m or (app + " first frame") not in out: return
        cw, ch, cx, cy = map(int, m.groups())
        fw, fh = (cw + 16) * 2, (ch + 40) * 2
        check(app + ": frame %dx%d is %d%% x %d%% of the screen" % (fw, fh, fw * 100 // SW, fh * 100 // SH),
              0.20 * SW <= fw <= 0.60 * SW and 0.30 * SH <= fh <= 0.70 * SH)
        check(app + ": no more than 15%% bigger than the Console (%dx%d)" % (CONSOLE_W, CONSOLE_H), fw <= CONSOLE_W * 1.15 and fh <= CONSOLE_H * 1.15)
        time.sleep(1.0)
        s_ = socket.socket(socket.AF_UNIX); s_.connect(sock); f = s_.makefile("rw"); f.readline()
        def cmd(c, **a):
            f.write(json.dumps({"execute": c, "arguments": a}) + "\n"); f.flush()
            while True:
                r = json.loads(f.readline())
                if "return" in r or "error" in r: return r
        cmd("qmp_capabilities")
        if "error" in cmd("screendump", filename=shot): check(app + ": screendump", False); return
        time.sleep(0.5)
        parts = open(shot, "rb").read().split(b"\n", 3)
        w, h = map(int, parts[1].split())
        check(app + ": the screen is %dx%d" % (SW, SH), (w, h) == (SW, SH), (w, h))
        if (w, h) != (SW, SH): return
        allt, hard = transitions(parts[3], cx * 2, cy * 2, cw * 2, ch * 2)
        check(app + ": the content shows %d colour changes" % allt, allt >= 200)
    finally:
        q.terminate(); q.wait()

for a in ("calendar", "notes", "music", "books"):
    run(a)
subprocess.run(["make", "-C", arch, "clean"], capture_output=True)
if fails: print("FAIL: %d: %s" % (len(fails), "; ".join(fails))); sys.exit(1)
print("ok: every app window is a normal size at scale 2")
