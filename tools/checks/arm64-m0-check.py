#!/usr/bin/env python3
"""ARM64 M0: the aarch64 kernel boots under qemu-system-aarch64 and prints on the PL011 UART.

Builds arch/arm64, boots it headless with a deadline, and asserts the three serial lines.
Skips (exit 0) when clang's aarch64 target, ld.lld or qemu-system-aarch64 is missing.
Usage: tools/checks/arm64-m0-check.py   (from the repo root)
"""
import os, shutil, subprocess, sys, tempfile, time

root = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..")
if not all(shutil.which(t) for t in ("clang", "ld.lld", "qemu-system-aarch64")):
    print("SKIP: clang, ld.lld or qemu-system-aarch64 not installed"); sys.exit(0)
if subprocess.run(["make", "-C", os.path.join(root, "arch/arm64")], capture_output=True).returncode:
    print("FAIL: arch/arm64 does not build"); sys.exit(1)
log = tempfile.mktemp()
q = subprocess.Popen(["qemu-system-aarch64", "-machine", "virt", "-cpu", "cortex-a72", "-m", "256", "-display", "none",
                      "-serial", "file:" + log, "-kernel", os.path.join(root, "arch/arm64/kernel8.elf")],
                     stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
out = ""
for _ in range(100):
    time.sleep(0.1)
    out = open(log, errors="replace").read() if os.path.exists(log) else ""
    if "M0 ok" in out: break
q.kill(); q.wait()
try: os.remove(log)
except OSError: pass
need = ["Joshua Tree on ARM64", "EL1", "M0 ok"]
miss = [n for n in need if n not in out]
if miss: print("FAIL: missing serial lines", miss, "got", repr(out)); sys.exit(1)
print("PASS: the aarch64 kernel booted under QEMU virt at EL1 and printed over the PL011 UART")
