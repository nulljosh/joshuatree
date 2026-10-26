#!/usr/bin/env python3
"""The Pi desktop takes keys from its first moment, and says so (roadmap: "the first few seconds after the desktop
appears take no keys"; the stamps in arch/arm64/boot.c find out why on the board).

Boots the virt build with a keyboard and, with no settling sleep, sends Cmd+Space the moment `M2 input ready` is out:
  1. the UART says `first key N ms after the desktop`, with N under 5000;
  2. Spotlight opens on that first chord ("spotlight open");
  3. no main-loop step held the keys up (no `slow step` line) on a healthy boot;
  4. `boottime` in the Terminal repeats the stamp, so the board's numbers can be read without a serial cable.

Discriminating: drop the first-key stamp in input_event and step 1 fails.

Usage: tools/checks/arm64-firstkey-check.py   (from the repo root)
"""
import json, os, re, shutil, socket, subprocess, sys, time
from pathlib import Path
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from scratch import scratch_dir

ROOT = Path(__file__).resolve().parents[2]
os.chdir(ROOT)
TMP = Path(scratch_dir("firstkey"))
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
    "-device", "virtio-keyboard-device", "-display", "none",
    "-serial", f"file:{log}", "-qmp", f"unix:{sock},server,nowait", "-kernel", "arch/arm64/kernel8.elf"],
    stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
uart = lambda: log.read_text(errors="replace") if log.exists() else ""
def wait(text, secs=15):
    for _ in range(int(secs * 20)):
        if text in uart(): return True
        time.sleep(.05)
    return False
try:
    s = None
    for _ in range(100):
        try: s = socket.socket(socket.AF_UNIX); s.settimeout(10); s.connect(str(sock)); break
        except OSError: s = None; time.sleep(.05)
    if s is None: check("QEMU's QMP socket came up", False); sys.exit(1)
    f = s.makefile("rw"); f.readline()
    def command(name, args=None):
        f.write(json.dumps({"execute": name, **({"arguments": args} if args else {})}) + "\n"); f.flush()
        while True:
            r = json.loads(f.readline())
            if "return" in r or "error" in r: return r.get("return")
    command("qmp_capabilities")
    if not wait("M2 input ready", 30): check("the desktop came up", False); sys.exit(1)
    command("send-key", {"keys": [{"type": "qcode", "data": "meta_l"}, {"type": "qcode", "data": "spc"}], "hold-time": 80})
    m = None
    for _ in range(100):
        m = re.search(r"first key (\d+) ms after the desktop", uart())
        if m: break
        time.sleep(.05)
    check("the first key is stamped, under 5 s after the desktop", bool(m) and int(m[1]) < 5000, uart()[-300:])
    check("Spotlight opens on the very first chord", wait("spotlight open", 5), uart()[-300:])
    time.sleep(1)
    check("no main-loop step held the keys up", "slow step" not in uart(), re.findall(r"slow step .*", uart()))
    def key(k): command("send-key", {"keys": [{"type": "qcode", "data": k}], "hold-time": 50}); time.sleep(.15)
    key("esc"); key("f2"); time.sleep(.5)
    for c in "boottime": key(c)
    n = uart().count("first key ")
    key("ret")
    for _ in range(60):
        if uart().count("first key ") > n: break
        time.sleep(.05)
    check("`boottime` in the Terminal repeats the stamp", uart().count("first key ") > n, uart()[-300:])
finally:
    q.terminate()
    try: q.wait(5)
    except subprocess.TimeoutExpired: q.kill()

if fails: print("FAIL: " + "; ".join(fails)); sys.exit(1)
print("PASS: the Pi desktop takes Cmd+Space the moment it is up, stamps the first key, and no loop step stalls the keys")
