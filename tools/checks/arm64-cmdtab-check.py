#!/usr/bin/env python3
"""Cmd+Tab on the Pi desktop (arch/arm64/appwin.c app_cycle; i386 already had its switcher in kernel/multiwin.c).

Boots the virt build with a keyboard, opens Calendar then Notes from Spotlight, and sends the chords through QMP:
  1. Cmd+Tab brings Calendar, the window behind, to the front ("switch calendar", then "focus app"), and the screen changed.
  2. Cmd+Tab again brings Notes back ("switch notes").
  3. Ctrl+Tab does the same on a keyboard with no Cmd key ("switch calendar").
  4. A bare Tab does not switch windows.

Discriminating: drop the Cmd+Tab line in arch/arm64/desk.c ui_key and steps 1 to 3 fail.

Usage: tools/checks/arm64-cmdtab-check.py   (from the repo root)
"""
import json, os, shutil, socket, subprocess, sys, time
from pathlib import Path
from PIL import Image
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from scratch import scratch_dir

ROOT = Path(__file__).resolve().parents[2]
os.chdir(ROOT)
TMP = Path(scratch_dir("cmdtab"))
fails = []
def check(name, ok, detail=""):
    print(("  ok: " if ok else "  FAIL: ") + name + (f" ({detail})" if detail and not ok else ""))
    if not ok: fails.append(name)

if not all(shutil.which(t) for t in ("clang", "ld.lld", "qemu-system-aarch64")):
    print("SKIP: no aarch64 toolchain"); sys.exit(0)
subprocess.run(["make", "-s", "-C", "arch/arm64", "kernel8.elf"], check=True)
log, sock = TMP / "uart", TMP / "qmp"
for p in (log, sock):
    if p.exists(): p.unlink()
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
    if not wait("M2 input ready", 30): check("the desktop came up", False); sys.exit(1)
    s = socket.socket(socket.AF_UNIX); s.settimeout(10); s.connect(str(sock)); f = s.makefile("rw"); f.readline()
    def command(name, args=None):
        f.write(json.dumps({"execute": name, **({"arguments": args} if args else {})}) + "\n"); f.flush()
        while True:
            r = json.loads(f.readline())
            if "return" in r or "error" in r: return r.get("return")
    def keys(*names):
        command("send-key", {"keys": [{"type": "qcode", "data": k} for k in names], "hold-time": 80}); time.sleep(.3)
    def shot(name):
        p = TMP / (name + ".ppm"); command("screendump", {"filename": str(p)}); return Image.open(p).convert("RGB")
    command("qmp_capabilities")
    time.sleep(1)
    for app in ("calendar", "notes"):
        n = len(uart()); keys("f1")
        for k in app: keys(k)
        keys("ret")
        check(f"{app} opened from Spotlight", wait(f"M3c window {app} first frame", 20, n), uart()[-300:])
    time.sleep(1); before = shot("notes-front")
    n = len(uart()); keys("tab"); time.sleep(.5)
    check("a bare Tab does not switch windows", "switch " not in uart()[n:], uart()[n:])
    n = len(uart()); keys("meta_l", "tab")
    check("Cmd+Tab brings Calendar to the front", wait("switch calendar", 5, n) and wait("focus app", 5, n), uart()[n:])
    time.sleep(.5)
    check("the screen changed with it", shot("cal-front").tobytes() != before.tobytes())
    n = len(uart()); keys("meta_l", "tab")
    check("Cmd+Tab again brings Notes back", wait("switch notes", 5, n), uart()[n:])
    n = len(uart()); keys("ctrl", "tab")
    check("Ctrl+Tab switches too, for a keyboard with no Cmd", wait("switch calendar", 5, n), uart()[n:])
finally:
    q.terminate()
    try: q.wait(5)
    except subprocess.TimeoutExpired: q.kill()

if fails: print("FAIL: " + "; ".join(fails)); sys.exit(1)
print("PASS: Cmd+Tab and Ctrl+Tab walk the Pi's open app windows to the front, and a bare Tab stays with the app")
