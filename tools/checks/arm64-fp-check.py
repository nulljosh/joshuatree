#!/usr/bin/env python3
"""ARM64 floating point state across interrupts: vectors.S keeps q0-q31, FPCR and FPSR over an IRQ.

The fp build (`make -C arch/arm64 fpsave`, main.c's FP_TEST and arch/arm64/fptest.c) loads known values into all 32 vector
registers and sets FPCR and FPSR, then spins while the timer interrupts fire; its interrupt handler stands in for one that
uses floating point and wipes every one of them. After the third tick the registers are compared with what was put in.
  - fp build: must print "FPTEST ok" (everything came back exact).
  - fpnosave build, the same kernel with FP_NOSAVE (vectors.S skips the save): must print "FPTEST FAIL", which proves the
    test can tell, so the "ok" above means the save and restore really work.
Skips (exit 0) when clang's aarch64 target, ld.lld or qemu-system-aarch64 is missing.
Usage: tools/checks/arm64-fp-check.py   (from the repo root)
"""
import os, shutil, subprocess, sys, tempfile, time

root = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..")
arch = os.path.join(root, "arch/arm64")
if not all(shutil.which(t) for t in ("clang", "ld.lld", "qemu-system-aarch64")):
    print("SKIP: clang, ld.lld or qemu-system-aarch64 not installed"); sys.exit(0)
subprocess.run(["make", "-C", arch, "clean"], capture_output=True)
if subprocess.run(["make", "-C", arch, "fpsave", "fpnosave"], capture_output=True).returncode:
    print("FAIL: arch/arm64 `make fpsave fpnosave` does not build"); sys.exit(1)
fails = []

def boot(image):
    log = tempfile.mktemp()
    q = subprocess.Popen(["qemu-system-aarch64", "-machine", "virt", "-cpu", "cortex-a72", "-m", "256", "-display", "none",
                          "-serial", "file:" + log, "-kernel", os.path.join(arch, image)], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    out = ""
    try:
        for _ in range(150):
            time.sleep(0.1)
            out = open(log, errors="replace").read() if os.path.exists(log) else ""
            if "M1a ok" in out: break
    finally:
        q.kill(); q.wait()
        try: os.remove(log)
        except OSError: pass
    return out

out = boot("fp-kernel8.elf")
if "FPTEST ok" in out and "tick 3" in out: print("  ok: with vectors.S saving them, q0-q31, FPCR and FPSR came back exact after three timer interrupts that wiped them")
else: fails.append(f"fp build: FP registers changed across the interrupts, got {out[-300:]!r}")
out = boot("fpnosave-kernel8.elf")
if "FPTEST FAIL" in out: print("  ok: with the save left out the same test fails (the test discriminates)")
else: fails.append(f"fpnosave build did not fail the test, so it proves nothing: {out[-300:]!r}")
subprocess.run(["make", "-C", arch, "clean"], capture_output=True)
for m in fails: print("FAIL: " + m)
if fails: sys.exit(1)
print("PASS: an interrupt cannot corrupt the interrupted code's floating point state: q0-q31, FPCR and FPSR survive a handler that overwrites them, and the control build without the save fails")
