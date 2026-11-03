#!/usr/bin/env python3
"""Parental controls on the Pi (12.0, arch/arm64/limits.c and cmd.c's `limits`), as in Screen Time.

The card holds ann (an admin) and kid, made with Python's hashlib as arm64-sudo-check does. The build fixes the clock at 21:00
(-DLIMITS_FAKE_HOUR=21) so the hours rule can be seen with no network. Two boots of the virt build:
  1. ann: `limits` says nobody is limited; without sudo it says to use sudo; `sudo limits kid apps notes,terminal,blackcomb`
     asks for her password and saves; `sudo limits kid web off` adds web=0; an admin cannot be limited; a bad value is
     refused. After QEMU stops, JT/LIMITS.TXT is exactly "kid apps=notes,terminal,blackcomb web=0".
  2. kid: Spotlight opens Notes; Mines is refused ("Mines is not allowed for this account") and never starts; the Terminal
     opens; `browse` is refused ("the web is off"); kid's own `limits kid apps all` is refused (not an admin) and
     `write LIMITS.TXT` is refused (a protected file). logout; ann signs in and sets `sudo limits kid hours 7-20`; logout;
     kid signs in at 21:00 and Notes is now refused ("outside this account's hours").
Discriminating: make limits_app_ok return 1 at once and Mines starts for kid.
Skips (exit 0) when clang's aarch64 target, ld.lld, qemu-system-aarch64 or mtools is missing.
Usage: tools/checks/arm64-limits-check.py   (from the repo root)
"""
import hashlib, json, os, shutil, socket, subprocess, sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from scratch import scratch_dir
from sdcard import PART, build_card
root = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
arch = os.path.join(root, "arch", "arm64")
if not all(shutil.which(t) for t in ("clang", "ld.lld", "qemu-system-aarch64", "mtype", "mcopy", "mmd")):
    print("SKIP: no aarch64 toolchain or mtools"); sys.exit(0)
fails = []
def check(name, ok, detail=""):
    print(("  ok: " if ok else "  FAIL: ") + name + (f" ({str(detail)[-400:]})" if detail and not ok else ""))
    if not ok: fails.append(name)

tmp = scratch_dir("arm64-limits")
MT = dict(os.environ, MTOOLS_SKIP_CHECK="1")
def mt(card, *a): return subprocess.run([a[0], "-i", f"{card}@@{PART * 512}", *a[1:]], capture_output=True, env=MT)
def record(name, pw, salt_byte):
    salt = bytes(range(salt_byte, salt_byte + 16))
    return "%s:p2:100000:%s:%s\n" % (name, salt.hex(), hashlib.pbkdf2_hmac("sha256", pw.encode(), salt, 100000, 32).hex())
card = os.path.join(tmp, "card.img"); build_card(card, False)
mt(card, "mmd", "::JT")
for fname, text in (("USERS.TXT", record("ann", "annpass12", 1) + record("kid", "kidpass34", 40)), ("SUDOERS.TXT", "ann\n")):
    p = os.path.join(tmp, fname); open(p, "w").write(text); mt(card, "mcopy", p, "::JT/" + fname)

subprocess.run(["make", "-C", arch, "clean"], capture_output=True)
b = subprocess.run(["make", "-C", arch, "kernel8.elf", "MAINFLAGS=-DLIMITS_FAKE_HOUR=21"], capture_output=True, text=True, timeout=900)
if b.returncode: print("FAIL: the build\n" + (b.stdout + b.stderr)[-1500:]); sys.exit(1)

QK = {" ": "spc", "-": "minus", ".": "dot", "/": "slash", ",": "comma"}
class S:
    def __init__(self, tag):
        self.log, self.sock = os.path.join(tmp, tag + ".uart"), os.path.join(tmp, tag + ".qmp")
        self.q = subprocess.Popen(["qemu-system-aarch64", "-machine", "virt", "-cpu", "cortex-a72", "-m", "256", "-global", "virtio-mmio.force-legacy=false",
                                   "-nic", "none", "-device", "ramfb", "-device", "virtio-keyboard-device",
                                   "-drive", "if=none,id=card,file=%s,format=raw" % card, "-device", "virtio-blk-device,drive=card",
                                   "-display", "none", "-serial", "file:" + self.log, "-qmp", "unix:%s,server,nowait" % self.sock,
                                   "-kernel", os.path.join(arch, "kernel8.elf")], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    def uart(self): return open(self.log, errors="replace").read() if os.path.exists(self.log) else ""
    def wait_for(self, text, secs=10.0, after=0):
        end = time.time() + secs
        while time.time() < end:
            if text in self.uart()[after:]: return True
            time.sleep(0.05)
        return False
    def cmd(self, c, **a):
        self.f.write(json.dumps({"execute": c, "arguments": a}) + "\n"); self.f.flush()
        while True:
            r = json.loads(self.f.readline())
            if "return" in r or "error" in r: return r
    def key(self, *k): self.cmd("send-key", keys=[{"type": "qcode", "data": x} for x in k]); time.sleep(0.1)
    def typ(self, s):
        for ch in s: self.key(QK.get(ch, ch))
    def boot(self):
        if not self.wait_for("M2 input ready", 90) or not self.wait_for("login: screen up", 60): return False
        s_ = socket.socket(socket.AF_UNIX); s_.settimeout(60); s_.connect(self.sock)
        self.f = s_.makefile("rw"); self.f.readline(); self.cmd("qmp_capabilities"); time.sleep(0.5); return True
    def login(self, name, pw):
        n = len(self.uart()); self.typ(name); self.key("ret"); self.typ(pw); self.key("ret")
        ok = self.wait_for("login: desktop up", 150, n); time.sleep(0.6); return ok
    def terminal(self):
        n = len(self.uart()); self.key("f2"); return self.wait_for("terminal open", 10, n) or self.wait_for("limits: ", 2, n)
    def run(self, line, want, secs=20):
        n = len(self.uart()); self.typ(line); self.key("ret"); ok = self.wait_for(want, secs, n); return ok, self.uart()[n:][-500:]
    def spot(self, name, want, secs=15):
        n = len(self.uart()); self.key("esc"); time.sleep(0.3); self.key("f1"); self.wait_for("spotlight open", 10, n)
        self.typ(name.lower()); time.sleep(0.3); self.key("ret"); ok = self.wait_for(want, secs, n); time.sleep(0.5); return ok, self.uart()[n:][-500:]
    def logout(self):
        n = len(self.uart()); self.typ("logout"); self.key("ret"); return self.wait_for("login: screen up", 30, n)
    def close(self):
        self.q.terminate()
        try: self.q.wait(5)
        except subprocess.TimeoutExpired: self.q.kill()

s = S("one")
try:
    if not s.boot() or not s.login("ann", "annpass12"): check("ann signs in", False, s.uart()[-400:]); raise SystemExit
    check("the Terminal opens for ann", s.terminal())
    check("limits: nobody is limited yet", *s.run("limits", "limits: nobody is limited"))
    check("without sudo it says to use sudo", *s.run("limits kid web off", "limits: needs sudo, try sudo limits"))
    n = len(s.uart()); s.typ("sudo limits kid apps notes,terminal,blackcomb"); s.key("ret"); s.wait_for("[sudo] password", 10, n)
    s.typ("annpass12"); s.key("ret")
    check("sudo limits kid apps ... saves the apps", s.wait_for("limits: kid apps=notes,terminal,blackcomb", 150, n), s.uart()[n:][-400:])
    check("web off adds web=0", *s.run("sudo limits kid web off", "limits: kid apps=notes,terminal,blackcomb web=0"))
    check("an admin cannot be limited", *s.run("sudo limits ann web off", "limits: ann is an admin, and admins are never limited"))
    check("a bad value is refused", *s.run("sudo limits kid hours 9", "limits: apps takes names"))
except SystemExit: pass
finally: s.close()
got = mt(card, "mtype", "::JT/LIMITS.TXT").stdout.decode(errors="replace")
check("JT/LIMITS.TXT holds kid's line and nothing else", got == "kid apps=notes,terminal,blackcomb web=0\n", got)

s = S("two")
try:
    if not s.boot() or not s.login("kid", "kidpass34"): check("kid signs in", False, s.uart()[-400:]); raise SystemExit
    check("kid: Spotlight opens Notes, an allowed app", *s.spot("Notes", "M3c window notes"))
    ok, out = s.spot("Mines", "limits: Mines is not allowed for this account")
    check("kid: Mines is refused and never starts", ok and "M3c window mines" not in out, out)
    s.key("esc"); time.sleep(0.3)
    check("kid: the Terminal opens, it is allowed", s.terminal())
    check("kid: the web is off", *s.run("browse example.com", "browser: the web is off for this account"))
    check("kid cannot lift a limit", *s.run("limits kid apps all", "limits: you are not an admin; try sudo limits"))
    ok, out = s.run("write LIMITS.TXT x", "write", 10)
    check("kid cannot write LIMITS.TXT by hand", ok and "limits:" not in mt(card, "mtype", "::JT/LIMITS.TXT").stdout.decode(errors="replace") and mt(card, "mtype", "::JT/LIMITS.TXT").stdout.decode(errors="replace").startswith("kid apps="), out)
    if not s.logout() or not s.login("ann", "annpass12"): check("ann signs in again", False, s.uart()[-400:]); raise SystemExit
    s.terminal()
    n = len(s.uart()); s.typ("sudo limits kid hours 7-20"); s.key("ret"); s.wait_for("[sudo] password", 10, n); s.typ("annpass12"); s.key("ret")
    check("ann sets hours 7-20", s.wait_for("limits: kid apps=notes,terminal,blackcomb hours=7-20 web=0", 150, n), s.uart()[n:][-400:])
    if not s.logout() or not s.login("kid", "kidpass34"): check("kid signs in again", False, s.uart()[-400:]); raise SystemExit
    ok, out = s.spot("Notes", "limits: Notes is outside this account's hours")
    check("kid at 21:00: Notes is outside the hours and does not open", ok and "M3c window notes" not in out, out)
except SystemExit: pass
finally: s.close()

if fails: print("FAIL: " + "; ".join(fails)); sys.exit(1)
print("PASS: an admin limits another account's apps, hours and web with sudo limits, kept in JT/LIMITS.TXT; the limited account is refused the rest, cannot lift a limit or write the file, and admins are never limited")
