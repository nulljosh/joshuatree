#!/usr/bin/env python3
"""ARM64 M3a, the first userland: an EL0 program runs unprivileged, talks to the kernel through svc, and cannot touch kernel memory.

Boots arch/arm64 on QEMU's virt machine and reads the UART. Checks, in this order:
  - "EL0: hello from EL0": the program ran at EL0 and its write(console) syscall printed.
  - "EL0: write returned 20": the syscall's return value (the byte count) came back in x0.
  - "EL0: kernel memory refused, write returned -14": asking the kernel to print a kernel-only page is refused (-EFAULT).
  - "M3 EL0 fault: data abort, permission fault level 3, address 0x0000000044004000": a direct EL0 load from the kernel-only
    page faults, and the kernel prints the fault.
  - "M3 kernel survived the fault", then a second EL0 task that prints and exits with status 7 ("M3 EL0 exit 7",
    "M3 userland ok"): the kernel kept running and can enter EL0 again.
  - "EL0: reached code after the blocked access" and "M3 userland FAIL" must never appear.
Skips (exit 0) when clang's aarch64 target, ld.lld or qemu-system-aarch64 is missing.
Usage: tools/checks/arm64-m3-check.py   (from the repo root)
"""
import os, shutil, subprocess, sys, tempfile, time

root = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..")
arch = os.path.join(root, "arch/arm64")
if not all(shutil.which(t) for t in ("clang", "ld.lld", "qemu-system-aarch64")):
    print("SKIP: clang, ld.lld or qemu-system-aarch64 not installed"); sys.exit(0)
subprocess.run(["make", "-C", arch, "clean"], capture_output=True)
if subprocess.run(["make", "-C", arch], capture_output=True).returncode:
    print("FAIL: arch/arm64 `make` does not build"); sys.exit(1)

tmp = tempfile.mkdtemp()
log = tmp + "/uart"
q = subprocess.Popen(["qemu-system-aarch64", "-machine", "virt", "-cpu", "cortex-a72", "-m", "256", "-display", "none",
                      "-serial", "file:" + log, "-kernel", os.path.join(arch, "kernel8.elf")],
                     stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
fails = []
out = ""
try:
    for _ in range(150):
        time.sleep(0.1)
        out = open(log, errors="replace").read() if os.path.exists(log) else ""
        if "M3 userland ok" in out or "M3 userland FAIL" in out: break
finally:
    q.kill(); q.wait()
    shutil.rmtree(tmp, ignore_errors=True)
    subprocess.run(["make", "-C", arch, "clean"], capture_output=True)

lines = out.splitlines()
want = ["M3 EL0 task starts",
        "EL0: hello from EL0",
        "EL0: write returned 20",
        "EL0: kernel memory refused, write returned -14",
        "M3 EL0 fault: data abort, permission fault level 3, address 0x0000000044004000",
        "M3 kernel survived the fault",
        "M3 EL0 second task starts",
        "EL0: second task",
        "M3 EL0 exit 7",
        "M3 userland ok"]
at = -1
for w in want:
    nxt = next((i for i in range(at + 1, len(lines)) if lines[i] == w), -1)
    if nxt < 0: fails.append(f"missing or out of order serial line {w!r}, got {out[-600:]!r}"); break
    at = nxt; print(f"  ok: {w}")
for bad in ("EL0: reached code after the blocked access", "M3 userland FAIL"):
    if bad in out: fails.append(f"{bad!r} printed: the EL0 program got past the blocked access")
for m in fails: print("FAIL: " + m)
if fails: sys.exit(1)
print("PASS: the aarch64 kernel runs an unprivileged EL0 program that prints through a write syscall and exits, refuses to print kernel memory for it, survives its direct access to a kernel-only page as a printed permission fault, and runs a second EL0 task after")
