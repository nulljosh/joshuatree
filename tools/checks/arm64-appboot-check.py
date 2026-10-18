#!/usr/bin/env python3
"""4.5: three apps opened at boot on QEMU's virt machine, and the kernel stays up.

Builds the apptest kernel with APP_NAME=calendar,toroid,notes (the virt .elf, not the Pi image arm64-windows-check.py boots).
Before 4.5 the boot self-tests ran after the apps opened, with `ap` on the third app's slot, and the kernel took a data abort
writing into a page that slot never mapped. Asserts: all three apps start, "M3 self-tests skipped: apps are open" is printed,
no KERNEL CRASH within 20 s.
Discriminating: drop the apps_live() early return in user_demo and the self-tests load over Calendar in slot 0 (it ends);
drop the `ap = &apps[0]` too and the kernel crash comes back.
Usage: tools/checks/arm64-appboot-check.py   (from the repo root)
"""
import os, shutil, subprocess, sys, tempfile, time

root = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..")
arch = os.path.join(root, "arch/arm64")
if not all(shutil.which(t) for t in ("clang", "ld.lld", "qemu-system-aarch64")):
    print("SKIP: clang, ld.lld or qemu-system-aarch64 not installed"); sys.exit(0)
subprocess.run(["rm", "-f", os.path.join(arch, "app-kernel8.elf"), os.path.join(arch, "app-main.o")])
if subprocess.run(["make", "-C", arch, "APP_NAME=calendar,toroid,notes", "app-kernel8.elf"], capture_output=True).returncode:
    print("FAIL: the apptest kernel did not build"); sys.exit(1)
log = os.path.join(tempfile.mkdtemp(), "uart.log")
q = subprocess.Popen(["qemu-system-aarch64", "-machine", "virt", "-cpu", "cortex-a72", "-m", "1024", "-device", "ramfb",
                      "-display", "none", "-serial", "file:" + log, "-kernel", os.path.join(arch, "app-kernel8.elf")],
                     stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
try:
    time.sleep(20)
finally:
    q.kill(); q.wait()
out = open(log, errors="replace").read()
fails = [f"missing {w!r}" for w in ("M3c app calendar starts", "M3c app toroid starts", "M3c app notes starts",
                                     "M3 self-tests skipped: apps are open") if w not in out]
for a in ("calendar", "toroid", "notes"):
    if f"M3c app {a} stopped" in out or f"M3c app {a} exit" in out: fails.append(f"{a} ended: the self-tests loaded over it")
if "KERNEL CRASH" in out: fails.append("KERNEL CRASH: " + out[out.index("KERNEL CRASH"):][:200])
subprocess.run(["rm", "-f", os.path.join(arch, "app-kernel8.elf"), os.path.join(arch, "app-main.o")])
if fails:
    for f in fails: print("FAIL:", f)
    sys.exit(1)
print("PASS: three apps open at boot on QEMU virt, the boot self-tests stand aside, and the kernel stays up")
