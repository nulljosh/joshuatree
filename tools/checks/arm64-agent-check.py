#!/usr/bin/env python3
"""Samantha's agent loop on the ARM Terminal (docs/AGENT.md), against a fake relay on the host.

Boots arch/arm64/kernel8.elf on QEMU virt with virtio-net and a keyboard, built with a throwaway token pointed at a
fake relay (a tiny HTTP server here, scripted answers, every prompt logged) and a plain http page server for
[[browse]]. Three boots, three scenarios:
  1. browse then answer: the answer ends with [[browse http://10.0.2.2:PORT/page]]; the Pi fetches the page and sends
     its text back as step 2, and the relay's final answer is printed. The log has "agent: step 1" and "agent: step 2".
  2. caps: every answer ends with five [[note N]] lines; only four run (the fifth stays as text) and the loop stops at
     "agent: step limit" after exactly 5 relay calls. Then an Esc after a step 1 answer stops the loop at step 2.
  3. unknown: [[reboot]] and [[browse ftp://x]] are logged as "agent: ignored [[...]]" and nothing runs; one call only.
Headless; python subprocess timeouts throughout. Skips where clang, lld or qemu-system-aarch64 is missing.
"""
import http.server, json, os, shutil, socket, subprocess, sys, tempfile, threading, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from freeport import free_port

root = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
arch = os.path.join(root, "arch/arm64")
if not all(shutil.which(t) for t in ("clang", "ld.lld", "qemu-system-aarch64")):
    print("SKIP: clang, ld.lld or qemu-system-aarch64 not installed"); sys.exit(0)

TOKEN = "arm-agent-check-token-0123456789abc"
PAGE = "The weather in Vancouver is rain. Marker PAGEWORD here."
tmp = tempfile.mkdtemp(prefix="jt-arm-agent-")
token_file = tmp + "/token"
with open(token_file, "w") as f: f.write(TOKEN + "\n")
os.chmod(token_file, 0o600)
fails = []
def check(name, ok, detail=""):
    print(("  ok: " if ok else "  FAIL: ") + name + (("  (" + detail + ")") if detail and not ok else ""))
    if not ok: fails.append(name)

# the page server: plain http, what [[browse]] fetches
class Page(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        body = ("<html><title>Weather</title><body><p>%s</p></body></html>" % PAGE).encode()
        self.send_response(200); self.send_header("Content-Type", "text/html"); self.send_header("Content-Length", str(len(body)))
        self.end_headers(); self.wfile.write(body)
    def log_message(self, *a): pass
pages = http.server.HTTPServer(("127.0.0.1", 0), Page)
page_port = pages.server_address[1]
threading.Thread(target=pages.serve_forever, daemon=True).start()

# the fake relay: the same wire shape as relay.py, answers from a script, logs every prompt
calls, script, delay = [], [], [0.0]   # delay: seconds before each answer, so a key can land mid-request
class Relay(http.server.BaseHTTPRequestHandler):
    def do_POST(self):
        n = int(self.headers.get("Content-Length", "0")); d = json.loads(self.rfile.read(n))
        calls.append(d); time.sleep(delay[0])
        if self.headers.get("Authorization") != "Bearer " + TOKEN:
            body = b"Wrong or missing relay token."; self.send_response(401)
        else:
            text = script[min(len(calls), len(script)) - 1]
            body = ("S 12345678-1234-1234-1234-123456789abc\n" + text).encode(); self.send_response(200)
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
        for ch in text: self.key("spc" if ch == " " else ch)
        self.key("ret")
    def close(self): self.q.kill(); self.q.wait()

def run(name, lines, question, settle):
    """One boot: script the relay, type the question, wait for the loop to end, return the UART text."""
    del calls[:]; script[:] = lines
    b = Boot(name)
    try:
        if not b.wait_for("net dhcp 10.0.2.15", 20): check(name + ": DHCP leased an address", False, b.uart()[-300:])
        b.ask(question)
        b.wait_for(settle, 60)
        time.sleep(1.5)
        return b, b.uart()
    except BaseException:
        b.close(); raise

try:
    print("arm64 agent loop: fake relay on 127.0.0.1:%d, page server on %d" % (relay_port, page_port))
    env = dict({k: v for k, v in os.environ.items() if not k.startswith("CLAUDE_RELAY")}, CLAUDE_RELAY_HOST="10.0.2.2",
               CLAUDE_RELAY_PORT=str(relay_port), CLAUDE_RELAY_TOKEN_FILE=token_file)
    r = subprocess.run(["make", "-C", arch], env=env, capture_output=True, text=True, timeout=300)
    if r.returncode: print("FAIL: arch/arm64 does not build:\n" + r.stdout[-2000:] + r.stderr[-2000:]); sys.exit(1)

    # 1. browse, then a final answer built from the page
    url = "http://10.0.2.2:%d/page" % page_port
    b, out = run("browse", ["Let me look.\n[[browse %s]]" % url, "It is raining in Vancouver. FINALWORD"], "weather", "FINALWORD")
    b.close()
    check("step 1 and step 2 are logged", "agent: step 1\n" in out and "agent: step 2\n" in out, out[-600:])
    check("the relay was called twice", len(calls) == 2, repr(len(calls)))
    check("the Pi fetched the page and printed it", "PAGEWORD" in out, out[-600:])
    check("step 2 sent the page text back as the next turn, in the same session",
          len(calls) == 2 and "PAGEWORD" in calls[1]["prompt"] and calls[1]["prompt"].startswith("The Pi ran your actions")
          and calls[1]["session"] == "12345678-1234-1234-1234-123456789abc", repr(calls[1:]))
    check("the final answer is printed and no third step runs", "FINALWORD" in out and "agent: step 3" not in out)
    check("the token never appears on the UART", TOKEN not in out)

    # 2. the caps: 4 actions per turn, 5 steps per question, then Esc
    five = "More.\n" + "".join("[[note n%d]]\n" % i for i in range(1, 6))
    b, out = run("caps", [five] * 6, "go", "agent: step limit")
    check("only four notes ran per turn, the fifth stayed as text", out.count("pi: n4") >= 1 and "pi: n5" not in out and "[[note n5]]" in out, out[-800:])
    check("the loop stopped at five steps", "agent: step 5\n" in out and "agent: step 6" not in out and "agent: step limit" in out, out[-400:])
    check("the relay was called exactly five times", len(calls) == 5, repr(len(calls)))
    check("each result turn fits the relay's body cap", all(len(c["prompt"]) <= 1200 for c in calls[1:]), repr([len(c["prompt"]) for c in calls]))
    # Esc during a loop: typed while step 1's request is in flight, read between steps
    del calls[:]; delay[0] = 1.5
    b.ask("again"); b.key("esc")
    b.wait_for("agent: stopped", 30); time.sleep(1)
    out2 = b.uart()[len(out):]
    b.close()
    delay[0] = 0.0
    check("Esc stops the loop after one step", "agent: stopped" in out2 and "agent: step 2" not in out2 and len(calls) == 1, out2[-400:])

    # 3. unknown actions are ignored and logged
    b, out = run("unknown", ["Hm.\n[[reboot]]\n[[browse ftp://x]]\n[[note fine]]", "All good. DONEWORD"], "hi", "DONEWORD")
    b.close()
    check("[[reboot]] and a non-http browse are logged and ignored", "agent: ignored [[reboot]]" in out and "agent: ignored [[browse ftp://x]]" in out, out[-400:])
    check("the known action still ran", "pi: fine" in out)
    check("the loop sends only the known action's result and stops at the relay's second answer", len(calls) == 2 and "reboot" not in calls[1]["prompt"] and "note fine" in calls[1]["prompt"] and "agent: step 3" not in out, repr(calls[1:]))
finally:
    relay.shutdown(); pages.shutdown()
    subprocess.run(["make", "-C", arch, "clean"], capture_output=True, timeout=120)
    shutil.rmtree(tmp, ignore_errors=True)
if fails:
    print("FAIL: %d of the agent loop checks failed" % len(fails)); sys.exit(1)
print("PASS: the ARM Terminal runs Samantha's actions and sends their results back as the next turn: browse then answer, "
      "4 actions per turn and 5 steps, Esc stops it, unknown actions are logged and ignored")
