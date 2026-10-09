#!/usr/bin/env python3
"""Claude in the ARM Terminal: open the Terminal (F1), type a question at its prompt (`Claude Haiku 5.5 $ `, naming the model) and Claude's answer is printed in
the Terminal, through the real relay on this machine and the shared IP stack on virtio-net. No app, no model, no
internet. The Console is logs only: typing with it in front asks nothing, and the answer never lands in it
(docs/TERMINAL.md). The file keeps its old name so the suite and the docs that point at it stay put.

The host side is the real tools/claude-relay/relay.py, bound to 127.0.0.1 on a free port, with a throwaway token file and
a stub `claude` first on PATH (it records its argv and stdin and answers in Claude Code's JSON shape). The kernel is
built with CLAUDE_RELAY_TOKEN_FILE pointing at that throwaway file and CLAUDE_RELAY_PORT at that port; QEMU's user
network reaches the host's loopback at 10.0.2.2. Keys go in through QMP send-key on a virtio keyboard, Enter included
(the i386 harness limit on Enter does not apply to the ARM virtio keyboard).

Boot 1, virtio-net, ramfb, virtio keyboard:
  0. the Console has no input: "hi" and Enter with it in front reach nobody. Then F1 opens the Terminal.
  1. typing: the letters land on the prompt row (its pixels change), Backspace takes one back, and the per-key echo
     lines stay on the UART only.
  2. ask: Enter sends; "claude: thinking", the stub got exactly the typed text on stdin, and the stub's answer comes back
     word for word, wrapped to lines of at most 53 columns, and the Terminal window gains ink. A reply at all proves the
     bearer was sent: the relay answers 401 without it. F1 back to the Console: the answer's ##### line is not there.
  3. follow-up: a second question reaches the stub with --resume <the session the first reply named>. The relay runs
     with --model claude-sonnet-5-5, so the first question is echoed after the default prompt "Claude Haiku 5.5 $ "
     and the follow-up after "Claude Sonnet 5.5 $ ", the model the relay said answered.
  4. wrong token: the relay restarts on the same port with another token; the next question prints "claude: error -401".
  The token never shows up on the UART.
Boot 2, no network card: the prompt is the default "Claude Haiku 5.5 $ ", a question prints "claude: no network" and the
relay is never asked.
Boot 3, built with no token file: the prompt is "Claude $ " (no relay, so no known model), a question prints
"claude: no token" and the relay is never asked.

Discriminating: point ask.c at another path ("/api/claudx") and step 2 fails with "claude: error -404": the stub is
never asked, no answer on the UART and no ##### line on the screen.
Skips (exit 0) when clang's aarch64 target, ld.lld or qemu-system-aarch64 is missing.
Usage: tools/checks/arm64-claude-console-check.py   (from the repo root)
"""
import json, os, re, shutil, socket, subprocess, sys, tempfile, time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from freeport import free_port

root = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
arch = os.path.join(root, "arch/arm64")
relay_py = os.path.join(root, "tools/claude-relay/relay.py")
if not all(shutil.which(t) for t in ("clang", "ld.lld", "qemu-system-aarch64")):
    print("SKIP: clang, ld.lld or qemu-system-aarch64 not installed"); sys.exit(0)

TOKEN = "arm-console-check-token-0123456789"
OTHER = "arm-console-other-token-9876543210"
QUESTION = "what is in version?"
SECOND = "and again"
ANSWER = ("The VERSION file holds the release number of Joshua Tree, and the Console wraps this answer "
          "at a space so that every line fits the fifty three columns of a Pi screen.")
DEFAULT = "Claude Haiku 5.5 $ "    # ask.c's prompt before any answer
SONNET = "Claude Sonnet 5.5 $ "    # after the relay (--model claude-sonnet-5-5) answered
PROMPT_RE = re.compile(r"Claude(?: [A-Z][a-z]+ \d+\.\d+)? \$ ")
MARK = "#" * 50   # the answer's last line: denser than any boot line, so a screendump can find it
STUB = r'''#!/usr/bin/env python3
import json, os, sys, uuid
prompt = sys.stdin.read()
with open(os.environ["STUB_LOG"], "a") as f: f.write(json.dumps({"argv": sys.argv[1:], "stdin": prompt}) + "\n")
sid = sys.argv[sys.argv.index("--resume") + 1] if "--resume" in sys.argv else str(uuid.uuid4())
print(json.dumps({"type": "result", "subtype": "success", "is_error": False, "result": os.environ["STUB_ANSWER"], "session_id": sid}))
'''
fails = []
def check(name, ok, detail=""):
    print(("  ok: " if ok else "  FAIL: ") + name + (("  (" + detail + ")") if detail and not ok else ""))
    if not ok: fails.append(name)

tmp = tempfile.mkdtemp(prefix="jt-arm-claude-")
os.makedirs(tmp + "/bin")
with open(tmp + "/bin/claude", "w") as f: f.write(STUB)
os.chmod(tmp + "/bin/claude", 0o755)
token_file = tmp + "/token"
with open(token_file, "w") as f: f.write(TOKEN + "\n")
os.chmod(token_file, 0o600)
stub_log = tmp + "/stub.log"
port = free_port()
base_env = {k: v for k, v in os.environ.items() if not k.startswith("CLAUDE_RELAY")}

def stub_calls():
    try: return [json.loads(l) for l in open(stub_log)]
    except FileNotFoundError: return []

def build(with_token):
    env = dict(base_env, CLAUDE_RELAY_HOST="10.0.2.2", CLAUDE_RELAY_PORT=str(port),
               CLAUDE_RELAY_TOKEN_FILE=token_file if with_token else tmp + "/no-such-token")
    r = subprocess.run(["make", "-C", arch], env=env, capture_output=True, text=True, timeout=300)
    if r.returncode: print("FAIL: arch/arm64 does not build:\n" + r.stdout[-2000:] + r.stderr[-2000:]); sys.exit(1)
    return r.stdout + r.stderr

relay = None
def start_relay(token):
    global relay
    stop_relay()
    env = dict(base_env, PATH=tmp + "/bin" + os.pathsep + os.environ["PATH"], STUB_LOG=stub_log, STUB_ANSWER=ANSWER + "\n" + MARK,
               CLAUDE_RELAY_TOKEN=token)
    err = open(tmp + "/relay-%d.log" % time.time_ns(), "w+")
    relay = subprocess.Popen([sys.executable, relay_py, "--port", str(port), "--cwd", tmp, "--timeout", "20",
                              "--model", "claude-sonnet-5-5"],
                             env=env, stdout=subprocess.DEVNULL, stderr=err)
    for _ in range(100):
        time.sleep(0.05)
        err.seek(0); m = re.search(r"listening on ([\d.]+):(\d+)", err.read())
        if m: return m.group(1)
        if relay.poll() is not None: break
    raise SystemExit("FAIL: the relay did not start: " + open(err.name).read())
def stop_relay():
    global relay
    if relay: relay.kill(); relay.wait(); relay = None

class Boot:
    def __init__(self, name, net):
        self.log, sock = "%s/%s.uart" % (tmp, name), "%s/%s.qmp" % (tmp, name)
        nic = ["-netdev", "user,id=n", "-device", "virtio-net-device,netdev=n"] if net else ["-nic", "none"]
        self.q = subprocess.Popen(["qemu-system-aarch64", "-machine", "virt", "-cpu", "cortex-a72", "-m", "256",
                                   "-global", "virtio-mmio.force-legacy=false", *nic, "-device", "ramfb",
                                   "-device", "virtio-keyboard-device", "-display", "none", "-serial", "file:" + self.log,
                                   "-qmp", "unix:%s,server,nowait" % sock, "-kernel", os.path.join(arch, "kernel8.elf")],
                                  stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        if not self.wait_for("M2 input ready", 30): raise SystemExit("FAIL: %s did not boot: %r" % (name, self.uart()[-600:]))
        time.sleep(0.5)
        self.s = socket.socket(socket.AF_UNIX); self.s.settimeout(20); self.s.connect(sock); self.f = self.s.makefile("rw")
        self.f.readline(); self.cmd("qmp_capabilities"); self.n = 0
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
    def terminal(self):   # F2 brings the Terminal to the front; a window switch is a whole redraw, so let it settle
        n = self.uart().count("terminal open")
        self.key("f2")
        if not self.wait_for("terminal open", 10, n + 1): raise SystemExit("FAIL: F2 did not open the Terminal: %r" % self.uart()[-300:])
        time.sleep(0.4)
    def type(self, text):
        for ch in text:
            if ch == " ": self.key("spc")
            elif ch == "?": self.key("shift", "slash")
            else: self.key(ch)
    def shot(self):
        self.n += 1; path = "%s.shot%d.ppm" % (self.log, self.n)
        self.cmd("screendump", filename=path); time.sleep(0.4)
        parts = open(path, "rb").read().split(b"\n", 3); w, h = map(int, parts[1].split())
        return w, h, parts[3]
    def close(self):
        self.q.kill(); self.q.wait()

def ink(shot_, x0, y0, x1, y1):   # dark pixels in a rectangle: the Console's text is near-black on white
    w, _, px = shot_
    return sum(1 for y in range(y0, y1) for x in range(x0, x1) if max(px[(y * w + x) * 3:(y * w + x) * 3 + 3]) < 0x90)

def densest_row(shot_, rect):   # the most inked pixel columns in any one text-row-high band of the rectangle
    w, h, px = shot_
    x0, y0, x1, y1 = rect
    dark = lambda x, y: max(px[(y * w + x) * 3:(y * w + x) * 3 + 3]) < 0x90
    k = 16 * h // 600
    # pixel columns with any ink inside the band: a run of # leaves almost no blank column, words leave gaps
    return max(sum(1 for x in range(x0, x1) if any(dark(x, y) for y in range(i, i + k))) for i in range(y0, y1 - k, 2))

def bands(shot_):   # the kernel's layout: an 800x600 design scaled by height; 19 rows of 16 under the title bar
    w, h, _ = shot_
    sc = lambda v: v * h // 600
    win_w, win_x, win_y = sc(500), (w - sc(500)) // 2, sc(100)
    x0, x1 = win_x + sc(8), win_x + win_w - sc(4)
    return (x0, win_y + sc(32), x1, win_y + sc(32 + 17 * 16)), (x0, win_y + sc(32 + 18 * 16), x1, win_y + sc(32 + 19 * 16))

def answer_after(out, marker_count):   # the lines printed after the n-th "claude: thinking", up to the next prompt line
    chunk = out.split("claude: thinking\n")[marker_count]
    return [l for l in PROMPT_RE.split(chunk)[0].split("\n") if l and not re.match(r"key \d+ (down|up)$", l)]

try:
    print("arm64 claude console: build with a throwaway token, relay on 127.0.0.1:%d" % port)
    out = build(True)
    check("the build says it has a token and never prints it", "token set" in out and TOKEN not in out, out[-300:])
    host = start_relay(TOKEN)
    check("the relay binds 127.0.0.1", host == "127.0.0.1", host)

    b = Boot("net", True)
    try:
        check("DHCP leased an address", "net dhcp 10.0.2.15 gw 10.0.2.2" in b.uart(), b.uart()[-300:])
        b.type("hi"); b.key("ret"); time.sleep(1.5)
        check("the Console takes no input: hi and Enter with it in front ask nobody",
              "claude: thinking" not in b.uart() and DEFAULT + "hi" not in b.uart() and not stub_calls(), b.uart()[-300:])
        b.terminal()
        a = b.shot(); body, prompt = bands(a)
        b.type(QUESTION + "x"); b.key("backspace"); time.sleep(0.4)
        t = b.shot()
        check("typed letters show on the prompt row", ink(t, *prompt) > ink(a, *prompt) + 60,
              "%d -> %d dark pixels" % (ink(a, *prompt), ink(t, *prompt)))
        check("key echoes stay on the UART", "key 17 down" in b.uart())
        b.key("ret")
        got = b.wait_for("claude: thinking", 10) and b.wait_for(MARK, 30)
        out = b.uart()
        calls = stub_calls()
        check("Enter sent the question: the stub got exactly the typed text, Backspace applied",
              len(calls) == 1 and calls[0]["stdin"] == QUESTION, repr([c["stdin"] for c in calls]))
        lines = answer_after(out, 1) if got else []
        check("the answer comes back word for word", " ".join(" ".join(lines).split()) == ANSWER + " " + MARK, repr(lines))
        check("it is wrapped to lines of at most 53 columns", len(lines) > 1 and all(len(l) <= 53 for l in lines),
              repr([len(l) for l in lines]))
        check("the question is echoed after the default prompt, Claude Haiku 5.5 $", DEFAULT + QUESTION + "\n" in out, out[-400:])
        check("... and the relay's model line is not printed as part of the answer", "M Claude" not in out)
        c = b.shot()
        dense_a, dense_c = densest_row(a, body), densest_row(c, body)
        print("    densest text row: %d inked columns at boot, %d with the answer" % (dense_a, dense_c))
        check("the answer is drawn in the Terminal window: its ##### line is the densest row on screen",
              dense_c > dense_a + 60, "%d vs %d" % (dense_c, dense_a))
        check("the prompt row is empty again after Enter", ink(c, *prompt) < ink(t, *prompt))
        n = b.uart().count("console open"); b.key("esc")
        check("Esc brings the Console back", b.wait_for("console open", 10, n + 1), b.uart()[-300:])
        time.sleep(0.4); d = b.shot()
        check("the answer is not in the Console: no ##### row there", densest_row(d, body) < dense_c - 60,
              "%d vs %d in the Terminal" % (densest_row(d, body), dense_c))
        b.terminal()
        # a follow-up resumes the relay's session
        b.type(SECOND); b.key("ret")
        b.wait_for("claude: thinking", 10, 2); b.wait_for(MARK, 30, 2)
        calls = stub_calls()
        sid = None
        if len(calls) == 2:
            sid = calls[1]["argv"][calls[1]["argv"].index("--resume") + 1] if "--resume" in calls[1]["argv"] else None
        check("the follow-up resumes the first answer's session", len(calls) == 2 and calls[1]["stdin"] == SECOND and sid,
              repr([c["argv"][-2:] for c in calls]))
        check("the follow-up is echoed after the model that answered: Claude Sonnet 5.5 $", SONNET + SECOND + "\n" in b.uart(),
              b.uart()[-400:])
        # the relay now wants another token
        start_relay(OTHER)
        b.type("hi"); b.key("ret")
        check("a wrong token prints claude: error -401", b.wait_for("claude: error -401", 20), b.uart()[-300:])
        check("the 401 never ran claude", len(stub_calls()) == 2)
        # the Terminal's own scrollback: five more refusals (3+ lines each) overflow its rows, then Page Up and End
        for k in range(2, 7): b.type("hi"); b.key("ret"); b.wait_for("claude: error -401", 20, k)
        time.sleep(0.4)
        region = lambda s, r: [s[2][(y * s[0] + r[0]) * 3:(y * s[0] + r[2]) * 3] for y in range(r[1], r[3])]
        e = b.shot(); b.key("pgup"); time.sleep(0.4); f = b.shot(); b.key("end"); time.sleep(0.4); g = b.shot()
        check("Page Up scrolls the Terminal back", region(e, body) != region(f, body))
        check("... the prompt row stays put while scrolled", region(e, prompt) == region(f, prompt))
        check("... and End brings back the newest picture, pixel for pixel", region(e, body) == region(g, body))
        out = b.uart()
        check("the token never appears on the UART", TOKEN not in out and OTHER not in out)
        long_lines = [l for l in out.split(DEFAULT + QUESTION)[-1].splitlines() if len(l) > 53]
        check("every line after the first question fits 53 columns", not long_lines, repr(long_lines))
    finally:
        b.close()

    n0 = len(stub_calls())
    b = Boot("nonet", False)
    try:
        b.terminal(); b.type("hi"); b.key("ret")
        check("with no network card the prompt is the default model", b.wait_for(DEFAULT + "hi\n", 10), b.uart()[-300:])
        check("with no network card: claude: no network", b.wait_for("claude: no network", 10), b.uart()[-300:])
        time.sleep(1)
        check("... and with no model baked in, no local-model line follows", "llm:" not in b.uart(), b.uart()[-300:])
    finally:
        b.close()
    check("... and the relay was never asked", len(stub_calls()) == n0)

    out = build(False)
    check("a build with no token file still links and says so", "no token" in out, out[-300:])
    start_relay(TOKEN)
    b = Boot("notoken", True)
    try:
        b.terminal(); b.type("hi"); b.key("ret")
        check("with no token the prompt is Claude alone", b.wait_for("\nClaude $ hi\n", 10), b.uart()[-300:])
        check("with no token: claude: no token", b.wait_for("claude: no token", 10), b.uart()[-300:])
    finally:
        b.close()
    check("... and the relay was never asked", len(stub_calls()) == n0)
finally:
    stop_relay()
    subprocess.run(["make", "-C", arch, "clean"], capture_output=True, timeout=120)
    shutil.rmtree(tmp, ignore_errors=True)
if fails:
    print("FAIL: %d of the Claude Terminal checks failed" % len(fails)); sys.exit(1)
print("PASS: the Console takes no input; typed at the ARM Terminal's prompt (Claude Haiku 5.5 $, then the model that answered), the question reaches the relay over virtio-net "
      "and the stub's answer is printed in the Terminal in 53-column lines, not the Console; a wrong token, no network and "
      "no token each print their own line")
