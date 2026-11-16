#!/usr/bin/env python3
"""13.9.3, Joshua's board test: Paddle froze on the Pi, the arrows did nothing and the whole desktop stalled. Each present
flushed the whole window (about 5 MB at 1080p, a cache clean per line) 100 times a second, which starved the USB keyboard.
appwin.c's blit now writes only the pixels that changed and a present flushes their bounding box.

On the 1080p dirtyrect build (window_scale 2, as on a Pi at 1080p): Spotlight opens Paddle, space serves, Down moves the left
paddle (the strip it lives in changes), the game runs three seconds, Esc closes it, and the kernel's exit line
`M3c app paddle presented N frames, P pixels each` must show many presents of a small area: under 2 percent of the window
each. Reverting the blit change makes every present the whole window and fails it.
Skips (exit 0) when qemu-system-aarch64 is missing. Usage: tools/checks/arm64-present-check.py (from anywhere)."""
import json, os, re, shutil, socket, subprocess, sys, time
from pathlib import Path
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from scratch import scratch_dir

ROOT = Path(__file__).resolve().parents[2]
if not shutil.which("qemu-system-aarch64"):
    print("SKIP: qemu-system-aarch64 missing"); sys.exit(0)
subprocess.run(["make", "-s", "-C", str(ROOT / "arch/arm64"), "dirtyrecttest"], check=True)
TMP = Path(scratch_dir("arm64-present")); log = TMP / "uart"; sock = TMP / "qmp"
for p in (log, sock):
    if p.exists(): p.unlink()
q = subprocess.Popen(["qemu-system-aarch64", "-machine", "virt", "-cpu", "cortex-a72", "-m", "512", "-nic", "none",
    "-global", "virtio-mmio.force-legacy=false", "-device", "ramfb", "-device", "virtio-keyboard-device",
    "-device", "virtio-tablet-device", "-display", "none", "-serial", "file:" + str(log),
    "-qmp", "unix:" + str(sock) + ",server,nowait", "-kernel", str(ROOT / "arch/arm64/dirtyrect-kernel8.elf")],
    stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
f = s = None
def uart(): return log.read_text(errors="replace") if log.exists() else ""
def wait(text, secs=20, after=0):
    end = time.time() + secs
    while time.time() < end:
        if text in uart()[after:]: return
        assert q.poll() is None, "QEMU exited"
        time.sleep(.1)
    raise AssertionError("timed out waiting for %r\n%s" % (text, uart()[-800:]))
try:
    wait("M2 input ready", 60)
    s = socket.socket(socket.AF_UNIX); s.settimeout(10); s.connect(str(sock)); f = s.makefile("rw"); f.readline()
    def command(name, args=None):
        f.write(json.dumps({"execute": name, **({"arguments": args} if args else {})}) + "\n"); f.flush()
        while True:
            reply = json.loads(f.readline())
            assert "error" not in reply, reply
            if "return" in reply: return reply["return"]
    def key(name, hold=50):
        command("send-key", {"keys": [{"type": "qcode", "data": name}], "hold-time": hold}); time.sleep(hold / 1000 + .15)
    def strip():   # the screen below the menu bar (its clock is the only other thing that could change); the ball waits for space
        path = TMP / "shot.ppm"; command("screendump", {"filename": str(path)})
        data = path.read_bytes(); w, h = 1920, 1080
        return data[len(data) - w * h * 3 + 60 * w * 3:]
    command("qmp_capabilities")
    key("f1")
    for k in "paddle": key(k)
    n = len(uart()); key("ret")
    wait("M3c window paddle first frame", after=n); time.sleep(1.5)
    a = strip()
    for _ in range(3): key("down", 60)
    time.sleep(.6)
    assert strip() != a, "Down did not move the paddle\n" + uart()[-600:]
    key("spc", 60); time.sleep(3)
    n = len(uart())
    for _ in range(6):
        key("esc"); time.sleep(.3)
        if "M3c app paddle exit 0" in uart()[n:]: break
    wait("M3c app paddle exit 0", after=n)
    m = re.search(r"M3c app paddle presented (\d+) frames, (\d+) pixels each", uart()[n:])
    assert m, "no present line at exit\n" + uart()[-600:]
    frames, each = int(m.group(1)), int(m.group(2))
    window = 1640 * 770   # Paddle's content at window_scale 2, roughly; a whole-window present is about this
    assert frames >= 60, "Paddle presented only %d frames in three seconds" % frames
    assert each < window // 50, "each present flushed %d pixels: the whole window again, not what changed (%d frames)" % (each, frames)
    print("PASS: Paddle's arrows move it at 1080p, and %d presents flushed %d pixels each, not the whole window" % (frames, each))
finally:
    if f: f.close()
    if s: s.close()
    if q.poll() is None: q.terminate()
    try: q.wait(timeout=5)
    except subprocess.TimeoutExpired: q.kill(); q.wait()
