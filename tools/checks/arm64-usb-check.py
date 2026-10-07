#!/usr/bin/env python3
"""ARM64 M4, USB keyboard and mouse: the aarch64 kernel finds an xHCI controller on PCI Express, enumerates a USB hub
and the keyboard behind it, plus a mouse on its own root port, and turns their HID boot reports into key codes and
pointer moves on the UART.

The QEMU topology mirrors a real Raspberry Pi 4 as closely as virt allows: the controller (qemu-xhci) sits behind a
PCIe root port, as the Pi's VL805 sits behind the BCM2711's, so the kernel has to number a bridge and open its memory
window before it can reach the registers. The keyboard sits behind a usb-hub, as every keyboard on a Pi sits behind
the VL805's internal hub, so the kernel has to power and reset the hub's ports and address the keyboard with a route
string. The mouse is on a root port directly.

Waits for "usb ready: 1 kbd, 1 mouse", then presses j and t through QMP send-key (letters only, never Enter)
and checks the kernel echoes each one ("usb key 0x0d j") and prints the Linux key codes going down and up (j is 36,
t is 20), then moves the mouse right 20
and down 10 through QMP input-send-event and checks the pointer, which starts mid-screen at 400,300, lands on 420,310,
then clicks and checks the left button (272) going down and up.
Then hot-plug, which only the once-a-second rescan can notice: a second keyboard added to a free hub port through QMP
device_add must be found and typed on, and removed again with device_del ("usb port 5.2 disconnected"); a third added to a
free root port likewise. The hub's per-port status lines ("usb hp N st .. ch ..") must be there too.
No virtio input devices are attached, so every event can only have come through USB. QEMU runs headless
(-display none) and is killed when the check ends.
Skips (exit 0) when clang's aarch64 target, ld.lld or qemu-system-aarch64 is missing.
Usage: tools/checks/arm64-usb-check.py   (from the repo root)
"""
import json, os, re, shutil, socket, subprocess, sys, tempfile, time

root = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..")
arch = os.path.join(root, "arch/arm64")
if not all(shutil.which(t) for t in ("clang", "ld.lld", "qemu-system-aarch64")):
    print("SKIP: clang, ld.lld or qemu-system-aarch64 not installed"); sys.exit(0)
subprocess.run(["make", "-C", arch, "clean"], capture_output=True)
if subprocess.run(["make", "-C", arch], capture_output=True).returncode:
    print("FAIL: arch/arm64 `make` does not build"); sys.exit(1)

tmp = tempfile.mkdtemp()
log, sock = tmp + "/uart", tmp + "/qmp"
q = subprocess.Popen(["qemu-system-aarch64", "-machine", "virt", "-cpu", "cortex-a72", "-m", "256", "-nic", "none",
                      "-device", "pcie-root-port,id=rp,chassis=1",
                      "-device", "qemu-xhci,id=x,bus=rp",
                      "-device", "usb-hub,id=h,bus=x.0,port=1",
                      "-device", "usb-kbd,bus=x.0,port=1.1",
                      "-device", "usb-mouse,bus=x.0,port=2",
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
    if not wait_for("usb ready: 1 kbd, 1 mouse", 200):
        fails.append(f"kernel did not bring up the USB keyboard and mouse, got {uart()[-1500:]!r}")
    else:
        for want, what in ((r"usb pci 1:0\.0 1b36:d class c0330", "the xHCI controller found on bus 1, behind the PCIe root port"),
                           (r"usb xhci at 0_10\d{6}", "its registers mapped from the bridge's memory window"),
                           (r"usb hub addr \d+ port (\d+) full .*\nusb hub port \1, 8 ports", "the hub on a root port enumerated"),
                           (r"usb kbd addr \d+ port \d+\.1 full", "a boot keyboard addressed behind the hub (route string 1)"),
                           (r"usb mouse addr \d+ port \d+ high", "a boot mouse on its own root port")):
            if re.search(want, uart()): print(f"  ok: {what}")
            else: fails.append(f"no {want!r} on the UART ({what})")
        if re.search(r"usb hp 1 st [0-9a-f]{4} ch [0-9a-f]{4}", uart()) and re.search(r"usb hp 2 empty st", uart()): print("  ok: one status line per hub port (connected and empty)")
        else: fails.append("no per-port 'usb hp N' status lines")
        s = socket.socket(socket.AF_UNIX); s.connect(sock); f = s.makefile("rw")
        f.readline()
        def cmd(c, **a):
            f.write(json.dumps({"execute": c, "arguments": a}) + "\n"); f.flush()
            while True:
                r = json.loads(f.readline())
                if "return" in r or "error" in r: return r
        cmd("qmp_capabilities")
        for key, code, usage in (("j", 36, 0x0d), ("t", 20, 0x17)):
            cmd("send-key", keys=[{"type": "qcode", "data": key}])
            for want in (f"usb key 0x{usage:02x} {key}", f"key {code} down", f"key {code} up"):
                if wait_for(want, 30): print(f"  ok: {key} on the USB keyboard printed {want!r}")
                else: fails.append(f"pressing {key}: no {want!r} on the UART, got {uart()[-300:]!r}")
        cmd("input-send-event", events=[{"type": "rel", "data": {"axis": "x", "value": 20}},
                                        {"type": "rel", "data": {"axis": "y", "value": 10}}])
        if wait_for("mouse 420,310", 30): print("  ok: the USB mouse moved right 20, down 10: 'mouse 420,310'")
        else: fails.append(f"moving the mouse: no 'mouse 420,310' on the UART, got {uart()[-300:]!r}")
        cmd("input-send-event", events=[{"type": "btn", "data": {"down": True, "button": "left"}}])
        cmd("input-send-event", events=[{"type": "btn", "data": {"down": False, "button": "left"}}])
        for want in ("key 272 down", "key 272 up"):
            if wait_for(want, 30): print(f"  ok: left click printed {want!r}")
            else: fails.append(f"clicking: no {want!r} on the UART, got {uart()[-300:]!r}")
        # Hot-plug: nothing is attached at these ports at boot, so only the once-a-second rescan can find them.
        r = cmd("device_add", driver="usb-kbd", bus="x.0", port="1.2", id="k2")
        if "error" in r: fails.append(f"device_add behind the hub failed: {r}")
        if wait_for("usb port 5.2 connected", 60) and re.search(r"usb kbd addr \d+ port 5\.2 full", uart()):
            print("  ok: a keyboard plugged into the hub after boot was found by the rescan")
            cmd("send-key", keys=[{"type": "qcode", "data": "k"}])   # the newest keyboard takes the keys
            if wait_for("usb key 0x0e k", 30): print("  ok: ... and its key presses arrive")
            else: fails.append(f"the hot-plugged keyboard's 'k' never arrived, got {uart()[-300:]!r}")
        else: fails.append(f"a keyboard plugged into the hub after boot was never found, got {uart()[-300:]!r}")
        cmd("device_del", id="k2")
        if wait_for("usb port 5.2 disconnected", 60): print("  ok: unplugging it printed 'usb port 5.2 disconnected'")
        else: fails.append(f"the unplugged hub keyboard was not noticed, got {uart()[-300:]!r}")
        r = cmd("device_add", driver="usb-kbd", bus="x.0", port="3", id="k3")
        if "error" in r: fails.append(f"device_add on a root port failed: {r}")
        if re.search(r"usb kbd addr \d+ port 7 ", uart()) or (wait_for("usb port 7 connected", 60) and wait_for(" port 7 ", 20)):
            print("  ok: a keyboard plugged into a root port after boot was found by the rescan")
            time.sleep(0.5)
            cmd("send-key", keys=[{"type": "qcode", "data": "u"}])
            if wait_for("usb key 0x18 u", 30): print("  ok: ... and its key presses arrive")
            else: fails.append(f"the hot-plugged root-port keyboard's 'u' never arrived, got {uart()[-300:]!r}")
        else: fails.append(f"a keyboard plugged into a root port after boot was never found, got {uart()[-300:]!r}")
finally:
    q.kill(); q.wait()
    shutil.rmtree(tmp, ignore_errors=True)
    subprocess.run(["make", "-C", arch, "clean"], capture_output=True)
for m in fails: print("FAIL: " + m)
if fails: sys.exit(1)
print("PASS: the aarch64 kernel finds an xHCI controller behind a PCIe root port, enumerates a USB hub and the keyboard behind it plus a mouse, and their key presses, moves and clicks arrive")
