#!/usr/bin/env python3
"""Cookies kept per person (6.26.0): a persistent cookie survives a restart in its owner's keychain, a session cookie
does not, and nobody else gets either.

A check card with ann and bob, and a stub server on the host: /set answers with Set-Cookie keep=abc123 (Max-Age, so
persistent) and sess=xyz789 (a session cookie); /echo writes back the Cookie header it was sent. Four boots of one card:
  1. ann: browse /set -> the jar says keep is kept; `cookies` lists both by name, never a value; /echo gets both.
     Then /expire (6.30.0) sends sess with an Expires date in 1970, tmp with the dashed 1970 spelling, and late with
     Max-Age=60 and a 1970 Expires: /echo gets keep and late (Max-Age wins), not sess or tmp.
  2. ann again: /echo gets keep=abc123 and late=z (the session cookie stayed in memory); `cookies clear`.
  3. bob: /echo gets no cookie, and `cookies` says 0.
  4. ann: /echo gets no cookie (the clear was kept).
The card's JT/HOME/ann/KEYCHAIN.BIN never holds the value in plain text, and no value reaches the UART.
Discriminating: drop jar_save from take_cookies in browser.c and boot 2's /echo gets nothing; make http_date_past in
clock.c return 0 and boot 1's /echo still carries sess and tmp.
Skips (exit 0) when clang's aarch64 target, ld.lld, qemu-system-aarch64 or mtools is missing.
Usage: tools/checks/arm64-cookiekeep-check.py   (from the repo root)
"""
import hashlib, http.server, json, os, shutil, socket, subprocess, sys, threading, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from scratch import scratch_dir
from sdcard import PART, build_card

root = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
arch = os.path.join(root, "arch", "arm64")
if not all(shutil.which(t) for t in ("clang", "ld.lld", "qemu-system-aarch64", "mmd", "mcopy")):
    print("SKIP: clang, ld.lld, qemu-system-aarch64 or mtools not installed"); sys.exit(0)
tmp = scratch_dir("arm64-cookiekeep")
seen = []
class Handler(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        if self.path == "/set":
            body = b"<html><body><p>cookies set</p></body></html>"
            self.send_response(200); self.send_header("Set-Cookie", "keep=abc123; Path=/; Max-Age=3600"); self.send_header("Set-Cookie", "sess=xyz789; Path=/")
        elif self.path == "/expire":
            body = b"<html><body><p>cookies expired</p></body></html>"
            self.send_response(200)
            self.send_header("Set-Cookie", "sess=; Path=/; Expires=Thu, 01 Jan 1970 00:00:00 GMT")
            self.send_header("Set-Cookie", "tmp=t1; Path=/; Expires=Thu, 01-Jan-1970 00:00:01 GMT")
            self.send_header("Set-Cookie", "late=z; Max-Age=60; Expires=Thu, 01 Jan 1970 00:00:00 GMT")
        else:
            got = self.headers.get("Cookie", ""); seen.append(got)
            body = ("<html><body><p>echo done</p></body></html>").encode()
            self.send_response(200)
        self.send_header("Content-Length", str(len(body))); self.end_headers(); self.wfile.write(body)
    def log_message(self, *a): pass
server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
port = server.server_address[1]
threading.Thread(target=server.serve_forever, daemon=True).start()
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
QK = {" ": ("spc",), "-": ("minus",), ".": ("dot",), "/": ("slash",), ":": ("shift", "semicolon")}
class Session:
    def __init__(self, tag):
        self.tag, self.log, self.sock = tag, os.path.join(tmp, tag + ".uart"), os.path.join(tmp, tag + ".qmp")
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
        return self.wait_for("terminal open", 10, n) and self.wait_for("net dhcp", 30)
    def run(self, line, want, secs=30):
        n = len(self.uart()); self.typ(line); self.key("ret")
        return self.wait_for(want, secs, n), self.uart()[n:]
    def stop(self): self.q.kill(); self.q.wait()
subprocess.run(["make", "-C", arch, "clean"], capture_output=True)
b = subprocess.run(["make", "-C", arch, "kernel8.elf"], capture_output=True, text=True, timeout=900)
if b.returncode: print((b.stdout + b.stderr)[-1500:]); print("FAIL: arch/arm64 does not build"); sys.exit(1)
base = "http://10.0.2.2:%d" % port
S = None
try:
    S = Session("one")
    check("boot 1: ann signs in", S.boot("ann", "hunter22"), S.uart()[-300:])
    ok, out = S.run("browse %s/set" % base, "cookies set", 30)
    check("the persistent cookie is kept, the session one is not", ok and "browser: cookie keep (kept)" in out and "browser: cookie sess\n" in out, out[-300:])
    ok, out = S.run("cookies", "browser: 2 cookies", 10)
    check("cookies lists both by name, never a value", ok and "10.0.2.2 keep kept" in out and "abc123" not in out and "xyz789" not in out, out[-300:])
    S.run("browse %s/echo" % base, "echo done", 30)
    check("the page gets both", seen[-1:] == ["keep=abc123; sess=xyz789"] or seen[-1:] == ["sess=xyz789; keep=abc123"], seen[-1:])
    ok, out = S.run("browse %s/expire" % base, "cookies expired", 30)
    check("a past Expires is not taken, Max-Age beats it", ok and "cookie tmp" not in out and "browser: cookie late (kept)" in out, out[-300:])
    S.run("browse %s/echo" % base, "echo done", 30)
    check("a past Expires deletes, in both spellings", sorted(seen[-1].split("; ")) == ["keep=abc123", "late=z"] if seen else False, seen[-1:])
    S.stop(); S = None
    S = Session("two")
    check("boot 2: ann again", S.boot("ann", "hunter22"), S.uart()[-300:])
    S.run("browse %s/echo" % base, "echo done", 30)
    check("after a restart only the persistent cookies come back", sorted(seen[-1].split("; ")) == ["keep=abc123", "late=z"] if seen else False, seen[-1:])
    ok, out = S.run("cookies clear", "browser: cookies cleared", 10)
    check("cookies clear empties the jar", ok, out[-200:])
    S.stop(); S = None
    S = Session("three")
    check("boot 3: bob", S.boot("bob", "bobpass44"), S.uart()[-300:])
    S.run("browse %s/echo" % base, "echo done", 30)
    check("bob's pages get none of ann's cookies", seen[-1:] == [""], seen[-1:])
    ok, out = S.run("cookies", "browser: 0 cookies", 10)
    check("and bob's jar is empty", ok, out[-200:])
    S.stop(); S = None
    S = Session("four")
    check("boot 4: ann", S.boot("ann", "hunter22"), S.uart()[-300:])
    S.run("browse %s/echo" % base, "echo done", 30)
    check("the clear was kept", seen[-1:] == [""], seen[-1:])
    S.stop(); S = None
    out = os.path.join(tmp, "kc.bin")
    mt(card, "mcopy", "-n", "::JT/HOME/ann/KEYCHAIN.BIN", out)
    kc = open(out, "rb").read() if os.path.exists(out) else b""
    check("ann's keychain is there and never holds a cookie value in plain text", kc[:4] == b"JTK1" and b"abc123" not in kc, kc[:8])
    check("no cookie value reaches the UART", all(v not in open(os.path.join(tmp, t + ".uart"), errors="replace").read() for t in ("one", "two", "three", "four") for v in ("abc123", "xyz789")))
finally:
    if S: S.stop()
    server.shutdown()
    subprocess.run(["make", "-C", arch, "clean"], capture_output=True)
if fails: print("FAIL: " + ", ".join(fails)); sys.exit(1)
print("PASS: a persistent cookie survives a restart in its owner's sealed keychain, a session cookie stays in memory, another account gets neither, and cookies clear is kept")
