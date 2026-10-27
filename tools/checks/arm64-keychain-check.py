#!/usr/bin/env python3
"""The keychain (6.21.0): the mail sign-in sealed in the person's home folder with a key only their login password makes.

A check card with ann and bob, and a fixture IMAP server over TLS on the host (its throwaway certificate built into the
kernel's anchors). Five boots of the same card:
  1. ann signs in, `mail login` with the app password -> "kept in your keychain". JT/HOME/ann/KEYCHAIN.BIN starts JTK1
     and holds neither the app password nor the mail user in plain text.
  2. ann again: `mail` lists the inbox with no prompt (the server sees the right LOGIN); `passwd` changes her password.
  3. ann with the new password: `mail` still works (passwd re-sealed the keychain).
  4. bob: `mail` is not signed in; ann's keychain is not his.
  5. one byte of ann's file flipped: ann signs in and `mail` says the keychain did not open, and is not signed in.
The app password never reaches the UART.
Discriminating: make keychain_unlock in keychain.c do nothing and boot 2 is not signed in.
Skips (exit 0) when clang's aarch64 target, ld.lld, openssl, qemu-system-aarch64 or mtools is missing.
Usage: tools/checks/arm64-keychain-check.py   (from the repo root)
"""
import hashlib, json, os, shutil, socket, socketserver, ssl, subprocess, sys, threading, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from scratch import scratch_dir
from sdcard import PART, build_card

root = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
arch = os.path.join(root, "arch", "arm64")
if not all(shutil.which(t) for t in ("clang", "ld.lld", "qemu-system-aarch64", "openssl", "mmd", "mcopy")):
    print("SKIP: clang, ld.lld, openssl, qemu-system-aarch64 or mtools not installed"); sys.exit(0)
tmp = scratch_dir("arm64-keychain")
cert, key = os.path.join(tmp, "cert.pem"), os.path.join(tmp, "key.pem")
subprocess.run(["openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes", "-days", "2", "-subj", "/CN=10.0.2.2",
                "-addext", "subjectAltName=DNS:10.0.2.2", "-keyout", key, "-out", cert], check=True, capture_output=True)
ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER); ctx.load_cert_chain(cert, key)
MUSER, MPASS = "ann@example.com", "imap-app-pass-77"
logins = []
HDR = b"From: Sam <sam@example.com>\r\nSubject: Hello Ann\r\n\r\n"
class Imap(socketserver.StreamRequestHandler):
    def handle(self):
        w = self.wfile.write
        w(b"* OK ready\r\n")
        while (line := self.rfile.readline()):
            tag, _, rest = line.decode().rstrip("\r\n").partition(" ")
            if rest.startswith("LOGIN"): logins.append(rest); w((tag + (" OK done\r\n" if rest == 'LOGIN "%s" "%s"' % (MUSER, MPASS) else " NO bad\r\n")).encode())
            elif rest.startswith("SELECT"): w(b"* 1 EXISTS\r\n" + tag.encode() + b" OK done\r\n")
            elif rest.startswith("FETCH"): w(b"* 1 FETCH (BODY[HEADER.FIELDS (FROM SUBJECT)] {%d}\r\n%s)\r\n%s OK done\r\n" % (len(HDR), HDR, tag.encode()))
            elif rest.startswith("LOGOUT"): w(b"* BYE\r\n" + tag.encode() + b" OK done\r\n"); return
            else: w(tag.encode() + b" BAD\r\n")
class TLSServer(socketserver.ThreadingTCPServer):
    allow_reuse_address = True; daemon_threads = True
    def get_request(self): s, a = super().get_request(); return ctx.wrap_socket(s, server_side=True), a
imap = TLSServer(("127.0.0.1", 0), Imap)
threading.Thread(target=imap.serve_forever, daemon=True).start()

fails = []
def check(name, ok, detail=""):
    print(("  ok: " if ok else "  FAIL: ") + name + ("" if ok or not detail else " -- " + str(detail)[-400:]))
    if not ok: fails.append(name)
MT = dict(os.environ, MTOOLS_SKIP_CHECK="1")
def mt(card, *a): return subprocess.run([a[0], "-i", f"{card}@@{PART * 512}", *a[1:]], capture_output=True, env=MT)
def record(name, pw, salt_byte):
    salt = bytes(range(salt_byte, salt_byte + 16))
    return "%s:p2:100000:%s:%s\n" % (name, salt.hex(), hashlib.pbkdf2_hmac("sha256", pw.encode(), salt, 100000, 32).hex())
card = os.path.join(tmp, "card.img"); build_card(card, False)
for d in ("::JT", "::JT/HOME", "::JT/HOME/ann", "::JT/HOME/bob"): mt(card, "mmd", d)
f = os.path.join(tmp, "users"); open(f, "w").write(record("ann", "hunter22", 32) + record("bob", "bobpass44", 64)); mt(card, "mcopy", f, "::JT/USERS.TXT")
f = os.path.join(tmp, "sudoers"); open(f, "w").write("ann\n"); mt(card, "mcopy", f, "::JT/SUDOERS.TXT")
def keychain_bytes():
    out = os.path.join(tmp, "kc.bin")
    if os.path.exists(out): os.remove(out)
    mt(card, "mcopy", "-n", "::JT/HOME/ann/KEYCHAIN.BIN", out)
    return open(out, "rb").read() if os.path.exists(out) else b""

QK = {" ": ("spc",), "-": ("minus",), ".": ("dot",), "/": ("slash",), ":": ("shift", "semicolon"), "@": ("shift", "2")}
class Session:
    def __init__(self, tag):
        self.log, self.sock = os.path.join(tmp, tag + ".uart"), os.path.join(tmp, tag + ".qmp")
        self.q = subprocess.Popen(["qemu-system-aarch64", "-machine", "virt", "-cpu", "cortex-a72", "-m", "256", "-global", "virtio-mmio.force-legacy=false",
                                   "-netdev", "user,id=n", "-device", "virtio-net-device,netdev=n", "-device", "ramfb", "-device", "virtio-keyboard-device",
                                   "-drive", "if=none,id=card,file=%s,format=raw" % card, "-device", "virtio-blk-device,drive=card",
                                   "-display", "none", "-serial", "file:" + self.log, "-qmp", "unix:%s,server,nowait" % self.sock,
                                   "-kernel", os.path.join(arch, "kernel8.elf")], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    def uart(self): return open(self.log, errors="replace").read() if os.path.exists(self.log) else ""
    def wait_for(self, text, secs=8.0, after=0):
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
    def key(self, *q): self.cmd("send-key", keys=[{"type": "qcode", "data": k} for k in q]); time.sleep(0.1)
    def typ(self, line):
        for ch in line: self.key(*QK.get(ch, (ch,)))
    def boot(self, user, pw):
        if not self.wait_for("M2 input ready", 90) or not self.wait_for("login: screen up", 60): return False
        s_ = socket.socket(socket.AF_UNIX); s_.settimeout(60); s_.connect(self.sock); self.f = s_.makefile("rw"); self.f.readline(); self.cmd("qmp_capabilities")
        time.sleep(0.5); n = len(self.uart())
        self.typ(user); self.key("ret"); self.typ(pw); self.key("ret")
        if not self.wait_for("login: desktop up", 200, n): return False
        time.sleep(0.5); self.key("f2")
        return self.wait_for("terminal open", 10, n)
    def run(self, line, want, secs=40):
        n = len(self.uart()); self.typ(line); self.key("ret")
        return self.wait_for(want, secs, n), self.uart()[n:]
    def stop(self): self.q.kill(); self.q.wait()

port = imap.server_address[1]
b = subprocess.run(["make", "-C", arch, "clean"], capture_output=True)
b = subprocess.run(["make", "-C", arch, "kernel8.elf", "TLS_TA=" + cert], capture_output=True, text=True, timeout=900)
if b.returncode: print((b.stdout + b.stderr)[-1500:]); print("FAIL: arch/arm64 does not build"); sys.exit(1)
S = None
try:
    S = Session("one")
    check("boot 1: ann signs in", S.boot("ann", "hunter22"), S.uart()[-300:])
    S.run("mail login 10.0.2.2:%d %s" % (port, MUSER), "mail login", 10); time.sleep(0.5)
    ok, out = S.run(MPASS, "mail: kept in your keychain", 60)
    check("mail login is kept in ann's keychain", ok, out[-300:])
    S.stop(); S = None
    kc = keychain_bytes()
    check("KEYCHAIN.BIN is in ann's home, sealed (JTK1, neither the app password nor the user in plain text)", kc[:4] == b"JTK1" and MPASS.encode() not in kc and MUSER.encode() not in kc, kc[:40])

    S = Session("two")
    check("boot 2: ann signs in again", S.boot("ann", "hunter22"), S.uart()[-300:])
    k = len(logins); ok, out = S.run("mail", "mail N opens one", 60)
    check("mail works after the restart with no prompt", ok and "Hello Ann" in out and logins[k:] and logins[-1] == 'LOGIN "%s" "%s"' % (MUSER, MPASS), (logins[k:], out[-300:]))
    S.run("passwd", "passwd", 10); time.sleep(0.5)
    S.typ("hunter22"); S.key("ret"); time.sleep(2.5); S.typ("hunter33"); S.key("ret"); time.sleep(0.8); n = len(S.uart()); S.typ("hunter33"); S.key("ret")
    check("passwd changes ann's password", S.wait_for("passwd ann: password changed", 120, n), S.uart()[n:][-300:])
    S.stop(); S = None

    S = Session("three")
    check("boot 3: ann signs in with the new password", S.boot("ann", "hunter33"), S.uart()[-300:])
    ok, out = S.run("mail", "mail N opens one", 60)
    check("the keychain followed the new password", ok and "Hello Ann" in out, out[-300:])
    S.stop(); S = None

    S = Session("four")
    check("boot 4: bob signs in", S.boot("bob", "bobpass44"), S.uart()[-300:])
    ok, out = S.run("mail", "mail: not signed in", 30)
    check("bob does not get ann's mail", ok, out[-300:])
    S.stop(); S = None

    kc = bytearray(keychain_bytes()); kc[-3] ^= 0x40
    f = os.path.join(tmp, "kc-bad.bin"); open(f, "wb").write(kc); mt(card, "mcopy", "-o", f, "::JT/HOME/ann/KEYCHAIN.BIN")
    S = Session("five")
    check("boot 5: ann signs in", S.boot("ann", "hunter33"), S.uart()[-300:])
    ok, out = S.run("mail", "mail: not signed in", 30)
    check("a changed byte: the keychain does not open, nothing in it is believed", ok and "your keychain did not open" in out, out[-300:])
    check("the app password never reached the UART", all(MPASS not in open(os.path.join(tmp, t + ".uart"), errors="replace").read() for t in ("one", "two", "three", "four", "five")))
finally:
    if S: S.stop()
    imap.shutdown()
    subprocess.run(["make", "-C", arch, "clean"], capture_output=True)
if fails: print("FAIL: " + ", ".join(fails)); sys.exit(1)
print("PASS: the mail sign-in is sealed in the person's home under their login password, survives a restart and a passwd, stays theirs alone, and a changed byte opens nothing")
