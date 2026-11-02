#!/usr/bin/env python3
"""8.13.0, the kit's screens: the Pi image fills each panel the Nimbus kit can carry at its own pixels.

Boots kernel8.img on QEMU's raspi4b with the firmware framebuffer set to each size (-global bcm2835-fb.xres/yres stands in for
what the firmware reads from the panel): the 7 inch DSI touch screen (800x480), the 8 inch HDMI screens (1024x600 and 1280x800),
Touch Display 2 standing up (720x1280) and a 1080p monitor. For each:
1. the boot's size line says the buffer is the panel's size ("fb 800x480>800x480"), not an 800x600 box the firmware stretches;
2. "M1c fb ok": the menu bar and a window that ends above the dock were drawn on that screen;
3. the screen QEMU shows is that size, and the dock drew all 12 tiles.
Discriminating: put back the 800x600 floor in fb_pick_size (main.c) and the 800x480 and 720x1280 lines say 800x600; drop the
width test from window_scale (crash.c) and 720x1280 lays out at half its width.
Skips (exit 0) without clang, ld.lld, or qemu-system-aarch64 with its raspi4b model.
Usage: tools/checks/arm64-screens-check.py   (from the repo root)
"""
import json, os, re, shutil, socket, subprocess, sys, time
from pathlib import Path

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from scratch import scratch_dir

root = Path(__file__).resolve().parents[2]
arch = root / "arch/arm64"
if not all(shutil.which(t) for t in ("clang", "ld.lld", "qemu-system-aarch64")):
    print("SKIP: clang, ld.lld or qemu-system-aarch64 not installed"); sys.exit(0)
if "raspi4b" not in subprocess.run(["qemu-system-aarch64", "-machine", "help"], capture_output=True, text=True).stdout:
    print("SKIP: QEMU here has no raspi4b model (needs QEMU 9 or newer)"); sys.exit(0)
tmp = Path(scratch_dir("arm64-screens"))

fails = []
def check(name, ok, detail=""):
    print(("  ok: " if ok else "  FAIL: ") + name + ("" if ok else "  " + repr(detail)[-600:]))
    if not ok: fails.append(name)

subprocess.run(["make", "-C", str(arch), "clean"], capture_output=True, timeout=60)
r = subprocess.run(["make", "-C", str(arch), "kernel8.img"], capture_output=True, text=True, timeout=600)
if r.returncode: print(r.stdout[-2000:], r.stderr[-2000:]); print("FAIL: the Pi image does not build"); sys.exit(1)

SIZE = re.compile(r" fb (\d+)x(\d+)>(\d+)x(\d+) ")
def boot(w, h):
    log, sock, shot = tmp / ("uart%dx%d" % (w, h)), tmp / "qmp", tmp / ("shot%dx%d.ppm" % (w, h))
    q = subprocess.Popen(["qemu-system-aarch64", "-machine", "raspi4b", "-display", "none", "-serial", "null", "-serial", "file:" + str(log),
                          "-global", "bcm2835-fb.xres=%d" % w, "-global", "bcm2835-fb.yres=%d" % h,
                          "-qmp", "unix:%s,server,nowait" % sock, "-kernel", str(arch / "kernel8.img")],
                         stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        out = ""
        for _ in range(900):
            time.sleep(0.1)
            out = log.read_text(errors="replace") if log.exists() else ""
            if "M1c fb ok" in out or "M1c fb FAIL" in out or q.poll() is not None: break
        time.sleep(1.5)   # the dock and the first frame after the line
        img = None
        if q.poll() is None:
            s = socket.socket(socket.AF_UNIX); s.settimeout(20); s.connect(str(sock)); f = s.makefile("rw"); f.readline()
            for c in ({"execute": "qmp_capabilities"}, {"execute": "screendump", "arguments": {"filename": str(shot)}}):
                f.write(json.dumps(c) + "\n"); f.flush()
                while True:
                    rr = json.loads(f.readline())
                    if "return" in rr or "error" in rr: break
            time.sleep(0.3)
            parts = shot.read_bytes().split(b"\n", 3); sw, sh = map(int, parts[1].split()); img = (sw, sh, parts[3])
        return log.read_text(errors="replace"), img
    finally:
        q.kill(); q.wait()

def px(img, x, y): sw, _, d = img; i = (y * sw + x) * 3; return tuple(d[i:i + 3])
try:
    for w, h in ((800, 480), (1024, 600), (1280, 800), (720, 1280), (1920, 1080)):
        out, img = boot(w, h)
        m = SIZE.search(out)
        got = tuple(map(int, m.groups())) if m else None
        check("%dx%d: the buffer is the panel's own size (%s)" % (w, h, "%sx%s>%sx%s" % got if got else "no size line"), got == (w, h, w, h), out[-400:])
        check("%dx%d: the menu bar and a window above the dock drew (M1c fb ok)" % (w, h), "M1c fb ok" in out and "KERNEL CRASH" not in out, out[-400:])
        if img:
            sw, sh, _ = img
            check("%dx%d: QEMU shows %dx%d, and the dock drew its 12 tiles" % (w, h, sw, sh), (sw, sh) == (w, h) and "M1d dock 12 icons" in out, (sw, sh, out[-300:]))
        else: check("%dx%d: a screen to look at" % (w, h), False, out[-300:])
finally:
    subprocess.run(["make", "-C", str(arch), "clean"], capture_output=True, timeout=60)
if fails: print("FAIL: %d of the screen checks" % len(fails)); sys.exit(1)
print("PASS: the Pi image fills 800x480, 1024x600, 1280x800, 720x1280 and 1080p panels at their own pixels, menu bar, a window and the dock on each")
