#!/usr/bin/env python3
"""/model and /effort on the ARM Terminal's prompt (docs/AGENT.md), against a fake relay on the host.

One QEMU boot with a throwaway token pointed at a fake relay (it logs every request body and answers
"S <uuid>", "M Claude Opus 5.5"). Typed at the Terminal, in order:
  1. /model and /effort with no argument list the choices and mark the current one; nothing is sent.
  2. /model opus, /effort high, then a question: the request body carries "model":"opus" and "effort":"high".
  3. /status says model, effort, the prompt text ("Claude Opus 5.5 high $"), relay answered, steps used 1.
  4. /model gpt and /effort max are refused with one line each and nothing is sent; so are /help, /status, /bogus.
  5. /model auto, /effort medium, then a question: the body has neither field (the relay's defaults apply).
  6. /clear starts a new conversation: the next question carries an empty session.
Headless; python subprocess timeouts throughout. Skips where clang, lld or qemu-system-aarch64 is missing.
"""
import http.server, json, os, shutil, socket, subprocess, sys, tempfile, threading, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from freeport import free_port

root = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
arch = os.path.join(root, "arch/arm64")
if not all(shutil.which(t) for t in ("clang", "ld.lld", "qemu-system-aarch64")):
    print("SKIP: clang, ld.lld or qemu-system-aarch64 not installed"); sys.exit(0)

TOKEN = "arm-model-check-token-0123456789abc"
SID = "12345678-1234-1234-1234-123456789abc"
tmp = tempfile.mkdtemp(prefix="jt-arm-model-")
token_file = tmp + "/token"
with open(token_file, "w") as f: f.write(TOKEN + "\n")
os.chmod(token_file, 0o600)
fails = []
def check(name, ok, detail=""):
    print(("  ok: " if ok else "  FAIL: ") + name + (("  (" + detail + ")") if detail and not ok else ""))
    if not ok: fails.append(name)

calls = []
holding = threading.Event()
release = threading.Event()
class Relay(http.server.BaseHTTPRequestHandler):
    def do_POST(self):
        n = int(self.headers.get("Content-Length", "0")); calls.append(json.loads(self.rfile.read(n)))
        if calls[-1]["prompt"] == "hold":
            holding.set(); release.wait(10)
        body = ("S %s\nM Claude Opus 5.5\nANSWER%d" % (SID, len(calls))).encode(); self.send_response(200)
        self.send_header("Content-Type", "text/plain"); self.send_header("Content-Length", str(len(body)))
        self.send_header("Connection", "close"); self.end_headers(); self.wfile.write(body)
    def log_message(self, *a): pass
relay = http.server.HTTPServer(("127.0.0.1", free_port()), Relay)
relay_port = relay.server_address[1]
threading.Thread(target=relay.serve_forever, daemon=True).start()

class Boot:
    def __init__(self, name):
        self.log, sock = "%s/%s.uart" % (tmp, name), "%s/%s.qmp" % (tmp, name)
        self.q = subprocess.Popen(["qemu-system-aarch64", "-machine", "virt", "-cpu", "cortex-a72", "-m", "256",
                                   "-global", "virtio-mmio.force-legacy=false", "-netdev", "user,id=n", "-device",
                                   "virtio-net-device,netdev=n", "-device", "ramfb", "-device", "virtio-keyboard-device",
                                   "-display", "none", "-serial", "file:" + self.log, "-qmp", "unix:%s,server,nowait" % sock,
                                   "-kernel", os.path.join(arch, "kernel8.elf")], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        if not self.wait_for("M2 input ready", 30): raise SystemExit("FAIL: %s did not boot: %r" % (name, self.uart()[-600:]))
        time.sleep(0.5)
        self.s = socket.socket(socket.AF_UNIX); self.s.settimeout(20); self.s.connect(sock); self.f = self.s.makefile("rw")
        self.f.readline(); self.cmd("qmp_capabilities")
        n = self.uart().count("terminal open"); self.key("f2")   # the Terminal takes the typing; the Console is logs (docs/TERMINAL.md)
        if not self.wait_for("terminal open", 10, n + 1): raise SystemExit("FAIL: F1 did not open the Terminal: %r" % self.uart()[-300:])
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
        for ch in text: self.key({" ": "spc", "/": "slash"}.get(ch, ch))
        self.key("ret")
    def close(self): self.q.kill(); self.q.wait()

def say(b, text, expect, secs=30):
    """Type one line, wait until `expect` shows up after it on the UART, return what the UART gained."""
    before = len(b.uart()); b.ask(text)
    ok = b.wait_for(expect, secs) if expect not in b.uart()[:before] else b.wait_for(expect, secs, b.uart()[:before].count(expect) + 1)
    time.sleep(0.6)
    return b.uart()[before:] if ok else "TIMEOUT " + b.uart()[before:]

try:
    env = dict({k: v for k, v in os.environ.items() if not k.startswith("CLAUDE_RELAY")}, CLAUDE_RELAY_HOST="10.0.2.2",
               CLAUDE_RELAY_PORT=str(relay_port), CLAUDE_RELAY_TOKEN_FILE=token_file)
    r = subprocess.run(["make", "-C", arch], env=env, capture_output=True, text=True, timeout=300)
    if r.returncode: print("FAIL: arch/arm64 does not build:\n" + r.stdout[-2000:] + r.stderr[-2000:]); sys.exit(1)
    b = Boot("model")
    try:
        if not b.wait_for("net dhcp 10.0.2.15", 20): check("DHCP leased an address", False, b.uart()[-300:])

        out = say(b, "/model", "model: [auto]"); check("/model lists the choices and marks the current one", "model: [auto] haiku sonnet opus" in out, out[-300:])
        out = say(b, "/effort", "effort: low [medium] high"); check("/effort lists its choices, medium current", "effort: low [medium] high" in out, out[-300:])
        check("listing sends nothing", len(calls) == 0)

        out = say(b, "/model opus", "model: opus\n"); check("/model opus is accepted", "model: opus\n" in out, out[-300:])
        out = say(b, "/effort high", "effort: high\n"); check("/effort high is accepted", "effort: high\n" in out, out[-300:])
        out = say(b, "hi there", "ANSWER1")
        check("the request carries model opus and effort high", len(calls) == 1 and calls[0].get("model") == "opus" and calls[0].get("effort") == "high", repr(calls))
        check("the question text is not changed", calls and calls[0]["prompt"] == "hi there", repr(calls))

        out = say(b, "/status", "steps used")
        check("/status shows model and effort", "status: model opus, effort high" in out, out[-400:])
        check("/status shows the prompt with the model that answered", "status: prompt Claude Opus 5.5 high $ \n" in out, out[-400:])
        check("/status says the relay answered and one step was used", "relay answered last time" in out and "steps used 1" in out, out[-400:])

        n = len(calls)
        out = say(b, "/model gpt-4", "model: not one of"); check("an invalid model is refused in one line", "model: not one of auto, haiku, sonnet, opus\n" in out, out[-300:])
        out = say(b, "/effort max", "effort: not one of"); check("an invalid effort is refused in one line", "effort: not one of low, medium, high\n" in out, out[-300:])
        out = say(b, "/bogus", "unknown command"); check("an unknown slash command is refused", "unknown command, try /help" in out, out[-300:])
        out = say(b, "/help", "/clear  new conversation"); check("/help lists the commands", "/model [auto|haiku|sonnet|opus]" in out and "/effort [low|medium|high]" in out, out[-400:])
        check("none of those were sent", len(calls) == n, repr(len(calls)))
        out = say(b, "/model", "model: auto haiku sonnet [opus]"); check("a refused value left the model alone", "[opus]" in out, out[-300:])

        out = say(b, "/model auto", "model: auto\n"); out = say(b, "/effort medium", "effort: medium\n")
        out = say(b, "second", "ANSWER2")
        check("back to default: neither field is sent", len(calls) == 2 and "model" not in calls[1] and "effort" not in calls[1], repr(calls[1:]))
        check("the session carries over", calls[1].get("session") == SID, repr(calls[1:]))
        out = say(b, "/clear", "clear: new conversation"); out = say(b, "third", "ANSWER3")
        check("/clear starts a new conversation (empty session)", len(calls) == 3 and calls[2].get("session") == "", repr(calls[2:]))
        # Session 1 has auto/medium and a conversation. Session 2 starts independently.
        b.key("f3"); check("F3 opens session 2", b.wait_for("terminal session 2", 10))
        out = say(b, "/status", "steps used")
        check("session 2 starts with default settings", "status: model auto, effort medium" in out and "steps used 0" in out, out[-400:])
        say(b, "/model sonnet", "model: sonnet\n"); say(b, "/effort low", "effort: low\n")
        say(b, "fourth", "ANSWER4")
        check("session 2 sends its settings and an empty session", len(calls) == 4 and calls[3].get("session") == "" and calls[3].get("model") == "sonnet" and calls[3].get("effort") == "low", repr(calls[3:]))
        b.key("f3"); check("F3 returns to session 1", b.wait_for("terminal session 1", 10))
        say(b, "fifth", "ANSWER5")
        check("session 1 keeps its conversation and defaults", len(calls) == 5 and calls[4].get("session") == SID and "model" not in calls[4] and "effort" not in calls[4], repr(calls[4:]))
        # A partially typed command belongs to the session where it began.
        for ch in "saved": b.key(ch)
        b.key("f3"); say(b, "/clear", "clear: new conversation")
        b.key("f3"); b.key("ret"); check("unfinished text survives switching", b.wait_for("ANSWER6", 30) and len(calls) == 6 and calls[5]["prompt"] == "saved")
        out = say(b, "/status", "steps used")
        check("clearing session 2 leaves session 1 alone", "steps used 3" in out, out[-400:])
        before = b.uart().count("terminal session 2")
        b.ask("hold"); check("slow request reaches relay", holding.wait(10))
        b.key("f3"); time.sleep(0.4)
        check("F3 cannot move a running request", b.uart().count("terminal session 2") == before)
        release.set(); check("request finishes in its original session", b.wait_for("ANSWER7", 30))
        check("the token never appears on the UART", TOKEN not in b.uart())
    finally:
        b.close()
finally:
    release.set()
    relay.shutdown()
    subprocess.run(["make", "-C", arch, "clean"], capture_output=True, timeout=120)
    shutil.rmtree(tmp, ignore_errors=True)
if fails:
    print("FAIL: %d of the model and effort checks failed" % len(fails)); sys.exit(1)
print("PASS: /model and /effort change what the Pi sends, the prompt shows the answering model, a bad value is refused "
      "in one line and never sent, /status /help /clear run on the Pi")
