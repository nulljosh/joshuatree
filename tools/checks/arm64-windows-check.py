#!/usr/bin/env python3
"""ARM64 4.0 gate: three apps run at once, one crashes, the other two do not notice.

`make -C arch/arm64 wintest` boots Calendar, Toroid and Notes together on QEMU's raspi4b (each in its own 16 MiB arena, its own
window). Once all three have drawn, the kernel queues Toroid the backquote it deliberately crashes on, then pokes the other two,
and prints `wingate: slotN live|gone <frames presented since>` for each. Asserts: Toroid faults and the kernel survives, its
slot is gone, Calendar and Notes are still there and kept presenting frames after the crash.
Skips (exit 0) when clang, ld.lld or qemu-system-aarch64 is missing.
Usage: tools/checks/arm64-windows-check.py   (from the repo root)
"""
import os, re, shutil, subprocess, sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from scratch import scratch_dir

arch = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "arch/arm64")
if not all(shutil.which(t) for t in ("clang", "ld.lld", "qemu-system-aarch64")):
    print("SKIP: clang, ld.lld or qemu-system-aarch64 not installed"); sys.exit(0)
if subprocess.run(["make", "-C", arch, "wintest"], capture_output=True).returncode:
    print("FAIL: arch/arm64 `make wintest` does not build"); sys.exit(1)
log = scratch_dir("arm64-windows") + "/uart"
if os.path.exists(log): os.remove(log)
q = subprocess.Popen(["qemu-system-aarch64", "-machine", "raspi4b", "-serial", "null", "-display", "none", "-serial", "file:" + log,
                      "-kernel", os.path.join(arch, "wingate-kernel8.img")], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
try:
    end, out = time.time() + 240, ""
    while time.time() < end and "wingate:" not in out:
        time.sleep(0.5); out = open(log, errors="replace").read() if os.path.exists(log) else ""
finally:
    q.kill(); q.wait()
fails = []
def check(name, ok):
    print(("  ok: " if ok else "  FAIL: ") + name)
    if not ok: fails.append(name)
m = re.search(r"wingate: slot0 (\w+) (\d+) slot1 (\w+) (\d+) slot2 (\w+) (\d+)", out)
check("all three apps drew a window", all("M3c window %s first frame" % n in out for n in ("calendar", "toroid", "notes")))
check("Toroid crashed on purpose and the kernel survived", "toroid: crashing on purpose" in out and "KERNEL CRASH" not in out)
check("the gate line printed", bool(m))
if m:
    check("Toroid's slot is gone", m[3] == "gone")
    check("Calendar is still there and kept presenting frames", m[1] == "live" and int(m[2]) > 0)
    check("Notes is still there and kept presenting frames", m[5] == "live" and int(m[6]) > 0)
if fails: print(out[-1500:]); sys.exit(1)
print("PASS: one app crashed, the other two did not notice")
