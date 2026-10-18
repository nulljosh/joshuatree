#!/usr/bin/env python3
"""ARM64 M3a, the first userland: an EL0 program runs unprivileged, talks to the kernel through svc, and cannot touch kernel memory.

Boots arch/arm64 on QEMU's virt machine and reads the UART. Checks, in this order:
  - "EL0: hello from EL0": the program ran at EL0 and its write(console) syscall printed.
  - "EL0: write returned 20": the syscall's return value (the byte count) came back in x0.
  - "EL0: kernel memory refused, write returned -14": asking the kernel to print a kernel-only page is refused (-EFAULT).
  - "EL0: wrapped pointer refused, write returned -14": write(1, 0xFFFFFFFFFFFFFF80, 256) cannot wrap the range check around
    into the kernel; the kernel carries on.
  - "M3 EL0 fault: instruction abort, permission fault level 2, address 0x0000000040000000": an EL0 branch to kernel text
    (the first kernel instruction) is an instruction abort, printed, and the kernel carries on to the next task.
  - "M3 EL0 fault: data abort, permission fault level 3, address 0x0000000044004000": a direct EL0 load from the kernel-only
    page faults, and the kernel prints the fault.
  - 4.4, "M4 arena isolation ok": with slot 0 running, a load from a page mapped for EL0 in slot 1's arena faults.
  - "M3 kernel survived the fault", then a second EL0 task that prints and exits with status 7 ("M3 EL0 exit 7",
    "M3 userland ok"): the kernel kept running and can enter EL0 again.
  - M3b, a C program: user/hello.c itself, built for aarch64 (uapp_hello.bin, a flat -pie image with no relocations), is
    loaded into the app window (M3c's loader) and runs at EL0 with jtsys.h's `svc #0` path and the Joshua Tree numbers. "jt-hello: start" is
    its first write (JT_SYS_WRITE 4, fd 1). The self-test runs before any card is mounted, so its open of HELLO.TXT is
    answered -ENOENT (M3d): it prints "jt-hello: open failed -2" and exits 1 through JT_SYS_EXIT 1 ("M3 EL0 exit 1",
    "M3b userland C ok"). Before M3d the open was -ENOSYS (-38); arm64-m3c-check.py runs it to exit 9 with a card.
  - "EL0: reached code after the blocked access", "M3 userland FAIL" and "M3b userland C FAIL" must never appear.
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
        if "M3b userland C ok" in out or "M3b userland C FAIL" in out or "M3 userland FAIL" in out: break
finally:
    q.kill(); q.wait()
    shutil.rmtree(tmp, ignore_errors=True)
    subprocess.run(["make", "-C", arch, "clean"], capture_output=True)

lines = out.splitlines()
want = ["M3 EL0 task starts",
        "EL0: hello from EL0",
        "EL0: write returned 20",
        "EL0: kernel memory refused, write returned -14",
        "EL0: wrapped pointer refused, write returned -14",
        "M3 EL0 fault: data abort, permission fault level 3, address 0x0000000044004000",
        "M3 kernel survived the fault",
        "M3 EL0 jump task starts",
        "M3 EL0 fault: instruction abort, permission fault level 2, address 0x0000000040000000",
        "M3 kernel survived the jump",
        "M3 EL0 second task starts",
        "EL0: second task",
        "M3 EL0 exit 7",
        "M3 userland ok",
        # 4.4: slot 1 holds a page EL0 could use; from slot 0 a load from it must fault (arena_only's APTable bits)
        "M4 arena probe starts",
        "M4 arena isolation ok",
        # M3b: user/hello.c, built for aarch64, over svc with the Joshua Tree numbers (write 4, exit 1). No card is mounted
        # yet, so hello's open comes back -ENOENT (-2, M3d) and the program takes its own failure path: exit 1.
        "M3b EL0 C task starts",
        "jt-hello: start",
        "jt-hello: open failed -2",
        "M3 EL0 exit 1",
        "M3b userland C ok"]
at = -1
for w in want:
    nxt = next((i for i in range(at + 1, len(lines)) if lines[i] == w), -1)
    if nxt < 0: fails.append(f"missing or out of order serial line {w!r}, got {out[-600:]!r}"); break
    at = nxt; print(f"  ok: {w}")
for bad in ("EL0: reached code after the blocked access", "M3 userland FAIL", "M3b userland C FAIL", "jt-hello: nosys=", "M4 arena isolation FAIL"):
    if bad in out: fails.append(f"{bad!r} printed: the EL0 program got past the blocked access")
for m in fails: print("FAIL: " + m)
if fails: sys.exit(1)
print("PASS: the aarch64 kernel runs an unprivileged EL0 program that prints through a write syscall and exits, refuses to print kernel memory for it, survives its direct access to a kernel-only page as a printed permission fault, and runs a second EL0 task after, then runs user/hello.c built for aarch64 over svc with the Joshua Tree syscall numbers: its write reaches the UART, its open finds no card (-ENOENT) and it exits through JT_SYS_EXIT")
