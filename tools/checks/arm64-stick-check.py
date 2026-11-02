#!/usr/bin/env python3
"""8.9.0, USB sticks: xhci.c drives a USB mass storage device (bulk-only transport, SCSI READ and WRITE(10)) and `stick` in
the Terminal reads its FAT partition (emmc.c points fat.c at it for the one command, then back at the card).

Boots the ARM64 kernel in QEMU's virt machine with the check card as a virtio disk and a second FAT32 image as a usb-storage
device on qemu-xhci behind a PCIe root port, the Pi 4's shape. Then:
1. the boot finds it: "usb disk ... MB", by READ CAPACITY;
2. `stick ls` lists the stick's root, STICK.TXT among it;
3. `stick cat stick.txt` prints its text, read sector by sector over USB;
4. `stick copy stick.txt` puts it in JT/ on the card, and `cat jt/stick.txt` reads the same text back from the card, so the
   card was mounted again after the stick and nothing went to the wrong disk;
5. `stick cat nofile.txt` says there is no such file, and the kernel carries on.
Discriminating: drop msc_start's call from enumerate and step 1 never prints; route dev_read to the card always and `stick ls`
lists the card's root (no STICK.TXT); leave stick_end out and step 4's cat finds no JT/ on the stick.
Skips (exit 0) when clang, ld.lld, qemu-system-aarch64 or mtools is missing.
Usage: tools/checks/arm64-stick-check.py   (from the repo root)
"""
import json, os, re, shutil, socket, subprocess, sys, time
from pathlib import Path

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from scratch import scratch_dir
from sdcard import PART, build_card

root = Path(__file__).resolve().parents[2]
arch = root / "arch/arm64"
if not all(shutil.which(t) for t in ("clang", "ld.lld", "qemu-system-aarch64", "mmd", "mcopy")):
    print("SKIP: clang, ld.lld, qemu-system-aarch64 or mtools not installed"); sys.exit(0)
tmp = Path(scratch_dir("arm64-stick"))

fails = []
def check(name, ok, detail=""):
    print(("  ok: " if ok else "  FAIL: ") + name + ("" if ok else "  " + repr(detail)[-900:]))
    if not ok: fails.append(name)

subprocess.run(["make", "-C", str(arch), "clean"], capture_output=True, timeout=60)
r = subprocess.run(["make", "-C", str(arch), "kernel8.elf"], capture_output=True, text=True, timeout=600)
if r.returncode: print(r.stdout[-2000:], r.stderr[-2000:]); print("FAIL: arch/arm64 does not build"); sys.exit(1)

card = str(tmp / "card.img")
build_card(card, False)
stick = str(tmp / "stick.img")
build_card(stick, False)
(tmp / "STICK.TXT").write_text("hello from a usb stick\n")
if subprocess.run(["mcopy", "-i", f"{stick}@@{PART * 512}", "-o", str(tmp / "STICK.TXT"), "::STICK.TXT"], capture_output=True,
                  env=dict(os.environ, MTOOLS_SKIP_CHECK="1")).returncode: print("FAIL: mtools could not put STICK.TXT on the stick"); sys.exit(1)
MT = dict(os.environ, MTOOLS_SKIP_CHECK="1")
def mt(*a): return subprocess.run([a[0], "-i", f"{card}@@{PART * 512}", *a[1:]], capture_output=True, env=MT)

QK = {" ": ("spc",), ".": ("dot",), "/": ("slash",)}
def boot_once(tag):
    log, sock = tmp / ("uart-" + tag), tmp / ("qmp-" + tag)
    for p_ in (log, sock):
        if p_.exists(): p_.unlink()
    q = subprocess.Popen(["qemu-system-aarch64", "-machine", "virt", "-cpu", "cortex-a72", "-m", "256", "-global", "virtio-mmio.force-legacy=false",
                          "-device", "ramfb", "-device", "virtio-keyboard-device", "-drive", "if=none,id=card,file=%s,format=raw" % card, "-device", "virtio-blk-device,drive=card",
                          "-device", "pcie-root-port,id=rp,chassis=1", "-device", "qemu-xhci,id=x,bus=rp",
                          "-drive", "if=none,id=stick,file=%s,format=raw" % stick, "-device", "usb-storage,bus=x.0,port=1,drive=stick",
                          "-display", "none", "-serial", "file:" + str(log), "-qmp", "unix:%s,server,nowait" % sock, "-kernel", str(arch / "kernel8.elf")],
                         stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    def uart(): return log.read_text(errors="replace") if log.exists() else ""
    def wait_for(text, secs, after=0):
        end = time.time() + secs
        while time.time() < end:
            if text in uart()[after:]: return True
            if q.poll() is not None: return False
            time.sleep(0.1)
        return False
    if not wait_for("M2 input ready", 60): q.kill(); raise SystemExit("FAIL: did not boot: %r" % uart()[-600:])
    time.sleep(0.5)
    s = socket.socket(socket.AF_UNIX); s.settimeout(30); s.connect(str(sock)); f = s.makefile("rw"); f.readline()
    def command(name, **a):
        f.write(json.dumps({"execute": name, "arguments": a}) + "\n"); f.flush()
        while True:
            rr = json.loads(f.readline())
            if "return" in rr or "error" in rr: return rr
    command("qmp_capabilities")
    def key(*k): command("send-key", keys=[{"type": "qcode", "data": x} for x in k]); time.sleep(0.12)
    def say(t, until, secs=20):
        n = len(uart())
        for ch in t: key(*QK.get(ch, (ch,)))
        key("ret"); wait_for(until, secs, n); time.sleep(0.4)
        return uart()[n:]
    key("f2")
    if not wait_for("terminal open", 10): q.kill(); raise SystemExit("FAIL: F2 did not open the Terminal: %r" % uart()[-300:])
    time.sleep(0.5)
    return q, uart, say
q, uart, say = boot_once("one")
try:
    m = re.search(r"usb disk addr \d+ port \S+ \w+ \S+ (\d+) MB", uart())
    check("the boot finds the stick by READ CAPACITY (%s MB)" % (m.group(1) if m else "?"), m and int(m.group(1)) > 0, uart()[-600:])
    o = say("stick ls", "joshua@tree %")
    check("stick ls lists the stick's root, with STICK.TXT", "STICK.TXT" in o, o[-600:])
    o = say("stick cat stick.txt", "hello from")
    check("stick cat prints its text over USB", "hello from a usb stick" in o, o[-400:])
    o = say("stick copy stick.txt", "stick: ")
    check("stick copy puts it in JT/ on the card", "stick: copied STICK.TXT to JT/, 23 bytes" in o or "stick: copied stick.txt to JT/, 23 bytes" in o, o[-400:])
    o = say("cat jt/stick.txt", "hello from")
    check("cat reads the copy back from the card (the card is the disk again)", "hello from a usb stick" in o, o[-400:])
    o = say("stick cat nofile.txt", "stick: ")
    check("a file that is not there is said so", "no file" in o, o[-300:])
    check("no kernel crash, no USB timeout", "KERNEL CRASH" not in uart() and "usb disk timeout" not in uart() and q.poll() is None, uart()[-400:])
finally:
    q.kill(); q.wait()
    subprocess.run(["make", "-C", str(arch), "clean"], capture_output=True, timeout=60)
if fails: print("FAIL: %d of the stick checks" % len(fails)); sys.exit(1)
print("PASS: a USB stick on the xHCI controller is found, listed, read and copied from, and the card is the disk again after")
