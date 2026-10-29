#!/usr/bin/env python3
"""Picking the Wi-Fi network on the Pi (6.22.0): `wifi join NAME` turns a passphrase into the network key on the Pi and
saves it for the next start.

QEMU virt has no Wi-Fi chip, so this checks everything up to the radio; the join itself is the board pass. On a check card
with a JT folder, at the Terminal:
  `wifi` says no network is set and none was heard; `wifi join My Home 5G` asks for the passphrase with stars; a short
  one is refused and saves nothing; a good one writes JT/WIFI.TXT as the network's name and the 32 byte PMK in hex,
  exactly PBKDF2-HMAC-SHA1(passphrase, name, 4096, 32) as Python works it out, and never the passphrase; the passphrase
  never reaches the UART. wpa-test.c checks wpa_pmk against IEEE 802.11i's own vector.
Discriminating: a PMK made with the wrong salt or round count does not match Python's.
Skips (exit 0) when clang's aarch64 target, ld.lld, qemu-system-aarch64 or mtools is missing.
Usage: tools/checks/arm64-wifi-join-check.py   (from the repo root)
"""
import hashlib, json, os, shutil, socket, subprocess, sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from scratch import scratch_dir
from sdcard import PART, build_card

root = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
arch = os.path.join(root, "arch", "arm64")
if not all(shutil.which(t) for t in ("clang", "ld.lld", "qemu-system-aarch64", "mmd", "mcopy")):
    print("SKIP: clang, ld.lld, qemu-system-aarch64 or mtools not installed"); sys.exit(0)
tmp = scratch_dir("arm64-wifi-join")
MT = dict(os.environ, MTOOLS_SKIP_CHECK="1")
card = os.path.join(tmp, "card.img"); build_card(card, False)
subprocess.run(["mmd", "-i", f"{card}@@{PART * 512}", "::JT"], capture_output=True, env=MT)
NAME, PASS = "My Home 5G", "shoreline-4242"
fails = []
def check(name, ok, detail=""):
    print(("  ok: " if ok else "  FAIL: ") + name + ("" if ok or not detail else " -- " + str(detail)[-400:]))
    if not ok: fails.append(name)

subprocess.run(["make", "-C", arch, "clean"], capture_output=True)
b = subprocess.run(["make", "-C", arch, "kernel8.elf"], capture_output=True, text=True, timeout=900)
if b.returncode: print((b.stdout + b.stderr)[-1500:]); print("FAIL: arch/arm64 does not build"); sys.exit(1)
log, sock = os.path.join(tmp, "uart"), os.path.join(tmp, "qmp")
q = subprocess.Popen(["qemu-system-aarch64", "-machine", "virt", "-cpu", "cortex-a72", "-m", "256", "-global", "virtio-mmio.force-legacy=false",
                      "-nic", "none", "-device", "virtio-keyboard-device",
                      "-drive", "if=none,id=card,file=%s,format=raw" % card, "-device", "virtio-blk-device,drive=card",
                      "-display", "none", "-serial", "file:" + log, "-qmp", "unix:%s,server,nowait" % sock,
                      "-kernel", os.path.join(arch, "kernel8.elf")], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
def uart(): return open(log, errors="replace").read() if os.path.exists(log) else ""
def wait_for(text, secs, after=0):
    end = time.time() + secs
    while time.time() < end:
        if text in uart()[after:]: return True
        time.sleep(0.1)
    return False
try:
    if not wait_for("M2 input ready", 60): raise SystemExit("FAIL: did not boot: %r" % uart()[-600:])
    time.sleep(0.5)
    s = socket.socket(socket.AF_UNIX); s.settimeout(30); s.connect(sock); f = s.makefile("rw"); f.readline()
    def command(name, **a):
        f.write(json.dumps({"execute": name, "arguments": a}) + "\n"); f.flush()
        while True:
            r = json.loads(f.readline())
            if "return" in r or "error" in r: return r
    QK = {" ": ("spc",), "-": ("minus",)}
    def key(*k): command("send-key", keys=[{"type": "qcode", "data": x} for x in k]); time.sleep(0.12)
    def line(t):
        for ch in t: key(*(("shift", ch.lower()) if ch.isupper() else QK.get(ch, (ch,))))
        key("ret")
    command("qmp_capabilities")
    key("f2")
    check("the Terminal opened", wait_for("terminal open", 10), uart()[-300:])
    n = len(uart()); line("wifi")
    check("wifi says no network is set and none was heard", wait_for("wifi join NAME picks one", 10, n) and "wifi: no network set" in uart()[n:] and "no networks heard" in uart()[n:], uart()[n:][-300:])
    n = len(uart()); line("wifi join " + NAME); time.sleep(0.5); line("short1")
    check("a passphrase under 8 characters is refused", wait_for("wifi: a passphrase is 8 to 63 plain characters, nothing saved", 10, n) and "Wi-Fi password for %s: \n" % NAME in uart()[n:], uart()[n:][-300:])
    n = len(uart()); line("wifi join " + NAME); time.sleep(0.5); line(PASS)
    check("a good one is saved, and with no radio here (QEMU) joined at the next start (6.88.0 joins at once on the Pi)",
          wait_for("wifi: saved %s, joining" % NAME, 20, n) and wait_for("wifi: the Pi joins it at its next start", 10, n), uart()[n:][-300:])
    check("the passphrase never reaches the UART", PASS not in uart() and "short1" not in uart())
finally:
    q.kill(); q.wait()
    subprocess.run(["make", "-C", arch, "clean"], capture_output=True)
out = os.path.join(tmp, "wifi.txt")
subprocess.run(["mcopy", "-n", "-i", f"{card}@@{PART * 512}", "::JT/WIFI.TXT", out], capture_output=True, env=MT)
got = open(out).read() if os.path.exists(out) else ""
want = "ssid=%s\npmk=%s\n" % (NAME.encode().hex(), hashlib.pbkdf2_hmac("sha1", PASS.encode(), NAME.encode(), 4096, 32).hex())
check("JT/WIFI.TXT is the name and Python's PMK, and not the passphrase", got == want and PASS not in got, (got, want))
if fails: print("FAIL: " + ", ".join(fails)); sys.exit(1)
print("PASS: wifi join turns a passphrase into the network key on the Pi, refuses a short one, saves only the name and the PMK, and the passphrase never shows")
