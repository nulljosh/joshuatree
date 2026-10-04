#!/usr/bin/env python3
"""ARM64 M2, keyboard, mouse, network and disk: the aarch64 kernel finds a virtio keyboard, a virtio tablet, a virtio network
card and a virtio disk on QEMU's virt machine, reads real events from the first two, gets a real answer over the third and
reads a known sector off the fourth.

Boots arch/arm64 with a 4 KiB disk image whose first sector says "JT-DISK-SECTOR-0 hello from the disk" (checks the kernel
prints it back) and a virtio network card on QEMU's user-mode network, checks the kernel asked the router (10.0.2.2)
for its hardware address over ARP and printed the answer QEMU gives (52:55:0a:00:02:02), then with the virtio-keyboard-device
and virtio-tablet-device waits for "M2 input ready, devices 2" on the UART, then presses keys through QMP
send-key (the same path a real keystroke in the QEMU window takes) and checks the kernel prints each Linux key code
going down and up: j is 36, t is 20. Then moves the pointer to the middle of the top half and clicks, through QMP
input-send-event, and checks the kernel prints the position scaled to the 800x600 screen and the left button (272).
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
log, sock, disk = tmp + "/uart", tmp + "/qmp", tmp + "/disk.img"
open(disk, "wb").write(b"JT-DISK-SECTOR-0 hello from the disk".ljust(4096, b"\0"))
q = subprocess.Popen(["qemu-system-aarch64", "-machine", "virt", "-cpu", "cortex-a72", "-m", "256",
                      "-global", "virtio-mmio.force-legacy=false", "-device", "virtio-keyboard-device", "-device", "virtio-tablet-device",
                      "-netdev", "user,id=n", "-device", "virtio-net-device,netdev=n",
                      "-drive", "file=%s,if=none,format=raw,id=d" % disk, "-device", "virtio-blk-device,drive=d",
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
    if wait_for("M2 net gateway 10.0.2.2 is 52:55:0a:00:02:02"): print("  ok: ARP to the router answered 52:55:0a:00:02:02")
    else: fails.append(f"no ARP answer from the router, got {uart()!r}")
    if wait_for("M2 blk sector0 JT-DISK-SECTOR-0 hello from the"): print("  ok: sector 0 read back off the virtio disk")
    else: fails.append(f"the disk's first sector never came back, got {uart()!r}")
    if not wait_for("M2 input ready, devices 2"):
        fails.append(f"kernel did not find both input devices, got {uart()!r}")
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
        cmd("input-send-event", events=[{"type": "abs", "data": {"axis": "x", "value": 16384}},
                                        {"type": "abs", "data": {"axis": "y", "value": 8192}}])
        if wait_for("mouse 400,150", 30): print("  ok: pointer at the middle of the top half printed 'mouse 400,150'")
        else: fails.append(f"moving the pointer: no 'mouse 400,150' on the UART, got {uart()[-200:]!r}")
        cmd("input-send-event", events=[{"type": "btn", "data": {"down": True, "button": "left"}}])
        cmd("input-send-event", events=[{"type": "btn", "data": {"down": False, "button": "left"}}])
        for want in ("key 272 down", "key 272 up"):
            if wait_for(want, 30): print(f"  ok: left click printed {want!r}")
            else: fails.append(f"clicking: no {want!r} on the UART, got {uart()[-200:]!r}")
finally:
    q.kill(); q.wait()
    shutil.rmtree(tmp, ignore_errors=True)
    subprocess.run(["make", "-C", arch, "clean"], capture_output=True)
for m in fails: print("FAIL: " + m)
if fails: sys.exit(1)
print("PASS: the aarch64 kernel drives a virtio disk (a known sector read back), a virtio network card (a real ARP answer from the router) and a virtio keyboard and tablet with one driver: key presses, pointer moves and clicks all arrive")
