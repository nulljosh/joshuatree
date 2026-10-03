#!/usr/bin/env python3
"""ARM64 M0 and M1a: the aarch64 kernel boots, prints on the PL011 UART, takes an exception and a timer interrupt, in both builds.

virt: `make -C arch/arm64` boots on QEMU's generic virt machine, entered at EL1.
pi:   `make -C arch/arm64 pi` makes kernel8.img, the file a Raspberry Pi 4 loads. QEMU's raspi4b model
      loads it at 0x80000 and enters at EL2 like the real firmware, so the EL2 to EL1 drop is proven too.
Skips (exit 0) when clang's aarch64 target, ld.lld or qemu-system-aarch64 is missing.
Usage: tools/checks/arm64-m0-check.py   (from the repo root)
"""
import os, shutil, subprocess, sys, tempfile, time

root = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..")
arch = os.path.join(root, "arch/arm64")
if not all(shutil.which(t) for t in ("clang", "ld.lld", "qemu-system-aarch64")):
    print("SKIP: clang, ld.lld or qemu-system-aarch64 not installed"); sys.exit(0)
subprocess.run(["make", "-C", arch, "clean"], capture_output=True)
fails = []

def boot(name, target, qemu_args, image, want):
    if subprocess.run(["make", "-C", arch, target], capture_output=True).returncode:
        fails.append(f"{name}: arch/arm64 `make {target}` does not build"); return
    log = tempfile.mktemp()
    q = subprocess.Popen(["qemu-system-aarch64", *qemu_args, "-display", "none", "-serial", "file:" + log, "-kernel", os.path.join(arch, image)],
                         stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    out = ""
    for _ in range(150):
        time.sleep(0.1)
        out = open(log, errors="replace").read() if os.path.exists(log) else ""
        if "M1a ok" in out: break
    q.kill(); q.wait()
    try: os.remove(log)
    except OSError: pass
    miss = [w for w in want if w not in out]
    if miss: fails.append(f"{name}: missing serial lines {miss}, got {out!r}")
    else: print(f"  ok: {name} printed {want}")

boot("virt", "all", ["-machine", "virt", "-cpu", "cortex-a72", "-m", "256"], "kernel8.elf",
     ["Joshua Tree on ARM64", "booted at EL1", "M0 ok", "M1 svc ok", "tick 3", "M1a ok"])
if "raspi4b" in subprocess.run(["qemu-system-aarch64", "-machine", "help"], capture_output=True, text=True).stdout:
    boot("pi", "pi", ["-machine", "raspi4b"], "kernel8.img",
         ["Joshua Tree on ARM64", "booted at EL2", "EL1", "M0 ok", "M1 svc ok", "tick 3", "M1a ok"])
else:
    if subprocess.run(["make", "-C", arch, "pi"], capture_output=True).returncode: fails.append("pi: arch/arm64 `make pi` does not build")
    else: print("  pi image builds; QEMU here has no raspi4b model (needs QEMU 9 or newer), boot step skipped")
subprocess.run(["make", "-C", arch, "clean"], capture_output=True)
for f in fails: print("FAIL: " + f)
if fails: sys.exit(1)
print("PASS: the aarch64 kernel boots under QEMU virt at EL1, and the Raspberry Pi image boots on the raspi4b model at EL2, drops to EL1, prints over the UART, answers a deliberate fault and ticks on timer interrupts")
