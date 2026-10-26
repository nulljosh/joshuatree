#!/usr/bin/env python3
"""The dock's running-app dots on both desktops (Joshua, 2026-10-07: "a little light below the icon, like macOS").

i386: boots with `open=note`, so Notes opens as a window; the serial line must say the Notes slot (4) has a dot, and
the dock band, against the same band once Notes is closed, must differ only in one small dark disc, under slot 4,
whose closed pixel is the tray again. Esc must take the dot away ("dock dots" all 0).
Pi (QEMU virt): the Console is up at boot there, so the Terminal's slot 6 has a dot (they share one window); Spotlight
opens Calendar (slot 3) and its dot follows it open and closed, as one small dark disc on the tray.

Discriminating: drop the gui_app_open call in kernel/dock_draw.c, dock_dots_tick in arch/arm64/boot.c, or the dot's
erase, and a step here fails.

Usage: tools/checks/dockdots-check.py   (from the repo root, after make kernel.elf)
"""
import json, os, re, shutil, socket, subprocess, sys, time
from pathlib import Path
from PIL import Image
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from freeport import free_port
from scratch import scratch_dir

ROOT = Path(__file__).resolve().parents[2]
os.chdir(ROOT)
TMP = Path(scratch_dir("dockdots"))
TRAY = (0xEF, 0xEB, 0xE4)
fails = []
def check(name, ok, detail=""):
    print(("  ok: " if ok else "  FAIL: ") + name + (f" ({detail})" if detail and not ok else ""))
    if not ok: fails.append(name)

def dot_blob(open_img, closed_img, band_top):
    """The pixels that differ in the dock band: their box, and whether it is a small dark disc on the tray."""
    w, h = open_img.size
    a = open_img.crop((0, band_top, w, h)).load(); b = closed_img.crop((0, band_top, w, h)).load()
    xs, ys = [], []
    for y in range(h - band_top):
        for x in range(w):
            if a[x, y] != b[x, y]: xs.append(x); ys.append(y)
    if not xs: return None
    x0, x1, y0, y1 = min(xs), max(xs), min(ys), max(ys)
    cx, cy = (x0 + x1) // 2, (y0 + y1) // 2
    dark = sum(a[cx, cy]) / 3 < 110
    tray = all(abs(b[cx, cy][i] - TRAY[i]) <= 6 for i in range(3))
    # the tray's run on the dot's row tells the slot: 11 equal tiles across it
    row = [x for x in range(w) if all(abs(b[x, cy][i] - TRAY[i]) <= 6 for i in range(3))]
    slot = (cx - row[0]) * 11 // (row[-1] - row[0] + 1) if row else -1
    return dict(w=x1 - x0 + 1, h=y1 - y0 + 1, dark=dark, tray=tray, slot=slot)

# ---- i386 ------------------------------------------------------------------------------------------------------
def i386():
    log, dump = TMP / "serial.log", TMP / "fb.raw"
    port = free_port(); W, H = 1920, 1080
    q = subprocess.Popen(["qemu-system-i386", "-kernel", "kernel.elf", "-append", "open=note", "-display", "none", "-vga", "std",
                          "-qmp", f"tcp:127.0.0.1:{port},server,nowait", "-serial", f"file:{log}"],
                         stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    serial = lambda: log.read_text(errors="replace") if log.exists() else ""
    def wait(needle, secs, after=0):
        for _ in range(int(secs * 10)):
            if needle in serial()[after:]: return True
            time.sleep(.1)
        return False
    try:
        s = None
        for _ in range(50):
            time.sleep(.2)
            try: s = socket.create_connection(("127.0.0.1", port)); break
            except OSError: pass
        if s is None: check("i386: QEMU's QMP socket came up", False); return
        f = s.makefile("rw"); f.readline()
        def cmd(o):
            f.write(json.dumps(o) + "\n"); f.flush()
            while True:
                r = json.loads(f.readline())
                if "return" in r or "error" in r: return r
        cmd({"execute": "qmp_capabilities"})
        def frame():
            cmd({"execute": "pmemsave", "arguments": {"val": 0xfd000000, "size": W * H * 4, "filename": str(dump)}})
            return Image.frombytes("RGBA", (W, H), dump.read_bytes(), "raw", "BGRA").convert("RGB")
        check("i386: Notes opened as a window and slot 4 got a dot", wait("dock dots 00001000000", 60), serial()[-300:])
        time.sleep(.5); opened = frame()
        if os.environ.get("JT_SHOT"): opened.crop((0, H - 200, W, H)).save(os.environ["JT_SHOT"])
        n = len(serial())
        cmd({"execute": "send-key", "arguments": {"keys": [{"type": "qcode", "data": "esc"}]}})
        check("i386: Esc closed Notes and the dot went", wait("dock dots 00000000000", 20, n), serial()[-300:])
        time.sleep(.5); closed = frame()
        blob = dot_blob(opened, closed, H - 160)
        check("i386: the dock band differs only in one small disc", blob is not None and blob["w"] <= 14 and blob["h"] <= 14, str(blob))
        if blob:
            check("i386: the disc is dark on the tray, and the tray once closed", blob["dark"] and blob["tray"], str(blob))
            check("i386: the disc sits under slot 4 (Notes)", blob["slot"] == 4, str(blob))
    finally:
        q.terminate()
        try: q.wait(5)
        except subprocess.TimeoutExpired: q.kill()

# ---- Pi (QEMU virt) ----------------------------------------------------------------------------------------------
def pi():
    if not all(shutil.which(t) for t in ("clang", "ld.lld", "qemu-system-aarch64")):
        print("  skip: no aarch64 toolchain"); return
    subprocess.run(["make", "-s", "-C", "arch/arm64", "kernel8.elf"], check=True)
    log, sock = TMP / "uart", TMP / "qmp"
    q = subprocess.Popen(["qemu-system-aarch64", "-machine", "virt", "-cpu", "cortex-a72", "-m", "256",
        "-nic", "none", "-global", "virtio-mmio.force-legacy=false", "-device", "ramfb",
        "-device", "virtio-keyboard-device", "-device", "virtio-tablet-device", "-display", "none",
        "-serial", f"file:{log}", "-qmp", f"unix:{sock},server,nowait", "-kernel", "arch/arm64/kernel8.elf"],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    uart = lambda: log.read_text(errors="replace") if log.exists() else ""
    def wait(text, secs=15, after=0):
        for _ in range(int(secs * 10)):
            if text in uart()[after:]: return True
            time.sleep(.1)
        return False
    try:
        if not wait("M2 input ready", 30): check("Pi: the desktop came up", False); return
        s = socket.socket(socket.AF_UNIX); s.settimeout(10); s.connect(str(sock)); f = s.makefile("rw"); f.readline()
        def command(name, args=None):
            f.write(json.dumps({"execute": name, **({"arguments": args} if args else {})}) + "\n"); f.flush()
            while True:
                r = json.loads(f.readline())
                if "return" in r or "error" in r: return r.get("return")
        def key(name):
            command("send-key", {"keys": [{"type": "qcode", "data": name}], "hold-time": 50}); time.sleep(.2)
        def shot(name):
            p = TMP / (name + ".ppm"); command("screendump", {"filename": str(p)}); return Image.open(p).convert("RGB")
        command("qmp_capabilities")
        time.sleep(1)
        n = len(uart()); key("f1")
        for k in "calendar": key(k)
        key("ret")
        bit = lambda k, after: [m[k] for m in re.findall(r"dock dots ([01]{11})", uart()[after:])][-1:]
        check("Pi: the Console's window, up at boot on QEMU virt, dots the Terminal's slot 6", bit(6, 0) == ["1"], uart()[-300:])
        for _ in range(100):
            if bit(3, n) == ["1"]: break
            time.sleep(.1)
        check("Pi: Calendar opened and slot 3 got a dot", bit(3, n) == ["1"], uart()[-300:])
        time.sleep(.5); opened = shot("cal-open")
        if os.environ.get("JT_SHOT_PI"): opened.crop((0, opened.size[1] - 120, opened.size[0], opened.size[1])).save(os.environ["JT_SHOT_PI"])
        n = len(uart())
        for _ in range(3):
            key("esc"); time.sleep(.3)
            if "M3c app calendar exit 0" in uart()[n:]: break
        time.sleep(1)
        check("Pi: closing Calendar took its dot away", bit(3, n) == ["0"], uart()[-300:])
        time.sleep(.5); closed = shot("cal-closed")
        blob = dot_blob(opened, closed, opened.size[1] - 80)
        check("Pi: the dock band differs only in one small disc, dark on the tray, under slot 3",
              blob is not None and blob["w"] <= 10 and blob["dark"] and blob["tray"] and blob["slot"] == 3, str(blob))
    finally:
        q.terminate()
        try: q.wait(5)
        except subprocess.TimeoutExpired: q.kill()

i386()
pi()
if fails: print("FAIL: " + "; ".join(fails)); sys.exit(1)
print("PASS: a running-app dot appears under Notes' tile on i386, and the Terminal's and Calendar's on the Pi, while they are "
      "open, as one small dark disc on the tray, and goes when they close")
