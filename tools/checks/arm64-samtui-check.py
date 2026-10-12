#!/usr/bin/env python3
"""`sam` with no question opens Samantha as a full-window chat view inside the ARM Terminal (docs/TERMINAL.md, GUIDE.md).

Built like arm64-samfast-check.py: TLS on, a throwaway private CA, the real tools/claude-relay/relay.py with a stub `claude`
that waits STUB_DELAY seconds before it answers, the relay at the baked host 10.0.2.2. Keys go in through QMP send-key on a
virtio keyboard; everything is read from the UART, where the chat view writes each row it draws as `tui| ...` (the same text
it hands the renderer). One boot, Terminal open (F2):
  1. `sam` alone: `tui| open`, the header `Samantha ... local only` (no relay found yet), the input row `you: `, and the
     Terminal's own log still has no Samantha line. The stub is never asked.
  2. a sum: `you: 10+10` and `sam: 20` land in the history, instantly, with no `thinking` marker and nothing sent to the relay.
  3. a question: the `thinking...` marker shows while the stub waits, keys typed meanwhile land on the input row, then
     `sam: ANSWER` lands in the history, the marker goes, and the header now names `relay 10.0.2.2`. The stub has the one
     question. A sum typed meanwhile is `sam: still thinking`, as in the shell.
  4. scroll: with more lines than the window, Up, Page Up and Home move the view back (`up N` grows, the last line shown
     falls), Down, Page Down and End bring it back to `up 0`. A new line snaps the view to the newest.
  5. Esc: `tui| closed`, the screen changes (a screendump differs), the zsh prompt works (`sam 3*3` prints 9, the shell's
     `sam: 9` with no `tui|`), and the busy flag is clear (a relay question runs with no `still thinking`).
  6. Esc while she is thinking: the marker says it will leave, the view closes after her answer, and the next question is not
     refused. `/quit` leaves too. A key that is not the chat view's still works (`/status` prints the model line in the history).
Discriminating: print the usage line for a bare `sam` and step 1 fails; run sums through the relay and step 2 fails; let Esc
return to the desktop without clearing the view and step 5 fails; skip the busy-flag reset and step 6's next sum fails.
Skips (exit 0) when clang's aarch64 target, ld.lld, openssl or qemu-system-aarch64 is missing.
Usage: tools/checks/arm64-samtui-check.py   (from the repo root)
"""
import hashlib, json, os, re, shutil, socket, subprocess, sys, tempfile, time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from freeport import free_port

root = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
arch = os.path.join(root, "arch/arm64")
relay_py = os.path.join(root, "tools/claude-relay/relay.py")
if not all(shutil.which(t) for t in ("clang", "ld.lld", "qemu-system-aarch64", "openssl")):
    print("SKIP: clang, ld.lld, openssl or qemu-system-aarch64 not installed"); sys.exit(0)

TOKEN = "arm-samtui-check-token-0123456789a"
ANSWER = "the chat view works"
DELAY = 6
STUB = r'''#!/usr/bin/env python3
import json, os, sys, time, uuid
prompt = sys.stdin.read()
with open(os.environ["STUB_LOG"], "a") as f: f.write(json.dumps({"stdin": prompt}) + "\n")
time.sleep(float(os.environ["STUB_DELAY"]))
print(json.dumps({"type": "result", "subtype": "success", "is_error": False, "result": os.environ["STUB_ANSWER"], "session_id": str(uuid.uuid4())}))
'''
fails = []
def check(name, ok, detail=""):
    print(("  ok: " if ok else "  FAIL: ") + name + (("  (" + detail[-500:] + ")") if detail and not ok else ""))
    if not ok: fails.append(name)

tmp = tempfile.mkdtemp(prefix="jt-arm-samtui-")
os.makedirs(tmp + "/bin")
with open(tmp + "/bin/claude", "w") as f: f.write(STUB)
os.chmod(tmp + "/bin/claude", 0o755)
token_file = tmp + "/token"
with open(token_file, "w") as f: f.write(TOKEN + "\n")
os.chmod(token_file, 0o600)
stub_log = tmp + "/stub.log"
port = free_port()

def sh(*args):
    subprocess.run(args, cwd=tmp, check=True, capture_output=True)
sh("openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes", "-days", "365", "-subj", "/CN=Joshua Tree relay CA",
   "-addext", "basicConstraints=critical,CA:TRUE", "-addext", "keyUsage=critical,keyCertSign,cRLSign", "-keyout", "ca.key", "-out", "ca.pem")
with open(tmp + "/server.ext", "w") as f:
    f.write("basicConstraints=critical,CA:FALSE\nkeyUsage=critical,digitalSignature,keyEncipherment\n"
            "extendedKeyUsage=serverAuth\nsubjectAltName=DNS:jt-relay\n")
sh("openssl", "req", "-new", "-newkey", "rsa:2048", "-nodes", "-subj", "/CN=jt-relay", "-keyout", "jt-relay.key", "-out", "server.csr")
sh("openssl", "x509", "-req", "-in", "server.csr", "-CA", "ca.pem", "-CAkey", "ca.key", "-CAcreateserial", "-days", "30",
   "-sha256", "-extfile", "server.ext", "-out", "jt-relay.pem")

base_env = {k: v for k, v in os.environ.items() if not k.startswith("CLAUDE_RELAY")}
def stub_calls():
    try: return [json.loads(l) for l in open(stub_log)]
    except FileNotFoundError: return []

relay = None
class Boot:
    def __init__(self):
        self.log, self.sock = tmp + "/boot.uart", tmp + "/boot.qmp"
        self.q = subprocess.Popen(["qemu-system-aarch64", "-machine", "virt", "-cpu", "cortex-a72", "-m", "256",
                                   "-global", "virtio-mmio.force-legacy=false", "-netdev", "user,id=n", "-device", "virtio-net-device,netdev=n",
                                   "-device", "ramfb", "-device", "virtio-keyboard-device", "-display", "none", "-serial", "file:" + self.log,
                                   "-qmp", "unix:%s,server,nowait" % self.sock, "-kernel", os.path.join(arch, "kernel8.elf")],
                                  stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        if not self.wait_for("M2 input ready", 30): raise SystemExit("FAIL: did not boot: %r" % self.uart()[-600:])
        time.sleep(0.5)
        self.s = socket.socket(socket.AF_UNIX); self.s.settimeout(20); self.s.connect(self.sock); self.f = self.s.makefile("rw")
        self.f.readline(); self.cmd("qmp_capabilities")
        n = self.uart().count("terminal open"); self.key("f2")
        if not self.wait_for("terminal open", 10, n + 1): raise SystemExit("FAIL: F2 did not open the Terminal")
        time.sleep(0.4)
    def uart(self): return open(self.log, errors="replace").read() if os.path.exists(self.log) else ""
    def wait_for(self, text, secs, count=1, since=0):
        end = time.time() + secs
        while time.time() < end:
            if self.uart()[since:].count(text) >= count: return True
            time.sleep(0.1)
        return False
    def cmd(self, c, **a):
        self.f.write(json.dumps({"execute": c, "arguments": a}) + "\n"); self.f.flush()
        while True:
            r = json.loads(self.f.readline())
            if "return" in r or "error" in r: return r
    def key(self, *qcodes):
        self.cmd("send-key", keys=[{"type": "qcode", "data": k} for k in qcodes]); time.sleep(0.12)
    SHIFT = {"(": ("shift", "9"), ")": ("shift", "0"), "+": ("shift", "equal"), "*": ("shift", "8"), "/": ("slash",), "-": ("minus",), ".": ("dot",), " ": ("spc",), "?": ("shift", "slash")}
    def type(self, text):
        for ch in text: self.key(*self.SHIFT.get(ch, (ch,)))
    def run(self, line): self.type(line); self.key("ret")
    def mark(self): return len(self.uart())
    def screen(self):   # a hash of the screen, or None when this QEMU cannot dump it
        path = tmp + "/screen.ppm"
        if os.path.exists(path): os.remove(path)
        r = self.cmd("screendump", filename=path)
        time.sleep(0.4)
        return hashlib.md5(open(path, "rb").read()).hexdigest() if "error" not in r and os.path.exists(path) else None
    def view(self, since):   # the last `tui| view A-B/N up U` since `since`: (last shown, total, up)
        m = re.findall(r"tui\| view (\d+)-(\d+)/(\d+) up (\d+)", self.uart()[since:])
        return (int(m[-1][1]), int(m[-1][2]), int(m[-1][3])) if m else None
    def close(self): self.q.kill(); self.q.wait()

try:
    print("arm64 samtui: relay on 127.0.0.1:%d (TLS, baked host 10.0.2.2), stub answers after %ds" % (port, DELAY))
    subprocess.run(["make", "-C", arch, "clean"], check=True, capture_output=True, timeout=120)
    env = dict(base_env, CLAUDE_RELAY_HOST="10.0.2.2", CLAUDE_RELAY_PORT=str(port), CLAUDE_RELAY_TOKEN_FILE=token_file, CLAUDE_RELAY_TLS="1")
    r = subprocess.run(["make", "-C", arch, "TLS_TA=" + tmp + "/ca.pem"], env=env, capture_output=True, text=True, timeout=600)
    if r.returncode: print("FAIL: arch/arm64 does not build:\n" + r.stdout[-2000:] + r.stderr[-2000:]); sys.exit(1)
    renv = dict(base_env, PATH=tmp + "/bin" + os.pathsep + os.environ["PATH"], STUB_LOG=stub_log, STUB_ANSWER=ANSWER,
                STUB_DELAY=str(DELAY), CLAUDE_RELAY_TOKEN=TOKEN)
    err = open(tmp + "/relay.log", "w+")
    relay = subprocess.Popen([sys.executable, relay_py, "--port", str(port), "--cwd", tmp, "--timeout", "30",
                              "--tls-cert", tmp + "/jt-relay.pem", "--tls-key", tmp + "/jt-relay.key"],
                             env=renv, stdout=subprocess.DEVNULL, stderr=err)
    for _ in range(100):
        time.sleep(0.05); err.seek(0)
        if re.search(r"listening on ([\d.]+):(\d+)", err.read()): break
    else: raise SystemExit("FAIL: the relay did not start")

    b = Boot()
    try:
        # 1. open
        shell = b.screen()
        m = b.mark(); b.run("sam")
        check("sam alone opens the chat view, not a usage line", b.wait_for("tui| open\n", 10, since=m) and "usage: sam" not in b.uart()[m:], b.uart()[m:][-300:])
        time.sleep(0.6)
        out = b.uart()[m:]
        check("the header is Samantha and says local only before any relay", re.search(r"tui\| head Samantha +local only\n", out) is not None, out[-300:])
        check("the input row is `you: `", "tui| input you: \n" in out, out[-300:])
        opened = b.screen()
        check("the screen changed (the chat view is drawn)", shell is None or opened != shell)
        check("the stub was not asked", not stub_calls())
        b.type("ab")
        check("typing shows on the input row", b.wait_for("tui| input you: ab\n", 5, since=m), b.uart()[m:][-200:])
        typed = b.screen()
        check("... and on the screen", opened is None or typed != opened)
        b.key("backspace"); b.key("backspace")
        # 2. a sum
        m = b.mark(); t0 = time.time(); b.run("10+10")
        check("a sum is answered in the history on the Pi", b.wait_for("tui| line sam: 20\n", 5, since=m) and "tui| line you: 10+10\n" in b.uart()[m:], b.uart()[m:][-300:])
        check("... at once, with no thinking marker and nothing sent", time.time() - t0 < 4 and "tui| marker" not in b.uart()[m:] and not stub_calls())
        m = b.mark(); b.run("what is 2*(3+4)")
        check("a worded sum too", b.wait_for("tui| line sam: 14\n", 5, since=m), b.uart()[m:][-300:])
        # 3. a question
        m = b.mark(); b.run("hello there friend")
        check("her question shows your line and the thinking marker", b.wait_for("tui| marker thinking...\n", 10, since=m) and "tui| line you: hello there friend\n" in b.uart()[m:], b.uart()[m:][-300:])
        b.type("zz")
        check("keys typed while she thinks land on the input row", b.wait_for("tui| input you: zz\n", 5, since=m), b.uart()[m:][-200:])
        b.key("backspace"); b.key("backspace")
        m2 = b.mark(); b.run("1+1")
        check("a sum typed meanwhile is still thinking, as in the shell", b.wait_for("tui| line sam: still thinking\n", 5, since=m2) and "tui| line sam: 2\n" not in b.uart()[m2:], b.uart()[m2:][-300:])
        check("the answer lands in the history", b.wait_for("tui| line sam: " + ANSWER + "\n", DELAY + 20, since=m), b.uart()[m:][-400:])
        time.sleep(0.8)
        out = b.uart()[m:]
        check("the stub had the one question", len(stub_calls()) == 1 and stub_calls()[0]["stdin"] == "hello there friend", repr(stub_calls()))
        check("the progress lines stay out of the history", "tui| line agent: step" not in out and "tui| line samantha: thinking" not in out and not re.search(r"tui\| line sam: \d+\.\ds \(", out), out[-400:])
        check("the header now names the relay", re.search(r"tui\| head Samantha +relay 10\.0\.2\.2\n", out) is not None, out[-300:])
        v = b.view(m)
        check("the marker is gone", v is not None and "tui| marker" not in out[out.rindex("tui| view"):], out[-300:])
        # 4. scroll
        for i in range(14): b.run("%d+%d" % (i, i + 1))
        time.sleep(1.0)
        m = b.mark(); b.run("1+2")
        b.wait_for("tui| line sam: 3\n", 5, since=m); time.sleep(0.5)
        base = b.view(m)
        check("the view follows the newest line (up 0)", base is not None and base[2] == 0 and base[0] == base[1], repr(base))
        bottom = b.screen()
        m = b.mark(); b.key("up"); time.sleep(0.4)
        v1 = b.view(m)
        check("Up scrolls the history back one line", v1 is not None and base is not None and v1[2] == 1 and v1[0] == base[0] - 1, repr((base, v1)))
        m = b.mark(); b.key("pgup"); time.sleep(0.4)
        v2 = b.view(m)
        check("Page Up goes back further", v2 is not None and v1 is not None and v2[2] > v1[2], repr((v1, v2)))
        m = b.mark(); b.key("home"); time.sleep(0.4)
        v3 = b.view(m)
        check("Home goes to the oldest line", v3 is not None and v3[2] >= v2[2], repr((v2, v3)))
        scrolled = b.screen()
        check("... and the screen shows it", bottom is None or scrolled != bottom)
        m = b.mark(); b.key("down"); time.sleep(0.4)
        v4 = b.view(m)
        check("Down comes forward one line", v4 is not None and v4[2] == v3[2] - 1, repr((v3, v4)))
        m = b.mark(); b.key("pgdn"); time.sleep(0.3); b.key("end"); time.sleep(0.4)
        v5 = b.view(m)
        check("Page Down and End are back at the newest", v5 is not None and v5[2] == 0, repr(v5))
        m = b.mark(); b.key("up"); b.key("up"); time.sleep(0.3); b.run("5+5")
        b.wait_for("tui| line sam: 10\n", 5, since=m); time.sleep(0.4)
        v6 = b.view(m)
        check("a new line snaps the view to the newest", v6 is not None and v6[2] == 0, repr(v6))
        # 5. Esc
        m = b.mark(); b.type("half"); b.key("esc")
        check("Esc closes the chat view", b.wait_for("tui| closed\n", 5, since=m), b.uart()[m:][-300:])
        time.sleep(0.5)
        back = b.screen()
        check("the screen went back to the shell", opened is None or back != opened)
        m = b.mark(); b.run("sam 3*3")
        check("the shell works: sam 3*3 prints 9 with no chat view", b.wait_for("sam: 9\n", 5, since=m) and "tui|" not in b.uart()[m:] and "still thinking" not in b.uart()[m:], b.uart()[m:][-300:])
        m = b.mark(); b.run("sam is this still ok")
        check("the busy flag is clear: a relay question runs", b.wait_for("samantha: thinking", 10, since=m) and "still thinking" not in b.uart()[m:], b.uart()[m:][-300:])
        check("... and its answer prints in the shell", b.wait_for("sam: " + ANSWER, DELAY + 20, since=m) and "tui|" not in b.uart()[m:], b.uart()[m:][-300:])
        time.sleep(0.8)
        # 6. Esc while she thinks, /quit, and slash commands
        m = b.mark(); b.run("sam"); b.wait_for("tui| open\n", 10, since=m)
        check("reopened: the header already knows the relay", re.search(r"tui\| head Samantha +relay 10\.0\.2\.2\n", b.uart()[m:]) is not None, b.uart()[m:][-300:])
        check("... and the earlier lines are still there", re.search(r"tui\| view 1-\d+/\d+ up 0\n", b.uart()[m:]) is None and b.view(m) is not None and b.view(m)[1] > 10, repr(b.view(m)))
        m = b.mark(); b.run("/status")
        check("/status prints into the history", b.wait_for("tui| line status: model auto, effort medium\n", 5, since=m), b.uart()[m:][-300:])
        m = b.mark(); b.run("another question please")
        b.wait_for("tui| marker thinking...\n", 10, since=m)
        b.key("esc")
        check("Esc while she thinks says it will leave when she answers", b.wait_for("tui| marker thinking (leaving when she answers)\n", 5, since=m), b.uart()[m:][-300:])
        check("... and has not closed yet", "tui| closed" not in b.uart()[m:])
        check("... then closes once her answer is in", b.wait_for("tui| closed\n", DELAY + 20, since=m) and "tui| line sam: " + ANSWER + "\n" in b.uart()[m:], b.uart()[m:][-400:])
        time.sleep(0.8)
        m = b.mark(); b.run("sam 6*7")
        check("the busy flag is clear after leaving mid-question", b.wait_for("sam: 42\n", 5, since=m) and "still thinking" not in b.uart()[m:], b.uart()[m:][-300:])
        m = b.mark(); b.run("sam"); b.wait_for("tui| open\n", 10, since=m)
        m = b.mark(); b.run("/quit")
        check("/quit leaves the chat view", b.wait_for("tui| closed\n", 5, since=m), b.uart()[m:][-300:])
        time.sleep(0.5)
        m = b.mark(); b.run("sam 8*8")
        check("... to a working prompt", b.wait_for("sam: 64\n", 5, since=m), b.uart()[m:][-200:])
        check("the stub was asked for exactly the three relay questions (two in the chat view, one in the shell)", len(stub_calls()) == 3, repr(stub_calls()))
        check("the token never appears on the UART", TOKEN not in b.uart())
    finally:
        b.close()
finally:
    if relay: relay.kill(); relay.wait()
    subprocess.run(["make", "-C", arch, "clean"], capture_output=True, timeout=120)
    shutil.rmtree(tmp, ignore_errors=True)
if fails:
    print("FAIL: %d of the samtui checks failed" % len(fails)); sys.exit(1)
print("PASS: bare sam opens a chat view in the Terminal: header, history, input row, sums answered on the Pi, relay replies with a thinking marker, "
      "Up/Page/Home/End scroll, and Esc or /quit return to a working zsh prompt with the busy flag clear")
