#!/usr/bin/env python3
"""The ARM Terminal is a zsh style shell, and Samantha is a command that never blocks it (docs/GUIDE.md, `sam`).

The host side is the real tools/claude-relay/relay.py on 127.0.0.1 with a throwaway token and a stub `claude` that waits
STUB_DELAY seconds before it answers (the same approach as arm64-claude-console-check.py). The kernel is built with
CLAUDE_RELAY_TOKEN_FILE pointing at the throwaway token; QEMU's user network reaches the host at 10.0.2.2. Keys go in
through QMP send-key on a virtio keyboard; everything is read from the UART. Headless: no window, no browser.

One boot, virtio-net, virtio keyboard, Terminal open (F2):
  1. the prompt is `joshua@tree % ` and an unknown word prints `zsh: command not found: NAME`; the stub is never asked.
  2. `sam` alone prints `usage: sam QUESTION`; the stub is never asked.
  3. `sam QUESTION` returns to the prompt at once while the stub holds its answer back: the relay has the question and the
     answer has not printed. A command typed meanwhile (`vol`) runs and prints before the answer; a second `sam` says
     `sam: still thinking`; `browse` says sam is using the network; a half-typed line (`vo`) survives the answer.
  4. then the delayed answer prints as `sam: ...` above the prompt, the half-typed line completes (`l` and Enter) and runs,
     and the stub was asked exactly once.
Discriminating: with `sam` blocking the keyboard (ask.c running the exchange without net_idle), the `vol` line prints only
after the answer and step 3 fails.
Skips (exit 0) when clang's aarch64 target, ld.lld or qemu-system-aarch64 is missing.
Usage: tools/checks/arm64-zsh-check.py   (from the repo root)
"""
import json, os, re, shutil, socket, subprocess, sys, tempfile, time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from freeport import free_port

root = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
arch = os.path.join(root, "arch/arm64")
relay_py = os.path.join(root, "tools/claude-relay/relay.py")
if not all(shutil.which(t) for t in ("clang", "ld.lld", "qemu-system-aarch64")):
    print("SKIP: clang, ld.lld or qemu-system-aarch64 not installed"); sys.exit(0)

TOKEN = "arm-zsh-check-token-0123456789"
PROMPT = "joshua@tree % "
ANSWER = "Delayed but here, and the shell stayed up"
DELAY = 8
STUB = r'''#!/usr/bin/env python3
import json, os, sys, time, uuid
prompt = sys.stdin.read()
with open(os.environ["STUB_LOG"], "a") as f: f.write(json.dumps({"stdin": prompt}) + "\n")
if os.path.exists(os.environ["STUB_FAIL"]):   # the relay's error path: a claude that dies with no answer
    sys.stderr.write("stub claude failed on purpose\\n"); sys.exit(3)
time.sleep(float(os.environ["STUB_DELAY"]))
print(json.dumps({"type": "result", "subtype": "success", "is_error": False, "result": os.environ["STUB_ANSWER"], "session_id": str(uuid.uuid4())}))
'''
fails = []
def check(name, ok, detail=""):
    print(("  ok: " if ok else "  FAIL: ") + name + (("  (" + detail + ")") if detail and not ok else ""))
    if not ok: fails.append(name)

tmp = tempfile.mkdtemp(prefix="jt-arm-zsh-")
os.makedirs(tmp + "/bin")
with open(tmp + "/bin/claude", "w") as f: f.write(STUB)
os.chmod(tmp + "/bin/claude", 0o755)
token_file = tmp + "/token"
with open(token_file, "w") as f: f.write(TOKEN + "\n")
os.chmod(token_file, 0o600)
stub_log = tmp + "/stub.log"
fail_flag = tmp + "/fail"
port = free_port()
base_env = {k: v for k, v in os.environ.items() if not k.startswith("CLAUDE_RELAY")}

def stub_calls():
    try: return [json.loads(l) for l in open(stub_log)]
    except FileNotFoundError: return []

relay = None
class Boot:
    def __init__(self):
        self.log, sock = tmp + "/boot.uart", tmp + "/boot.qmp"
        self.q = subprocess.Popen(["qemu-system-aarch64", "-machine", "virt", "-cpu", "cortex-a72", "-m", "256",
                                   "-global", "virtio-mmio.force-legacy=false", "-netdev", "user,id=n", "-device", "virtio-net-device,netdev=n",
                                   "-device", "ramfb", "-device", "virtio-keyboard-device", "-display", "none", "-serial", "file:" + self.log,
                                   "-qmp", "unix:%s,server,nowait" % sock, "-kernel", os.path.join(arch, "kernel8.elf")],
                                  stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        if not self.wait_for("M2 input ready", 30): raise SystemExit("FAIL: did not boot: %r" % self.uart()[-600:])
        time.sleep(0.5)
        self.s = socket.socket(socket.AF_UNIX); self.s.settimeout(20); self.s.connect(sock); self.f = self.s.makefile("rw")
        self.f.readline(); self.cmd("qmp_capabilities")
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
    def type(self, text):
        for ch in text:
            self.key("spc" if ch == " " else ch)
    def run(self, line):
        self.type(line); self.key("ret")
    def close(self):
        self.q.kill(); self.q.wait()

try:
    print("arm64 zsh shell: build with a throwaway token, relay on 127.0.0.1:%d, stub answers after %ds" % (port, DELAY))
    env = dict(base_env, CLAUDE_RELAY_HOST="10.0.2.2", CLAUDE_RELAY_PORT=str(port), CLAUDE_RELAY_TOKEN_FILE=token_file, CLAUDE_RELAY_TLS="0")
    r = subprocess.run(["make", "-C", arch, "TLS_TA="], env=env, capture_output=True, text=True, timeout=300)
    if r.returncode: print("FAIL: arch/arm64 does not build:\n" + r.stdout[-2000:] + r.stderr[-2000:]); sys.exit(1)
    renv = dict(base_env, PATH=tmp + "/bin" + os.pathsep + os.environ["PATH"], STUB_LOG=stub_log, STUB_FAIL=fail_flag, STUB_ANSWER=ANSWER,
                STUB_DELAY=str(DELAY), CLAUDE_RELAY_TOKEN=TOKEN)
    err = open(tmp + "/relay.log", "w+")
    relay = subprocess.Popen([sys.executable, relay_py, "--port", str(port), "--cwd", tmp, "--timeout", "30",
                              "--model", "claude-sonnet-5-5"], env=renv, stdout=subprocess.DEVNULL, stderr=err)
    for _ in range(100):
        time.sleep(0.05); err.seek(0)
        if re.search(r"listening on ([\d.]+):(\d+)", err.read()): break
    else: raise SystemExit("FAIL: the relay did not start")

    b = Boot()
    try:
        check("DHCP leased an address", "net dhcp 10.0.2.15 gw 10.0.2.2" in b.uart(), b.uart()[-300:])
        n = b.uart().count("terminal open"); b.key("f2")
        if not b.wait_for("terminal open", 10, n + 1): raise SystemExit("FAIL: F2 did not open the Terminal: %r" % b.uart()[-300:])
        time.sleep(0.4)
        b.run("frobnicate now")
        check("an unknown word prints zsh: command not found: NAME", b.wait_for("zsh: command not found: frobnicate\n", 10), b.uart()[-300:])
        check("... echoed after the zsh prompt", PROMPT + "frobnicate now\n" in b.uart(), b.uart()[-300:])
        b.run("sam")
        check("sam alone prints a usage line", b.wait_for("usage: sam QUESTION\n", 10), b.uart()[-300:])
        time.sleep(0.5)
        check("... and nothing so far went to Samantha", not stub_calls() and "samantha: thinking" not in b.uart())

        b.run("sam what is up")
        check("sam QUESTION reaches the relay", b.wait_for("samantha: thinking", 10), b.uart()[-300:])
        t0 = time.time()
        b.run("vol")
        got = b.wait_for("volume ", 4)
        check("a command typed while she thinks runs at once", got and time.time() - t0 < DELAY - 2 and ANSWER not in b.uart(),
              "%.1fs, %r" % (time.time() - t0, b.uart()[-300:]))
        b.run("sam second")
        check("a second sam says still thinking", b.wait_for("sam: still thinking\n", 4) and ANSWER not in b.uart(), b.uart()[-300:])
        b.run("browse example")
        check("a network command says sam is using the network", b.wait_for("sam is using the network", 4), b.uart()[-300:])
        b.type("vo")
        check("the stub has the one question and has not answered", len(stub_calls()) == 1 and stub_calls()[0]["stdin"] == "what is up"
              and ANSWER not in b.uart(), repr(stub_calls()))
        check("the delayed answer lands as sam: ...", b.wait_for("sam: " + ANSWER, DELAY + 15), b.uart()[-300:])
        out = b.uart()
        check("... after the vol line and the still-thinking line, not before", out.index("volume ") < out.index("sam: " + ANSWER)
              and out.index("sam: still thinking") < out.index("sam: " + ANSWER))
        b.key("l"); b.key("ret")
        check("the half-typed line survived the answer and runs: vol", b.wait_for("volume ", 5, 2), b.uart()[-300:])
        check("the stub was asked exactly once", len(stub_calls()) == 1, repr(stub_calls()))
        b.run("sam again")
        check("a new sam works once she has answered", b.wait_for("samantha: thinking", 10, 2), b.uart()[-300:])
        b.run("reboot")
        check("reboot while she is thinking is refused", b.wait_for("Wait for Samantha to finish, then try again.\n", 5)
              and "power: restart" not in b.uart(), b.uart()[-300:])
        check("... and the refused reboot does not wedge her answer", b.wait_for("sam: " + ANSWER, DELAY + 15, 2), b.uart()[-300:])
        # The bug found on the Pi: after any finished question reboot answered "Wait for Samantha" forever, because the typed
        # line that holds the command counted as a question running. Every end of a question must leave reboot working.
        open(fail_flag, "w").close()
        b.run("sam this one fails")
        check("a relay error ends the question with an error line", b.wait_for("samantha: error", 20), b.uart()[-400:])
        time.sleep(1.0)
        n = b.uart().count("Wait for Samantha")
        b.run("reboot")
        check("reboot runs after an error reply (not refused)", b.wait_for("power: restart", 10) and b.uart().count("Wait for Samantha") == n, b.uart()[-400:])
        b.run("shutdown")
        check("shutdown runs too", b.wait_for("power: shutdown", 10) and b.uart().count("Wait for Samantha") == n, b.uart()[-400:])
        os.remove(fail_flag)
        b.run("sam after the error")
        check("a second sam works after the error", b.wait_for("sam: " + ANSWER, DELAY + 15, 3), b.uart()[-400:])
        check("the token never appears on the UART", TOKEN not in b.uart())
        b.run("reboot")
        check("reboot is a command: it restarts the board", b.wait_for("power: restart", 10, 2), b.uart()[-300:])
    finally:
        b.close()
finally:
    if relay: relay.kill(); relay.wait()
    subprocess.run(["make", "-C", arch, "clean"], capture_output=True, timeout=120)
    shutil.rmtree(tmp, ignore_errors=True)
if fails:
    print("FAIL: %d of the zsh shell checks failed" % len(fails)); sys.exit(1)
print("PASS: the Terminal prompt is joshua@tree %, an unknown word is command not found, and sam QUESTION returns to the prompt at once: "
      "a command typed while she thinks runs, a second sam says still thinking, her delayed answer prints as sam: above the prompt, and reboot restarts the board")
