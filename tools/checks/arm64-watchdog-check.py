#!/usr/bin/env python3
"""4.5: an app that freezes is stopped by the watchdog and the desktop carries on.

Builds the hangtest Pi image (`make hangtest`, -DHANG_TEST): Calendar (slot 0) is patched to spin on its first instruction with
no syscall, Notes (slot 1) is a normal app. Boots it on QEMU's raspi4b. Asserts: the UART says "M4 watchdog: calendar ran 3 s
without a syscall, stopped", "M3c app calendar stopped by a fault", and then "hang: slot0 gone slot1 live N" with N >= 1, so
Notes was never touched and kept presenting frames; no KERNEL CRASH.
Discriminating: take the `wd_arm()` out of app_tick and Calendar spins forever (the check times out, the line never comes).
Usage: tools/checks/arm64-watchdog-check.py   (from the repo root)
"""
import os, re, shutil, subprocess, sys, tempfile, time

root = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..")
arch = os.path.join(root, "arch/arm64")
if not all(shutil.which(t) for t in ("clang", "ld.lld", "qemu-system-aarch64")):
    print("SKIP: clang, ld.lld or qemu-system-aarch64 not installed"); sys.exit(0)
subprocess.run(["rm", "-f", os.path.join(arch, "hang-main.o"), os.path.join(arch, "hang-pi-main.o"), os.path.join(arch, "hang-kernel8.img")])
if subprocess.run(["make", "-C", arch, "hangtest", "hang-kernel8.img"], capture_output=True).returncode:
    print("FAIL: arch/arm64 `make hangtest` does not build"); sys.exit(1)
log = os.path.join(tempfile.mkdtemp(), "uart")
q = subprocess.Popen(["qemu-system-aarch64", "-machine", "raspi4b", "-serial", "null", "-display", "none", "-serial", "file:" + log,
                      "-kernel", os.path.join(arch, "hang-kernel8.img")], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
try:
    end, out = time.time() + 240, ""
    while time.time() < end and "hang: slot0" not in out and "KERNEL CRASH" not in out:
        time.sleep(0.5); out = open(log, errors="replace").read() if os.path.exists(log) else ""
finally:
    q.kill(); q.wait()
fails = []
for w in ("M4 watchdog: calendar ran 3 s without a syscall, stopped", "M3c app calendar stopped by a fault"):
    if w not in out: fails.append(f"missing {w!r}")
m = re.search(r"hang: slot0 gone slot1 live (\d+)", out)
if not m: fails.append("never printed 'hang: slot0 gone slot1 live N': Notes did not survive, or the watchdog never fired")
elif int(m.group(1)) < 1: fails.append("Notes presented no frames after the watchdog fired")
if "KERNEL CRASH" in out: fails.append("KERNEL CRASH: " + out[out.index("KERNEL CRASH"):][:200])
subprocess.run(["rm", "-f", os.path.join(arch, "hang-kernel8.img")])
if fails:
    for f in fails: print("FAIL:", f)
    sys.exit(1)
print(f"PASS: a frozen Calendar was stopped by the watchdog after 3 s, and Notes kept presenting ({m.group(1)} frames after)")
