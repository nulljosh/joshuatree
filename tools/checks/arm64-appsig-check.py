#!/usr/bin/env python3
"""Signed apps (6.54.0): a signed app on the card runs for anyone, an unsigned one only for an admin, a bad signature never.

A throwaway key (tools/signapp.sh --new-key) is built into the kernel as APP_KEY; `make clean` runs after, so a test-key kernel
never outlives the check. The card holds two accounts (guest, and joshua, an admin; 1000 PBKDF2 rounds so sign-in is quick)
and, in JT/APPS, apps made by tools/mkapp.sh from one C file that prints a line and exits 7:
  SUN    signed by the key            MOON   the same image, no signature
  TAMP   signed, then one byte of the image flipped
  OTHER  signed by another key        COPY   SUN.APP and SUN.SIG copied under a new name (the name is signed too)
Boot 1, signed in as guest: `userrun sun` runs to exit 7; `userrun moon` says it is not signed; TAMP, OTHER and COPY say their
signature does not match; `sudo userrun moon` is refused (guest is no admin); Spotlight lists Sun and not Moon.
Boot 2, signed in as joshua: `userrun moon` is still refused without sudo; `sudo userrun moon` and the password run it; under
sudo TAMP is still refused; and while sudo is live Spotlight lists Moon.
Discriminating: make appsig_ok return 1 and TAMP, OTHER and COPY run; make cmd.c pass 1 for allow_unsigned and Moon runs for guest.
Skips (exit 0) when clang's aarch64 target, ld.lld, qemu-system-aarch64, mtools or openssl is missing.
Usage: tools/checks/arm64-appsig-check.py   (from the repo root)
"""
import hashlib, json, os, shutil, socket, subprocess, sys, time
from pathlib import Path

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from scratch import scratch_dir
from sdcard import PART, build_card

root = Path(__file__).resolve().parents[2]
arch = root / "arch/arm64"
if not all(shutil.which(t) for t in ("clang", "ld.lld", "qemu-system-aarch64", "mmd", "mcopy", "openssl")):
    print("SKIP: clang, ld.lld, qemu-system-aarch64, mtools or openssl not installed"); sys.exit(0)
tmp = Path(scratch_dir("arm64-appsig"))
for p in tmp.iterdir(): p.unlink() if p.is_file() else shutil.rmtree(p)

fails = []
def check(name, ok, detail=""):
    print(("  ok: " if ok else "  FAIL: ") + name + ("" if ok else "  " + repr(detail)[-600:]), flush=True)
    if not ok: fails.append(name)
def run(*a, **k):
    r = subprocess.run([str(x) for x in a], capture_output=True, text=True, timeout=600, **k)
    if r.returncode: print(r.stdout[-1500:], r.stderr[-1500:]); print("FAIL: " + " ".join(map(str, a[:3]))); sys.exit(1)
    return r

sign = root / "tools/signapp.sh"
run(sign, "--new-key", tmp / "key.pem", tmp / "key.h")
run(sign, "--new-key", tmp / "other.pem", tmp / "other.h")
subprocess.run(["make", "-C", str(arch), "clean"], capture_output=True, timeout=60)
try:
    run("make", "-C", arch, "kernel8.elf", "APP_KEY=%s" % (tmp / "key.h"))
    (tmp / "sun.c").write_text('#include "jtsys.h"\n__attribute__((section(".text.start"), used))\n'
                               'void _start(void) { static const char m[] = "appsig: ran\\n"; jt_write(1, m, sizeof m - 1); jt_exit(7); }\n')
    run(root / "tools/mkapp.sh", tmp / "sun.c", tmp / "SUN.APP")
    img = (tmp / "SUN.APP").read_bytes()
    for n in ("MOON", "TAMP", "OTHER"): (tmp / (n + ".APP")).write_bytes(img)
    run(sign, tmp / "SUN.APP", tmp / "key.pem")
    run(sign, tmp / "TAMP.APP", tmp / "key.pem")
    t = bytearray(img); t[len(t) // 2] ^= 1; (tmp / "TAMP.APP").write_bytes(bytes(t))
    run(sign, tmp / "OTHER.APP", tmp / "other.pem")
    shutil.copy(tmp / "SUN.APP", tmp / "COPY.APP"); shutil.copy(tmp / "SUN.SIG", tmp / "COPY.SIG")
    APPS = ["SUN.APP", "SUN.SIG", "MOON.APP", "TAMP.APP", "TAMP.SIG", "OTHER.APP", "OTHER.SIG", "COPY.APP", "COPY.SIG"]

    def record(name, pw, salt_byte):
        salt = bytes(range(salt_byte, salt_byte + 16))
        return "%s:p2:1000:%s:%s\n" % (name, salt.hex(), hashlib.pbkdf2_hmac("sha256", pw.encode(), salt, 1000, 32).hex())
    card = str(tmp / "card.img")
    build_card(card, False)
    MT = dict(os.environ, MTOOLS_SKIP_CHECK="1")
    def mt(*a): return subprocess.run([a[0], "-i", f"{card}@@{PART * 512}", *map(str, a[1:])], capture_output=True, env=MT)
    (tmp / "USERS.TXT").write_text(record("joshua", "treepass1", 1) + record("guest", "guestpw22", 40))
    (tmp / "SUDOERS.TXT").write_text("joshua\n")
    if mt("mmd", "::JT").returncode or mt("mmd", "::JT/APPS").returncode: print("FAIL: mtools could not make JT/APPS"); sys.exit(1)
    for f in ("USERS.TXT", "SUDOERS.TXT"): mt("mcopy", "-o", tmp / f, "::JT/" + f)
    for f in APPS:
        if mt("mcopy", "-o", tmp / f, "::JT/APPS/" + f).returncode: print("FAIL: mtools could not copy " + f); sys.exit(1)

    QK = {" ": "spc", ".": "dot", "-": "minus"}
    class Boot:
        def __init__(self, tag):
            self.log, sock = tmp / (tag + ".uart"), tmp / (tag + ".qmp")
            self.q = subprocess.Popen(["qemu-system-aarch64", "-machine", "virt", "-cpu", "cortex-a72", "-m", "256", "-global", "virtio-mmio.force-legacy=false",
                                       "-nic", "none", "-device", "ramfb", "-device", "virtio-keyboard-device",
                                       "-drive", "if=none,id=card,file=%s,format=raw" % card, "-device", "virtio-blk-device,drive=card",
                                       "-display", "none", "-serial", "file:%s" % self.log, "-qmp", "unix:%s,server,nowait" % sock,
                                       "-kernel", str(arch / "kernel8.elf")], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            self.sock, self.f = sock, None
        def uart(self): return self.log.read_text(errors="replace") if self.log.exists() else ""
        def wait_for(self, text, secs, after=0):
            end = time.time() + secs
            while time.time() < end:
                if text in self.uart()[after:]: return True
                if self.q.poll() is not None: return False
                time.sleep(0.1)
            return False
        def cmd(self, c, **a):
            self.f.write(json.dumps({"execute": c, "arguments": a}) + "\n"); self.f.flush()
            while True:
                r = json.loads(self.f.readline())
                if "return" in r or "error" in r: return r
        def key(self, *k): self.cmd("send-key", keys=[{"type": "qcode", "data": x} for x in k]); time.sleep(0.12)
        def typ(self, text):
            for ch in text: self.key(QK.get(ch, ch))
        def login(self, name, pw):
            if not self.wait_for("login: screen up", 90): raise SystemExit("FAIL: no login screen: %r" % self.uart()[-600:])
            s = socket.socket(socket.AF_UNIX); s.settimeout(60); s.connect(str(self.sock)); self.f = s.makefile("rw"); self.f.readline()
            self.cmd("qmp_capabilities"); time.sleep(0.5)
            n = len(self.uart()); self.typ(name); self.key("ret"); self.typ(pw); self.key("ret")
            if not self.wait_for("login: desktop up", 90, n): raise SystemExit("FAIL: %s did not sign in: %r" % (name, self.uart()[n:][-600:]))
            time.sleep(0.5); self.key("f2")
            if not self.wait_for("terminal open", 10): raise SystemExit("FAIL: F2 did not open the Terminal")
            time.sleep(0.5)
        def say(self, line, until, secs=20):
            n = len(self.uart()); self.typ(line); self.key("ret"); self.wait_for(until, secs, n); time.sleep(0.3)
            return self.uart()[n:]
        def sudo(self, line, pw, until):
            n = len(self.uart()); self.typ(line); self.key("ret"); time.sleep(0.8); self.typ(pw); self.key("ret"); self.wait_for(until, 30, n); time.sleep(0.3)
            return self.uart()[n:]
        def spot(self, text):
            self.key("esc"); time.sleep(0.4)
            n = len(self.uart()); self.key("f1"); self.wait_for("spotlight open", 10, n)
            self.typ(text); self.wait_for("spotlight: %s " % text, 10, n); time.sleep(0.3)
            out = self.uart()[n:]; self.key("esc"); time.sleep(0.3); self.key("f2"); time.sleep(0.5)
            return out
        def close(self):
            if self.q.poll() is None: self.q.kill()
            self.q.wait()

    b = Boot("guest")
    try:
        b.login("guest", "guestpw22")
        out = b.say("userrun sun", "M3c app sun exit")
        check("guest: the signed app runs (signed, exit 7)", "JT/APPS/SUN.APP: " in out and "bytes, signed" in out and "appsig: ran" in out and "M3c app sun exit 7" in out, out)
        out = b.say("userrun moon", "userrun moon: ")
        check("guest: the unsigned one is refused", "userrun moon: not signed, so only an admin runs it" in out and "M3c load moon" not in out, out)
        for n in ("tamp", "other", "copy"):
            out = b.say("userrun " + n, "userrun %s: " % n)
            check("guest: %s's signature does not match, so it does not run" % n.upper(),
                  "userrun %s: its signature does not match it" % n in out and "M3c load %s" % n not in out, out)
        out = b.sudo("sudo userrun moon", "guestpw22", "sudo: ")
        check("guest: sudo does not help someone who is not an admin", "guest is not an admin" in out and "M3c load moon" not in out, out)
        out = b.spot("moo")
        check("guest: Spotlight leaves the unsigned Moon out", "spotlight: moo 0\n" in out, out)
        out = b.spot("su")
        check("guest: and lists the signed Sun", "spotlight: su " in out and "spotlight: su 0\n" not in out, out)
        check("boot 1: no kernel crash", "KERNEL CRASH" not in b.uart(), b.uart()[-600:])
    finally:
        b.close()
    b = Boot("admin")
    try:
        b.login("joshua", "treepass1")
        out = b.say("userrun moon", "userrun moon: ")
        check("an admin without sudo: still refused", "userrun moon: not signed" in out, out)
        out = b.sudo("sudo userrun moon", "treepass1", "M3c app moon exit")
        check("sudo userrun moon runs it (not signed, an admin's, exit 7)", "bytes, not signed, an admin's" in out and "appsig: ran" in out and "M3c app moon exit 7" in out, out)
        out = b.say("sudo userrun tamp", "userrun tamp: ")
        check("under sudo a bad signature is still refused", "userrun tamp: its signature does not match it" in out and "M3c load tamp" not in out, out)
        out = b.spot("moo")
        check("while sudo is live Spotlight lists Moon", "spotlight: moo 1\n" in out, out)
        check("boot 2: no kernel crash", "KERNEL CRASH" not in b.uart(), b.uart()[-600:])
    finally:
        b.close()
finally:
    subprocess.run(["make", "-C", str(arch), "clean"], capture_output=True)
if fails: print("FAIL: " + ", ".join(fails)); sys.exit(1)
print("PASS: a signed app runs for anyone, an unsigned one only under an admin's sudo, and a changed, foreign-key or renamed one never")
