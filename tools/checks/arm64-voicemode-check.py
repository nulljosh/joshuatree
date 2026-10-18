#!/usr/bin/env python3
"""ARM64 voice mode: double-tap Space and hold the second tap to talk to Samantha, release to send. The heartest kernel on QEMU virt (no
microphone: a tone stands in). QMP holds the key down: the UART must say "voice: listening" and "mic mark on", the
release must say "voice: released", "mic mark off", post the samples to the stub words service and print "you: open
notes", which reaches the stub relay as the question. The two taps leave no spaces: a line typed around the hold comes out
as "hi" alone. A quick double tap without the hold stays two spaces. Skips where clang, ld.lld or qemu is missing.
Usage: tools/checks/arm64-voicemode-check.py   (from the repo root)
"""
import http.server, json, os, shutil, socket, subprocess, sys, tempfile, threading, time

root = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..")
arch = os.path.join(root, "arch/arm64")
if not all(shutil.which(t) for t in ("clang", "ld.lld", "qemu-system-aarch64")):
    print("SKIP: clang, ld.lld or qemu-system-aarch64 not installed"); sys.exit(0)
TOKEN = "hear-check-token-0123456789abcdef"
tmp = tempfile.mkdtemp(); token_file = tmp + "/token"; open(token_file, "w").write(TOKEN)
fails = []
def check(what, ok, detail=""):
    print(("  ok: " if ok else "  FAIL: ") + what + ("" if ok or not detail else " :: " + detail))
    if not ok: fails.append(what)

def free_pair():   # two free ports, P and P + 1, like relay.py at 8765 and stt.py at 8766
    for p in range(20000, 60000, 7):
        try:
            with socket.socket() as a, socket.socket() as b:
                a.bind(("127.0.0.1", p)); b.bind(("127.0.0.1", p + 1)); return p
        except OSError: continue
    sys.exit("no free port pair")
port = free_pair()
calls, heard, refuse = [], [], [False]
class Relay(http.server.BaseHTTPRequestHandler):
    def do_POST(self):
        n = int(self.headers.get("Content-Length", "0")); d = json.loads(self.rfile.read(n)); calls.append(d)
        body = b"S 12345678-1234-1234-1234-123456789abc\nOpening Notes. DONEWORD"
        self.send_response(200); self.send_header("Content-Type", "text/plain"); self.send_header("Content-Length", str(len(body)))
        self.send_header("Connection", "close"); self.end_headers(); self.wfile.write(body)
    def log_message(self, *a): pass
class Words(http.server.BaseHTTPRequestHandler):
    def do_POST(self):
        n = int(self.headers.get("Content-Length", "0")); pcm = self.rfile.read(n)
        heard.append((self.path, self.headers.get("Authorization"), n))
        if refuse[0] or self.headers.get("Authorization") != "Bearer " + TOKEN: body, st = b"no or wrong token", 401
        elif self.path != "/api/stt" or n % 2 or n < 8000: body, st = b"bad body", 400
        else: body, st = b"open notes", 200
        self.send_response(st); self.send_header("Content-Type", "text/plain"); self.send_header("Content-Length", str(len(body)))
        self.send_header("Connection", "close"); self.end_headers(); self.wfile.write(body)
    def log_message(self, *a): pass
relay = http.server.HTTPServer(("127.0.0.1", port), Relay); words = http.server.HTTPServer(("127.0.0.1", port + 1), Words)
for s in (relay, words): threading.Thread(target=s.serve_forever, daemon=True).start()

class Boot:
    def __init__(self):
        self.log, sock = tmp + "/uart", tmp + "/qmp"
        self.q = subprocess.Popen(["qemu-system-aarch64", "-machine", "virt", "-cpu", "cortex-a72", "-m", "256",
                                   "-global", "virtio-mmio.force-legacy=false", "-netdev", "user,id=n", "-device",
                                   "virtio-net-device,netdev=n", "-device", "ramfb", "-device", "virtio-keyboard-device",
                                   "-display", "none", "-serial", "file:" + self.log, "-qmp", "unix:%s,server,nowait" % sock,
                                   "-kernel", os.path.join(arch, "hear-kernel8.elf")], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        if not self.wait_for("M2 input ready", 30): raise SystemExit("FAIL: did not boot: %r" % self.uart()[-600:])
        time.sleep(0.5)
        self.s = socket.socket(socket.AF_UNIX); self.s.settimeout(20); self.s.connect(sock); self.f = self.s.makefile("rw")
        self.f.readline(); self.cmd("qmp_capabilities")
        n = self.uart().count("terminal open"); self.key("f2")
        if not self.wait_for("terminal open", 10, n + 1): raise SystemExit("FAIL: F2 did not open the Terminal: %r" % self.uart()[-300:])
        time.sleep(0.4)
    def uart(self): return open(self.log, errors="replace").read() if os.path.exists(self.log) else ""
    def wait_for(self, text, secs, count=1):
        end = time.time() + secs
        while time.time() < end:
            if self.uart().count(text) >= count: return True
            time.sleep(0.1)
        return False
    def cmd(self, c, **a):
        self.f.write(json.dumps({"execute": c, "arguments": a}) + "\n"); self.f.flush()
        while True:
            r = json.loads(self.f.readline())
            if "return" in r or "error" in r: return r
    def key(self, *qcodes):
        self.cmd("send-key", keys=[{"type": "qcode", "data": k} for k in qcodes]); time.sleep(0.12)
    def ask(self, text):
        for ch in text: self.key("spc" if ch == " " else ch)
        self.key("ret")
    def close(self): self.q.kill(); self.q.wait()

try:
    subprocess.run(["make", "-C", arch, "clean"], capture_output=True)   # a plain `make` before leaves claude_cfg.h without the token
    env = dict({k: v for k, v in os.environ.items() if not k.startswith("CLAUDE_RELAY")}, CLAUDE_RELAY_HOST="10.0.2.2",
               CLAUDE_RELAY_PORT=str(port), CLAUDE_RELAY_TOKEN_FILE=token_file)
    r = subprocess.run(["make", "-C", arch, "heartest"], env=env, capture_output=True, text=True, timeout=300)
    if r.returncode: print("FAIL: arch/arm64 `make heartest` does not build:\n" + r.stdout[-2000:] + r.stderr[-2000:]); sys.exit(1)
    b = Boot()
    def space(down): b.cmd("input-send-event", events=[{"type": "key", "data": {"down": down, "key": {"type": "qcode", "data": "spc"}}}])
    if not b.wait_for("net dhcp 10.0.2.15", 20): check("DHCP leased an address", False, b.uart()[-300:])
    # a quick double tap is two spaces
    for ch in "ab": b.key(ch)
    space(True); time.sleep(0.05); space(False); time.sleep(0.1); space(True); time.sleep(0.05); space(False); time.sleep(0.1)
    b.key("c"); b.key("ret"); time.sleep(0.5)
    out = b.uart()
    check("a quick double tap stays two spaces", "% ab  c" in out, out[-300:])
    # tap, then hold: listening; release: sent
    for ch in "hi": b.key(ch)
    space(True); time.sleep(0.05); space(False); time.sleep(0.1)
    space(True)
    listening = b.wait_for("voice: listening", 5)
    time.sleep(0.8)
    out2 = b.uart()
    space(False)
    b.wait_for("DONEWORD", 60); time.sleep(1)
    out3 = b.uart()
    check("holding Space starts listening", listening and "mic mark on" in out2, out2[-300:])
    check("the level bar is drawn while held", "listening [" in out2 and "release to send" in out2, out2[-300:])
    check("the release sends the samples with the token", "voice: released" in out3 and len(heard) == 1 and heard[0][0] == "/api/stt" and heard[0][1] == "Bearer " + TOKEN, repr(heard) + out3[-300:])
    check("the words came back and reached sam", "you: open notes" in out3 and len(calls) == 1 and calls[0]["prompt"].startswith("open notes"), repr(calls) + out3[-300:])
    check("the mic mark went away", "mic mark off" in out3, out3[-300:])
    b.key("ret"); time.sleep(0.5)
    out4 = b.uart()
    check("the taps left no spaces on the line", "% hi\n" in out4 and "% hi " not in out4, out4[-300:])
    check("the token never appears on the UART", TOKEN not in out4)
    b.close()
finally:
    relay.shutdown(); words.shutdown()
    shutil.rmtree(tmp, ignore_errors=True)
if fails:
    print("FAIL: %d of the voice mode checks failed" % len(fails)); sys.exit(1)
print("PASS: double-tap and hold Space listens with a mic mark and a level bar, the release sends the words to sam, and the taps leave no spaces")
