#!/usr/bin/env python3
"""Wi-Fi in Settings (6.23.0): the networks the scan heard as rows, a password field with stars, and Join saved for the
next start.

QEMU has no radio, so the kernel is built with WIFI_FAKE=1: wifi.c's stub then reports three made-up networks (My Home 5G
strong, Shaw Open okay, Cafe Guest weak). FORCE rebuilds that object on every make, and the check cleans after itself, so
the list never reaches a real build. On a check card with a JT folder:
  Spotlight opens Settings, Down six times reaches section 6 Wi-Fi, and its card of three rows is drawn white; Enter picks
  row 1, Down row 2, Up row 1 again (`settings wifi pick N NAME`); Enter opens the password field, and while it is open no
  `key N` line reaches the UART; R looks again (6.88.0, `settings wifi scan 3`); a password typed and Enter -> `settings wifi saved My Home 5G`, and JT/WIFI.TXT on the card
  is that name and exactly Python's PMK. The password never reaches the UART.
Discriminating: drop wf_secret from set_secret in users_ui.h and the typed keys show up on the UART.
Skips (exit 0) when clang's aarch64 target, ld.lld, qemu-system-aarch64, PIL or mtools is missing.
Usage: tools/checks/arm64-wifi-pane-check.py   (from the repo root)
"""
import hashlib, json, os, shutil, socket, subprocess, sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from scratch import scratch_dir
try:
    from PIL import Image
except ImportError:
    print("SKIP: PIL is not installed"); sys.exit(0)
from sdcard import PART, build_card

root = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
arch = os.path.join(root, "arch", "arm64")
if not all(shutil.which(t) for t in ("clang", "ld.lld", "qemu-system-aarch64", "mmd", "mcopy")):
    print("SKIP: clang, ld.lld, qemu-system-aarch64 or mtools not installed"); sys.exit(0)
tmp = scratch_dir("arm64-wifi-pane")
MT = dict(os.environ, MTOOLS_SKIP_CHECK="1")
card = os.path.join(tmp, "card.img"); build_card(card, False)
subprocess.run(["mmd", "-i", f"{card}@@{PART * 512}", "::JT"], capture_output=True, env=MT)
NAME, PASS = "My Home 5G", "tidepool-77"
fails = []
def check(name, ok, detail=""):
    print(("  ok: " if ok else "  FAIL: ") + name + ("" if ok or not detail else " -- " + str(detail)[-400:]))
    if not ok: fails.append(name)

subprocess.run(["make", "-C", arch, "clean"], capture_output=True)
b = subprocess.run(["make", "-C", arch, "kernel8.elf", "WIFI_FAKE=1"], capture_output=True, text=True, timeout=900)
if b.returncode: print((b.stdout + b.stderr)[-1500:]); print("FAIL: arch/arm64 does not build"); sys.exit(1)
log, sock = os.path.join(tmp, "uart"), os.path.join(tmp, "qmp")
q = subprocess.Popen(["qemu-system-aarch64", "-machine", "virt", "-cpu", "cortex-a72", "-m", "256", "-global", "virtio-mmio.force-legacy=false",
                      "-nic", "none", "-device", "ramfb", "-device", "virtio-keyboard-device",
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
    def key(*k): command("send-key", keys=[{"type": "qcode", "data": x} for x in k]); time.sleep(0.15)
    command("qmp_capabilities")
    key("f1"); key("s"); key("e"); key("t"); key("t")
    n = len(uart()); key("ret")
    check("Spotlight opens Settings", wait_for("settings open\n", 10, n), uart()[n:][-300:])
    for _ in range(6): key("down")
    check("Down six times reaches section 6 Wi-Fi", wait_for("settings section 6 Wi-Fi\n", 10, n), uart()[n:][-300:])
    time.sleep(0.5)
    command("screendump", filename=os.path.join(tmp, "pane.ppm"))
    img = Image.open(os.path.join(tmp, "pane.ppm")).convert("RGB")
    img.crop((150, 100, 650, 450)).save(os.path.join(tmp, "pane.png"))
    WX, WY = 150, 100; cx, cy = WX + 8 + 150 + 16, WY + 74 + 24
    rows_white = [img.getpixel((cx + 200, cy + 15 + r * 30)) == (255, 255, 255) for r in range(3)]
    check("the card of three heard networks is drawn", all(rows_white) and img.getpixel((cx + 200, cy + 15 + 3 * 30)) != (255, 255, 255), rows_white)
    n = len(uart()); key("r")
    check("R looks again (6.88.0)", wait_for("settings wifi scan 3\n", 10, n), uart()[n:][-300:])
    n = len(uart()); key("ret")
    check("Enter gives the list the keys, row 1 picked", wait_for("settings wifi pick 1 My Home 5G\n", 10, n), uart()[n:][-300:])
    n = len(uart()); key("down"); key("up")
    check("Down and Up move the pick", wait_for("settings wifi pick 2 Shaw Open\n", 10, n) and wait_for("settings wifi pick 1 My Home 5G\n", 10, n), uart()[n:][-300:])
    key("ret"); time.sleep(0.4)
    n = len(uart())
    for ch in PASS: key("minus" if ch == "-" else ch)
    time.sleep(0.4)
    check("no key line reaches the UART while the password field is open", "key " not in uart()[n:], uart()[n:][-300:])
    n = len(uart()); key("ret")
    check("Enter saves it for the next start", wait_for("settings wifi saved My Home 5G\n", 20, n), uart()[n:][-300:])
    check("the password never reaches the UART", PASS not in uart())
finally:
    q.kill(); q.wait()
    subprocess.run(["make", "-C", arch, "clean"], capture_output=True)
out = os.path.join(tmp, "wifi.txt")
subprocess.run(["mcopy", "-n", "-i", f"{card}@@{PART * 512}", "::JT/WIFI.TXT", out], capture_output=True, env=MT)
got = open(out).read() if os.path.exists(out) else ""
want = "ssid=%s\npmk=%s\n" % (NAME.encode().hex(), hashlib.pbkdf2_hmac("sha1", PASS.encode(), NAME.encode(), 4096, 32).hex())
check("JT/WIFI.TXT is the picked name and Python's PMK", got == want, (got, want))
if fails: print("FAIL: " + ", ".join(fails)); sys.exit(1)
print("PASS: Settings lists the heard networks, picks one with the keys, takes its password with stars and no key lines on the UART, and saves the name and PMK for the next start")
