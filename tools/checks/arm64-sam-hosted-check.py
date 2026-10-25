#!/usr/bin/env python3
"""Samantha without the relay (arch/arm64/ask.c hosted()): when the relay is out of reach, `sam` asks Turing's hosted
Samantha (turing.jaybulb.com /api/chat, Ollama's chat shape) over HTTPS with the public roots, before the local model.

A stub of that endpoint runs here over TLS (a throwaway CA passed as TLS_TA, a cert for 10.0.2.2), and the build names it
with SAM_HOSTED_HOST=10.0.2.2 and SAM_HOSTED_PORT. Headless QEMU, keys through QMP, the answer read off the UART.
  1. a release-style build (no relay, no token): `sam hello there` prints "samantha: no token", then "samantha: asking
     10.0.2.2" and her answer, escapes undone ("Samantha" in quotes, a second line). The stub saw the question and
     no Authorization header.
  2. a dev build whose relay is not listening: "samantha: error ..." first, then the hosted answer.
  3. the stub answers 500: "samantha: 10.0.2.2 did not answer" and no "sam:" line.
Discriminating: drop hosted() from fallback() and steps 1 and 2 print no answer.
Skips (exit 0) when clang's aarch64 target, ld.lld, openssl or qemu-system-aarch64 is missing.
Usage: tools/checks/arm64-sam-hosted-check.py   (from the repo root)
"""
import http.server, json, os, re, shutil, socket, ssl, subprocess, sys, tempfile, threading, time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from freeport import free_port

root = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
arch = os.path.join(root, "arch/arm64")
if not all(shutil.which(t) for t in ("clang", "ld.lld", "qemu-system-aarch64", "openssl")):
    print("SKIP: clang, ld.lld, openssl or qemu-system-aarch64 not installed"); sys.exit(0)

QUESTION = "hello there"
ANSWER = 'Hi, I am \\"Samantha\\".\\nAsk away.'   # as the Worker's JSON carries it
fails = []
def check(name, ok, detail=""):
    print(("  ok: " if ok else "  FAIL: ") + name + (("  (" + detail[-400:] + ")") if detail and not ok else ""))
    if not ok: fails.append(name)

tmp = tempfile.mkdtemp(prefix="jt-arm-hosted-")
def sh(*args): subprocess.run(args, cwd=tmp, check=True, capture_output=True)
sh("openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes", "-days", "365", "-subj", "/CN=hosted check CA",
   "-addext", "basicConstraints=critical,CA:TRUE", "-addext", "keyUsage=critical,keyCertSign,cRLSign", "-keyout", "ca.key", "-out", "ca.pem")
open(tmp + "/server.ext", "w").write("basicConstraints=critical,CA:FALSE\nkeyUsage=critical,digitalSignature,keyEncipherment\n"
                                     "extendedKeyUsage=serverAuth\nsubjectAltName=DNS:10.0.2.2,IP:10.0.2.2\n")
sh("openssl", "req", "-new", "-newkey", "rsa:2048", "-nodes", "-subj", "/CN=10.0.2.2", "-keyout", "s.key", "-out", "s.csr")
sh("openssl", "x509", "-req", "-in", "s.csr", "-CA", "ca.pem", "-CAkey", "ca.key", "-CAcreateserial", "-days", "30",
   "-sha256", "-extfile", "server.ext", "-out", "s.pem")

seen, mode = [], {"status": 200}
class Stub(http.server.BaseHTTPRequestHandler):
    def do_POST(self):
        body = self.rfile.read(int(self.headers.get("Content-Length", "0")))
        seen.append((self.path, dict(self.headers), body.decode("utf-8", "replace")))
        out = ('{"model":"samantha","message":{"role":"assistant","content":"%s"},"done":true}' % ANSWER).encode()
        self.send_response(mode["status"]); self.send_header("Content-Length", str(len(out))); self.end_headers(); self.wfile.write(out)
    def log_message(self, *a): pass
srv = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Stub)
ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER); ctx.load_cert_chain(tmp + "/s.pem", tmp + "/s.key")
srv.socket = ctx.wrap_socket(srv.socket, server_side=True)
threading.Thread(target=srv.serve_forever, daemon=True).start()
hport = srv.server_address[1]
token_file = tmp + "/token"
open(token_file, "w").write("arm-hosted-check-token-0123456789\n")

def build(dev):
    env = {k: v for k, v in os.environ.items() if not k.startswith(("CLAUDE_RELAY", "SAM_HOSTED", "JT_WIFI"))}
    env.update(SAM_HOSTED_HOST="10.0.2.2", SAM_HOSTED_PORT=str(hport))
    if dev: env.update(CLAUDE_RELAY_HOST="10.0.2.2", CLAUDE_RELAY_PORT=str(free_port()), CLAUDE_RELAY_TOKEN_FILE=token_file)
    subprocess.run(["make", "-C", arch, "clean"], capture_output=True)
    r = subprocess.run(["make", "-C", arch, "kernel8.elf", "TLS_TA=" + tmp + "/ca.pem"], env=env, capture_output=True, text=True, timeout=300)
    if r.returncode: print("FAIL: arch/arm64 does not build:\n" + r.stdout[-2000:] + r.stderr[-2000:]); sys.exit(1)

class Boot:
    def __init__(self, name):
        self.log, sock = "%s/%s.uart" % (tmp, name), "%s/%s.qmp" % (tmp, name)
        self.q = subprocess.Popen(["qemu-system-aarch64", "-machine", "virt", "-cpu", "cortex-a72", "-m", "256",
                                   "-global", "virtio-mmio.force-legacy=false", "-netdev", "user,id=n", "-device", "virtio-net-device,netdev=n",
                                   "-device", "ramfb", "-device", "virtio-keyboard-device", "-display", "none", "-serial", "file:" + self.log,
                                   "-qmp", "unix:%s,server,nowait" % sock, "-kernel", os.path.join(arch, "kernel8.elf")],
                                  stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        self.watchdog = threading.Timer(180, self.q.kill); self.watchdog.daemon = True; self.watchdog.start()
        if not self.wait_for("M2 input ready", 30): self.close(); raise SystemExit("FAIL: %s did not boot: %r" % (name, self.uart()[-600:]))
        self.wait_for("net dhcp", 15); time.sleep(0.5)
        self.s = socket.socket(socket.AF_UNIX); self.s.settimeout(20); self.s.connect(sock); self.f = self.s.makefile("rw")
        self.f.readline(); self.cmd("qmp_capabilities")
        self.cmd("send-key", keys=[{"type": "qcode", "data": "f2"}])
        if not self.wait_for("terminal open", 10): raise SystemExit("FAIL: F2 did not open the Terminal: %r" % self.uart()[-300:])
    def uart(self): return open(self.log, errors="replace").read() if os.path.exists(self.log) else ""
    def wait_for(self, pattern, secs):
        end = time.time() + secs
        while time.time() < end:
            if re.search(pattern, self.uart()): return True
            time.sleep(0.1)
        return False
    def cmd(self, c, **a):
        self.f.write(json.dumps({"execute": c, "arguments": a}) + "\n"); self.f.flush()
        while True:
            r = json.loads(self.f.readline())
            if "return" in r or "error" in r: return r
    def ask(self, text):
        for ch in text: self.cmd("send-key", keys=[{"type": "qcode", "data": "spc" if ch == " " else ch}]); time.sleep(0.12)
        self.cmd("send-key", keys=[{"type": "qcode", "data": "ret"}])
    def close(self): self.watchdog.cancel(); self.q.kill(); self.q.wait()

SAID = 'sam: Hi, I am "Samantha".'
try:
    build(dev=False)
    b = Boot("release")
    try:
        b.ask("sam " + QUESTION); b.wait_for("Ask away|did not answer", 60); out = b.uart()
        check("no token: \"samantha: no token\", then it asks the hosted Samantha", "samantha: no token\nsamantha: asking 10.0.2.2" in out, out[-400:])
        check("her answer prints with its escapes undone", SAID in out and "Ask away." in out, out[-400:])
        req = seen[-1] if seen else ("", {}, "")
        check("the stub got POST /api/chat with the question", req[0] == "/api/chat" and json.loads(req[2] or "{}").get("messages", [{}])[0].get("content") == QUESTION, repr(req)[:300])
        check("no Authorization header and no Pi status went out", "Authorization" not in req[1] and '"pi"' not in req[2], repr(req)[:300])
        mode["status"] = 500; n = len(seen)
        b.ask("sam " + QUESTION); b.wait_for("10.0.2.2 did not answer", 60); out = b.uart()
        check("a 500 from the host: \"did not answer\" and no second answer", len(seen) == n + 1 and "samantha: 10.0.2.2 did not answer" in out
              and out.count(SAID) == 1, out[-300:])
        mode["status"] = 200
    finally: b.close()

    build(dev=True)
    b = Boot("dev")
    try:
        b.ask("sam " + QUESTION); b.wait_for("Ask away|did not answer", 90); out = b.uart()
        check("a dev card whose relay is down: the relay error, then the hosted answer",
              re.search(r"samantha: (error|timeout)[^\n]*\n(.*\n)?samantha: asking 10\.0\.2\.2", out) is not None and SAID in out, out[-400:])
    finally: b.close()
finally:
    srv.shutdown()
    subprocess.run(["make", "-C", arch, "clean"], capture_output=True)
    shutil.rmtree(tmp, ignore_errors=True)
for m in fails: print("FAIL: " + m)
if fails: sys.exit(1)
print("PASS: with the relay out of reach, `sam` asks the hosted Samantha over HTTPS and prints her answer, with no token sent")
