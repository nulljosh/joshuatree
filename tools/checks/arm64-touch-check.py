#!/usr/bin/env python3
"""8.14.0, touch screens: a USB HID touch panel (the kit's HDMI screens send their touch over USB) is an absolute pointer.

Boots the ARM64 kernel on QEMU's virt machine with qemu-xhci behind a PCIe root port, the Pi 4's shape, and a usb-tablet on it:
like a touch panel, a HID device that is not a boot mouse and reports X and Y as absolute values. Then:
1. xhci.c reads its report descriptor and says "usb touch" (not "no absolute pointer");
2. a tap (move to the Terminal's dock tile, press, release) opens the Terminal;
3. a tap on the Samantha tile, at the other end of the same row, opens her prompt: the X axis is scaled, not just passed through.
There is no virtio tablet in this boot, so only the USB path can move the pointer.
Discriminating: take kind 4 out of the configuration walk (xhci.c) and the line is "usb hid if 0 ..." and nothing opens;
scale X by the wrong maximum and the second tap lands on the wrong tile.
Skips (exit 0) without clang, ld.lld or qemu-system-aarch64.
Usage: tools/checks/arm64-touch-check.py   (from the repo root)
"""
import json, os, shutil, socket, subprocess, sys, time
from pathlib import Path

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from scratch import scratch_dir

root = Path(__file__).resolve().parents[2]
arch = root / "arch/arm64"
if not all(shutil.which(t) for t in ("clang", "ld.lld", "qemu-system-aarch64")):
    print("SKIP: clang, ld.lld or qemu-system-aarch64 not installed"); sys.exit(0)
tmp = Path(scratch_dir("arm64-touch"))
fails = []
def check(name, ok, detail=""):
    print(("  ok: " if ok else "  FAIL: ") + name + ("" if ok else "  " + repr(detail)[-600:]))
    if not ok: fails.append(name)

subprocess.run(["make", "-C", str(arch), "clean"], capture_output=True, timeout=60)
r = subprocess.run(["make", "-C", str(arch), "kernel8.elf"], capture_output=True, text=True, timeout=600)
if r.returncode: print(r.stdout[-2000:], r.stderr[-2000:]); print("FAIL: arch/arm64 does not build"); sys.exit(1)

# The dock on QEMU's 800x600 ramfb at scale 1, from kernel/dock_geom.c with the default dock_scale_pct of 7.
W, H, PCT, ICONS, GAP, PAD, MBOT = 800, 600, 7, 12, 6, 10, 24
icon = min(H * PCT // 100, (min(740, W - 40) - 2 * PAD - (ICONS - 1) * GAP) // ICONS)
x0 = (W - (ICONS * icon + (ICONS - 1) * GAP + 2 * PAD)) // 2
y0 = H - icon - 2 * PAD - MBOT
def tile(slot): return x0 + PAD + slot * (icon + GAP) + icon // 2, y0 + PAD + icon // 2

log, sock = tmp / "uart", tmp / "qmp"
q = subprocess.Popen(["qemu-system-aarch64", "-machine", "virt", "-cpu", "cortex-a72", "-m", "256", "-global", "virtio-mmio.force-legacy=false",
                      "-device", "ramfb", "-device", "virtio-keyboard-device",
                      "-device", "pcie-root-port,id=rp,chassis=1", "-device", "qemu-xhci,id=x,bus=rp", "-device", "usb-tablet,bus=x.0,port=1",
                      "-display", "none", "-serial", "file:" + str(log), "-qmp", "unix:%s,server,nowait" % sock, "-kernel", str(arch / "kernel8.elf")],
                     stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
def uart(): return log.read_text(errors="replace") if log.exists() else ""
def wait_for(text, secs, after=0):
    end = time.time() + secs
    while time.time() < end:
        if text in uart()[after:]: return True
        if q.poll() is not None: return False
        time.sleep(0.1)
    return False
try:
    if not wait_for("M2 input ready", 60): print("FAIL: did not boot: %r" % uart()[-600:]); sys.exit(1)
    check("xhci.c reads the tablet's report descriptor and takes it as a touch screen", wait_for("usb touch", 20), [l for l in uart().splitlines() if "usb" in l][-6:])
    time.sleep(1.0)
    s = socket.socket(socket.AF_UNIX); s.settimeout(30); s.connect(str(sock)); f = s.makefile("rw"); f.readline()
    def command(name, **a):
        f.write(json.dumps({"execute": name, "arguments": a}) + "\n"); f.flush()
        while True:
            rr = json.loads(f.readline())
            if "return" in rr or "error" in rr: return rr
    command("qmp_capabilities")
    def tap(x, y):
        command("input-send-event", events=[{"type": "abs", "data": {"axis": "x", "value": int(x * 32767 / W)}},
                                            {"type": "abs", "data": {"axis": "y", "value": int(y * 32767 / H)}}])
        time.sleep(0.4)
        for down in (True, False):
            command("input-send-event", events=[{"type": "btn", "data": {"button": "left", "down": down}}]); time.sleep(0.25)
    n = len(uart()); tap(*tile(6))
    check("a tap on the Terminal's tile %s opens the Terminal" % (tile(6),), wait_for("terminal open", 10, n), uart()[n:][-400:])
    time.sleep(0.5)
    n = len(uart()); tap(*tile(7))
    check("a tap on Samantha's tile %s opens her prompt" % (tile(7),), wait_for("dock Samantha", 10, n), uart()[n:][-400:])
    check("no kernel crash", "KERNEL CRASH" not in uart() and q.poll() is None, uart()[-400:])
finally:
    q.kill(); q.wait()
    subprocess.run(["make", "-C", str(arch), "clean"], capture_output=True, timeout=60)
if fails: print("FAIL: %d of the touch checks" % len(fails)); sys.exit(1)
print("PASS: a USB touch panel is read from its report descriptor as an absolute pointer, and taps open the tiles they land on")
