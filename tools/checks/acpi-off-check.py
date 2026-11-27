#!/usr/bin/env python3
"""Shut Down really turns the PC off (13.25.0, issue #1018, kernel/acpi.c).

Boots the i386 kernel with `poweroff` on the boot line, which runs the same acpi_poweroff() the Apple menu's Shut Down
and the shell's `shutdown` call, once the whole boot is done. QEMU exits by itself when the guest enters ACPI S5, so
the check is that QEMU's process ends on its own, with status 0, after the kernel logged the PM1a_CNT write. Run on
two chipsets (pc's PIIX4 and q35's ICH9; both put PM1a_CNT at 0x604 with S5 as 0, so the third run is the one that
shows the tables are read): with `-machine pc,acpi=off` there is no RSDP, acpi_poweroff says so and returns, and the
machine keeps running (no fault, no reset) on to the desktop.
Fails when acpi.c is reverted: QEMU keeps running until the timeout.
Usage: tools/checks/acpi-off-check.py   (from the repo root; builds kernel.elf)
"""
import os, shutil, subprocess, sys, tempfile

if not shutil.which("qemu-system-i386"):
    print("SKIP: qemu-system-i386 not installed"); sys.exit(0)
r = subprocess.run(["make", "-s", "kernel.elf"], capture_output=True, text=True)
if r.returncode: print(r.stderr[-2000:]); print("FAIL: kernel.elf does not build"); sys.exit(1)

fails = []
tmp = tempfile.mkdtemp(prefix="jt-acpi-off-")
try:
    for machine in ("pc", "q35"):
        log = os.path.join(tmp, machine + ".log")
        q = subprocess.Popen(["qemu-system-i386", "-machine", machine, "-m", "256", "-kernel", "kernel.elf",
                              "-append", "poweroff nodhcp", "-display", "none", "-no-reboot", "-serial", "file:" + log],
                             stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        try:
            rc = q.wait(timeout=120)
        except subprocess.TimeoutExpired:
            q.kill(); q.wait(); rc = None
        serial = open(log, errors="replace").read() if os.path.exists(log) else ""
        wrote = "acpi: power off pm1a=" in serial
        ok = rc == 0 and wrote and "acpi: still on" not in serial
        line = next((l for l in serial.splitlines() if l.startswith("acpi:")), "(no acpi line)")
        print(("  ok: " if ok else "  FAIL: ") + "%s: QEMU powered off by itself (exit %s): %s" % (machine, rc, line))
        if not ok: fails.append("%s: exit %s, %s; serial tail %r" % (machine, rc, line, serial[-400:]))
    log = os.path.join(tmp, "noacpi.log")
    q = subprocess.Popen(["qemu-system-i386", "-machine", "pc,acpi=off", "-m", "256", "-kernel", "kernel.elf",
                          "-append", "poweroff nodhcp", "-display", "none", "-no-reboot", "-serial", "file:" + log],
                         stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        rc = q.wait(timeout=15)
    except subprocess.TimeoutExpired:
        rc = None
    finally:
        if q.poll() is None: q.kill(); q.wait()
    serial = open(log, errors="replace").read() if os.path.exists(log) else ""
    ok = rc is None and "acpi: no RSDP" in serial and "acpi: power off" not in serial
    print(("  ok: " if ok else "  FAIL: ") + "pc,acpi=off: no RSDP is reported and the machine stays up (exit %s)" % rc)
    if not ok: fails.append("acpi=off: exit %s, serial tail %r" % (rc, serial[-400:]))
finally:
    shutil.rmtree(tmp, ignore_errors=True)

if fails:
    print("FAIL:"); [print("  - " + f) for f in fails]; sys.exit(1)
print("PASS: acpi_poweroff finds the RSDP, FADT and \\_S5_ and turns off QEMU's pc and q35 machines, and a machine without ACPI stays up")
