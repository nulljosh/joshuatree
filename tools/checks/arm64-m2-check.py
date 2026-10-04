#!/usr/bin/env python3
"""ARM64 M2, keyboard: the aarch64 kernel finds a virtio keyboard on QEMU's virt machine and reads real key events.

Boots arch/arm64 with a virtio-keyboard-device, waits for "M2 kbd ready" on the UART, then presses keys through QMP
send-key (the same path a real keystroke in the QEMU window takes) and checks the kernel prints each Linux key code
going down and up: j is 36, t is 20.
Skips (exit 0) when clang's aarch64 target, ld.lld or qemu-system-aarch64 is missing.
Usage: tools/checks/arm64-m2-check.py   (from the repo root)
"""
import json, os, shutil, socket, subprocess, sys, tempfile, time

root = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..")
arch = os.path.join(root, "arch/arm64")
if not all(shutil.which(t) for t in ("clang", "ld.lld", "qemu-system-aarch64")):
    print("SKIP: clang, ld.lld or qemu-system-aarch64 not installed"); sys.exit(0)
subprocess.run(["make", "-C", arch, "clean"], capture_output=True)
if subprocess.run(["make", "-C", arch], capture_output=True).returncode:
    print("FAIL: arch/arm64 `make` does not build"); sys.exit(1)

tmp = tempfile.mkdtemp()
log, sock = tmp + "/uart", tmp + "/qmp"
q = subprocess.Popen(["qemu-system-aarch64", "-machine", "virt", "-cpu", "cortex-a72", "-m", "256",
                      "-global", "virtio-mmio.force-legacy=false", "-device", "virtio-keyboard-device",
                      "-display", "none", "-serial", "file:" + log, "-qmp", "unix:%s,server,nowait" % sock,
                      "-kernel", os.path.join(arch, "kernel8.elf")], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
fails = []
def uart(): return open(log, errors="replace").read() if os.path.exists(log) else ""
def wait_for(text, tries=100):
    for _ in range(tries):
        if text in uart(): return True
        time.sleep(0.1)
    return False
try:
    if not wait_for("M2 kbd ready"):
        fails.append(f"kernel did not find the keyboard, got {uart()!r}")
    else:
        s = socket.socket(socket.AF_UNIX); s.connect(sock); f = s.makefile("rw")
        f.readline()
        def cmd(c, **a):
            f.write(json.dumps({"execute": c, "arguments": a}) + "\n"); f.flush()
            while True:
                r = json.loads(f.readline())
                if "return" in r or "error" in r: return r
        cmd("qmp_capabilities")
        for key, code in (("j", 36), ("t", 20)):
            cmd("send-key", keys=[{"type": "qcode", "data": key}])
            for want in (f"key {code} down", f"key {code} up"):
                if wait_for(want, 30): print(f"  ok: {key} printed {want!r}")
                else: fails.append(f"pressing {key}: no {want!r} on the UART, got {uart()[-200:]!r}")
finally:
    q.kill(); q.wait()
    shutil.rmtree(tmp, ignore_errors=True)
    subprocess.run(["make", "-C", arch, "clean"], capture_output=True)
for m in fails: print("FAIL: " + m)
if fails: sys.exit(1)
print("PASS: the aarch64 kernel drives a virtio keyboard and prints real key presses and releases")
