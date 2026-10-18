#!/usr/bin/env python3
"""ARM64 Spotlight lists every app, Calculator included, and Enter opens it (headless QEMU, own private scratch)."""
import json
from pathlib import Path
import socket
import subprocess
import time
from scratch import scratch_dir

ROOT = Path(__file__).resolve().parents[2]
TMP = Path(scratch_dir("spotlight"))
APPS = 22   # 10 dock tiles (the Apps tile is the Launchpad since 4.1.2, not an app) + Clock, Brick, Music, Browser, Books + Calculator + Finder + Settings + Mines, Toroid, Hamurapi, Keyrate
subprocess.run(["make", "-s", "-C", str(ROOT / "arch/arm64"), "kernel8.elf"], check=True)
log = TMP / "uart"; sock = TMP / "qmp"
q = subprocess.Popen(["qemu-system-aarch64", "-machine", "virt", "-cpu", "cortex-a72", "-m", "256",
    "-nic", "none", "-global", "virtio-mmio.force-legacy=false", "-device", "ramfb",
    "-device", "virtio-keyboard-device", "-device", "virtio-tablet-device", "-display", "none",
    "-serial", "file:" + str(log), "-qmp", "unix:" + str(sock) + ",server,nowait",
    "-kernel", str(ROOT / "arch/arm64/kernel8.elf")], stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
s = None; f = None
def uart(): return log.read_text(errors="replace")
try:
    for _ in range(300):
        if log.exists() and "M2 input ready" in uart(): break
        assert q.poll() is None, "QEMU exited"
        time.sleep(.1)
    else: raise AssertionError("desktop did not boot")
    s = socket.socket(socket.AF_UNIX); s.settimeout(10); s.connect(str(sock)); f = s.makefile("rw"); f.readline()
    def command(name, args=None):
        f.write(json.dumps({"execute": name, **({"arguments": args} if args else {})}) + "\n"); f.flush()
        while True:
            reply = json.loads(f.readline())
            assert "error" not in reply, reply
            if "return" in reply: return reply["return"]
    def key(name):
        command("send-key", {"keys": [{"type": "qcode", "data": name}], "hold-time": 50}); time.sleep(.15)
    command("qmp_capabilities")
    key("f1")
    assert f"spotlight:  {APPS}\n" in uart(), "an empty query must list all %d apps\n%s" % (APPS, uart()[-600:])
    for k in "calc": key(k)
    assert "spotlight: calc 1\n" in uart(), "typing calc must show exactly Calculator\n" + uart()[-600:]
    key("ret")
    time.sleep(.3)
    assert "calc open" in uart().split("spotlight: calc 1\n")[-1], "Enter did not open the Calculator"
    print("PASS: empty Spotlight lists all %d apps, typing calc shows Calculator and Enter opens it" % APPS, flush=True)
finally:
    if f: f.close()
    if s: s.close()
    if q.poll() is None: q.terminate()
    try: q.wait(timeout=5)
    except subprocess.TimeoutExpired: q.kill(); q.wait()
    q.stderr.close()
