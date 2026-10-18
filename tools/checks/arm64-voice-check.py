#!/usr/bin/env python3
"""ARM64 voice: Samantha's [[say TEXT]] is spoken on the Pi. A stub relay answers `sam hi` with "[[say hello there]]" and
serves /api/tts (0.4 s of a 440 Hz 16-bit mono PCM at 22050 Hz, relay.py's wire shape); the Terminal must print the
caption, fetch the clip with the token and the text, report "voice: 8820 samples at 22050 Hz" and queue every sample
into the app sound ring ("voice: queued 8820 samples"; virt has no PWM, audio.c drains into nothing). Then the stub
answers /api/tts with 413 and the caption stands alone with "voice: no audio (413)". The token never reaches the UART.
Skips where clang, ld.lld or qemu is missing.
Usage: tools/checks/arm64-voice-check.py   (from the repo root)
"""
import http.server, json, math, re, os, shutil, socket, struct, subprocess, sys, tempfile, threading, time

root = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..")
arch = os.path.join(root, "arch/arm64")
if not all(shutil.which(t) for t in ("clang", "ld.lld", "qemu-system-aarch64")):
    print("SKIP: clang, ld.lld or qemu-system-aarch64 not installed"); sys.exit(0)
TOKEN = "voice-check-token-0123456789abcdef"
tmp = tempfile.mkdtemp(); token_file = tmp + "/token"; open(token_file, "w").write(TOKEN)
fails = []
def check(what, ok, detail=""):
    print(("  ok: " if ok else "  FAIL: ") + what + ("" if ok or not detail else " :: " + detail))
    if not ok: fails.append(what)
CLIP = b"".join(struct.pack("<h", int(8000 * math.sin(2 * math.pi * 440 * i / 22050))) for i in range(8820))
tts, calls, refuse = [], [], [False]
class Relay(http.server.BaseHTTPRequestHandler):
    def do_POST(self):
        n = int(self.headers.get("Content-Length", "0")); raw = self.rfile.read(n)
        auth_ok = self.headers.get("Authorization") == "Bearer " + TOKEN
        if self.path == "/api/tts":
            tts.append((auth_ok, json.loads(raw).get("text")))
            if refuse[0]: body, st, ct = b"text over the cap", 413, "text/plain"
            elif not auth_ok: body, st, ct = b"no", 401, "text/plain"
            else: body, st, ct = CLIP, 200, "application/octet-stream"
        else:   # the first turn speaks, the result turn ends the loop (relay.py's wire shape)
            calls.append(json.loads(raw).get("prompt", ""))
            text = b"Sure.\n[[say hello there]]" if len(calls) % 2 else b"Done."
            body, st, ct = b"S 12345678-1234-1234-1234-123456789abc\n" + text, 200, "text/plain"
        self.send_response(st); self.send_header("Content-Type", ct); self.send_header("Content-Length", str(len(body)))
        if st == 200 and self.path == "/api/tts": self.send_header("X-Sample-Rate", "22050")
        self.send_header("Connection", "close"); self.end_headers(); self.wfile.write(body)
    def log_message(self, *a): pass
relay = http.server.HTTPServer(("127.0.0.1", 0), Relay); port = relay.server_address[1]
threading.Thread(target=relay.serve_forever, daemon=True).start()

class Boot:
    def __init__(self):
        self.log, sock = tmp + "/uart", tmp + "/qmp"
        self.q = subprocess.Popen(["qemu-system-aarch64", "-machine", "virt", "-cpu", "cortex-a72", "-m", "256",
                                   "-global", "virtio-mmio.force-legacy=false", "-netdev", "user,id=n", "-device",
                                   "virtio-net-device,netdev=n", "-device", "ramfb", "-device", "virtio-keyboard-device",
                                   "-display", "none", "-serial", "file:" + self.log, "-qmp", "unix:%s,server,nowait" % sock,
                                   "-kernel", os.path.join(arch, "kernel8.elf")], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
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
    r = subprocess.run(["make", "-C", arch], env=env, capture_output=True, text=True, timeout=300)
    if r.returncode: print("FAIL: arch/arm64 does not build:\n" + r.stdout[-2000:] + r.stderr[-2000:]); sys.exit(1)
    b = Boot()
    if not b.wait_for("net dhcp 10.0.2.15", 20): check("DHCP leased an address", False, b.uart()[-300:])
    b.ask("sam hi")
    b.wait_for("voice: queued", 60); time.sleep(1)
    out = b.uart()
    check("the caption is printed", "say: hello there" in out, out[-500:])
    check("the clip was fetched with the token and the text", tts == [(True, "hello there")], repr(tts))
    check("the clip's size is reported", "voice: 8820 samples at 22050 Hz" in out, out[-500:])
    check("every sample reached the sound ring", "voice: queued 8820 samples" in out, out[-500:])
    check("the token never appears on the UART", TOKEN not in out)
    # the chat view: the face above the rows, the mouth moving with the clip
    b.ask("sam"); b.wait_for("face: on", 10); time.sleep(0.5)
    for ch in "hi": b.key(ch)
    b.key("ret")
    b.wait_for("voice: queued", 60); time.sleep(2)
    outf = b.uart()[len(out):]
    mouths = sorted(set(int(m) for m in re.findall(r"face: mouth=(\d+)", outf)))
    check("the chat view drew the face", "face: on" in outf, outf[-300:])
    check("the mouth moved with her voice", len(mouths) >= 2 and max(mouths) > 0, repr(mouths) + outf[-300:])
    b.key("esc"); time.sleep(0.5)
    out = b.uart()
    refuse[0] = True
    b.ask("sam again")
    b.wait_for("voice: no audio", 60); time.sleep(0.5)
    out2 = b.uart()[len(out):]
    check("a refusal leaves the caption and says so", "say: hello there" in out2 and "voice: no audio (413)" in out2 and "voice: queued" not in out2, out2[-400:])
    b.close()
finally:
    relay.shutdown()
    shutil.rmtree(tmp, ignore_errors=True)
if fails:
    print("FAIL: %d of the voice checks failed" % len(fails)); sys.exit(1)
print("PASS: [[say TEXT]] is fetched from the relay's /api/tts with the token and queued into the Pi's sound ring; a refusal leaves the caption")
