#!/usr/bin/env python3
"""Headless proof that the Claude app (user/claude.c, 2.14.0) asks Claude Code through the relay and
shows the answer, and fails with a clear line when it cannot.

The host side is the real tools/claude-relay/relay.py with a stub `claude` first on PATH (records
its argv and stdin, answers in Claude Code's JSON shape; no model, no network). QEMU's user network
reaches the relay on the host's loopback at 10.0.2.2. The kernel gets the relay and its token from
the boot line (claudehost=, claudeport=, claudetoken=), as Settings would hold them; the app never
sees either.

Boot 1, `open=claude` with the relay set:
  1. ask: types a question with QMP send-key letters, Enter; "claude: reply" on serial, the stub got
     exactly the typed text on stdin, and the transcript gains real ink plus the terracotta "Claude"
     label. A reply from the relay at all proves the kernel added the right bearer: the relay answers
     401 without it.
  2. resume: a second question reaches the stub with --resume <the session the first reply named>.
  3. wrong token: the relay restarts with another token; the next question ends "claude: error -401"
     and the transcript shows the red error line.
  4. relay down: the relay is stopped; the next question ends in a bounded time with "claude: error"
     and another red line, and the app still takes keys and closes on Esc.
Boot 2, `open=claude` with no relay set (the browser demo's case): a question ends at once with
  "claude: error -13" (the kernel's -EACCES, the network never touched) and the red line.
Source: the token never appears in ring-3 headers or the app; the kernel adds it only for
  /api/claude under JT_POST_CLAUDE; the bearer is disarmed after every POST.

Discriminating: drop `http_post_set_bearer(claude_token_get())` from kernel/syscall.c and step 1
fails with "claude: error -401"; drop the -EACCES early return and boot 2 waits on a relay that is
not there and misses its deadline.

Usage: python3 tools/checks/ring3claude-check.py   (from the repo root, after make kernel.elf)
"""
import json, os, re, shutil, subprocess, sys, tempfile, time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from jtvm import VM, ROOT, SCALE
from freeport import free_port

TOKEN = "jt-check-token-0123456789abcdef"
OTHER = "jt-other-token-fedcba9876543210"
WX, WY, WW, WH = 78, 72, 805, 345          # the dock-launch window content, logical px
TOP, BOTTOM = WY + 46, WY + 270            # the transcript band
ACCENT, ERR, BODY = (0xB5, 0x50, 0x2C), (0xA3, 0x3B, 0x3B), (0xF5, 0xF0, 0xEB)
QUESTION = "what is in version"
SECOND = "and again"
fails = []

STUB = r'''#!/usr/bin/env python3
import json, os, sys, uuid
prompt = sys.stdin.read()
with open(os.environ["STUB_LOG"], "a") as f: f.write(json.dumps({"argv": sys.argv[1:], "stdin": prompt}) + "\n")
sid = sys.argv[sys.argv.index("--resume") + 1] if "--resume" in sys.argv else str(uuid.uuid4())
text = "The VERSION file says 2.14.0. It is the version the kernel prints at boot, and the landing page shows it too."
print(json.dumps({"type": "result", "subtype": "success", "is_error": False, "result": text, "session_id": sid}))
'''


def check(name, ok, detail=""):
    print(("ok   " if ok else "FAIL ") + name + (("  (" + detail + ")") if detail and not ok else ""))
    if not ok: fails.append(name)


def near(p, c, tol=40):
    return all(abs(p[i] - c[i]) <= tol for i in range(3))


def count(img, pred, y0=TOP, y1=BOTTOM):
    n = 0
    for y in range(y0 * SCALE, y1 * SCALE, 2):
        for x in range((WX + 16) * SCALE, (WX + WW - 16) * SCALE, 2):
            if pred(img.getpixel((x, y))): n += 1
    return n


def painted(vm, secs=30):
    """The first frame where the whole transcript band is the window body (no wallpaper showing through)."""
    end = time.time() + secs
    while time.time() < end:
        img = vm.frame()
        if all(near(img.getpixel((x * SCALE, y * SCALE)), BODY, 6) for x in (WX + 20, WX + WW // 2, WX + WW - 20) for y in (BOTTOM - 4, BOTTOM - 30)):
            return img
        time.sleep(0.3)
    return None


def ink(img): return count(img, lambda p: p[0] < 90 and p[1] < 90 and p[2] < 90)
def accent(img): return count(img, lambda p: near(p, ACCENT, 12))
def red(img): return count(img, lambda p: near(p, ERR, 12))


class Relay:
    def __init__(self, tmp, port, token):
        env = dict(os.environ, PATH=os.path.join(tmp, "bin") + os.pathsep + os.environ["PATH"],
                   STUB_LOG=os.path.join(tmp, "stub.log"), CLAUDE_RELAY_TOKEN=token)
        self.log = open(os.path.join(tmp, "relay-%d.log" % time.time_ns()), "w+")
        self.p = subprocess.Popen([sys.executable, str(ROOT / "tools/claude-relay/relay.py"), "--port", str(port),
                                   "--cwd", tmp, "--timeout", "20"], env=env, stdout=subprocess.DEVNULL, stderr=self.log)
        for _ in range(100):
            time.sleep(0.05)
            self.log.seek(0)
            if "listening on" in self.log.read(): return
        raise SystemExit("FAIL: relay did not start")

    def stop(self):
        self.p.kill(); self.p.wait()


def stub_calls(tmp):
    try: return [json.loads(l) for l in open(os.path.join(tmp, "stub.log"))]
    except FileNotFoundError: return []


def ask(vm, text, n_before, marker_re, secs):
    """Types text, presses Enter, waits for the (n_before+1)th reply-or-error marker. Returns the marker line."""
    vm.type(text, gap=0.08)
    time.sleep(0.3)
    vm.key("ret")
    end = time.time() + secs
    while time.time() < end:
        lines = re.findall(r"claude: (?:reply|error) -?\d+", vm.serial())
        if len(lines) > n_before: return lines[n_before]
        time.sleep(0.2)
    return None


def source_checks():
    rd = lambda p: (ROOT / p).read_text()
    sc, app, jts = rd("kernel/syscall.c"), rd("user/claude.c"), rd("user/jtsys.h")
    check("kernel adds the Claude bearer itself", "http_post_set_bearer(claude_token_get())" in sc)
    check("only for /api/claude under JT_POST_CLAUDE", "path_is_claude" in sc and '"/api/claude"' in sc and "claude && !path_is_claude(kpath)" in sc)
    check("no relay set up is -EACCES before the network", "return -EACCES" in sc)
    check("bearer disarmed after every POST", sc.count("http_post_set_bearer(0)") >= 2)
    check("no ring-3 header or app holds the token", "claude_token" not in jts and "claude_token" not in app and "Bearer" not in app)
    check("the app posts with JT_POST_CLAUDE", "JT_POST_CLAUDE" in app and '"/api/claude"' in app)


def main():
    source_checks()
    tmp = tempfile.mkdtemp(prefix="jt-claude-app-")
    os.makedirs(os.path.join(tmp, "bin"))
    with open(os.path.join(tmp, "bin", "claude"), "w") as f: f.write(STUB)
    os.chmod(os.path.join(tmp, "bin", "claude"), 0o755)
    port = free_port()
    relay = Relay(tmp, port, TOKEN)
    net = ("-net", "nic,model=rtl8139", "-net", "user")
    vm = None
    try:
        vm = VM(None, f"open=claude claudehost=10.0.2.2 claudeport={port} claudetoken={TOKEN}",
                "claude: ring-3 window", extra=net, work=tmp)
        check("token never echoed to serial", TOKEN not in vm.serial())
        vm.move(480, 500)                      # park the pointer on the dock, off the transcript
        before = painted(vm)
        check("the window body is painted", before is not None)
        if before is None: return 1
        ink0, acc0 = ink(before), accent(before)

        # 1. ask
        m = ask(vm, QUESTION, 0, None, 90)
        check("first question answered (claude: reply)", bool(m) and "reply" in m, str(m))
        time.sleep(1.0)
        after = vm.frame()
        calls = stub_calls(tmp)
        check("the stub got exactly the typed question", bool(calls) and calls[0]["stdin"] == QUESTION, repr(calls[0]["stdin"]) if calls else "no call")
        check("read-only tools reached claude", bool(calls) and "--tools" in calls[0]["argv"] and "Bash" not in calls[0]["argv"])
        dink = ink(after) - ink0
        check("the answer is drawn (transcript ink grew)", dink > 1000, "+%d" % dink)
        check("the terracotta Claude label is drawn", accent(after) - acc0 > 40, "+%d" % (accent(after) - acc0))
        print("     ink +%d, accent +%d" % (dink, accent(after) - acc0))

        # 2. resume
        m = ask(vm, SECOND, 1, None, 90)
        calls = stub_calls(tmp)
        check("second question answered", bool(m) and "reply" in m, str(m))
        first_sid = None
        if len(calls) >= 2:
            check("first question started a fresh session", "--resume" not in calls[0]["argv"])
            first_sid = calls[1]["argv"][-1] if "--resume" in calls[1]["argv"] else None
            check("second question resumed a session", first_sid is not None, " ".join(calls[1]["argv"][-2:]))
            check("second question carried the typed text", calls[1]["stdin"] == SECOND, repr(calls[1]["stdin"]))
        else:
            check("the stub saw two questions", False, str(len(calls)))

        # 3. wrong token
        relay.stop()
        relay = Relay(tmp, port, OTHER)
        red0 = red(vm.frame())
        m = ask(vm, "hi", 2, None, 90)
        check("wrong token ends in claude: error -401", m == "claude: error -401", str(m))
        time.sleep(1.0)
        check("the red error line is drawn", red(vm.frame()) - red0 > 40, "+%d" % (red(vm.frame()) - red0))
        check("a refused request never reached claude", len(stub_calls(tmp)) == 2, str(len(stub_calls(tmp))))

        # 4. relay down
        relay.stop(); relay = None
        t0 = time.time()
        m = ask(vm, "hi", 3, None, 150)
        took = time.time() - t0
        check("relay down ends in claude: error", bool(m) and "error" in m, str(m))
        print("     relay down: %s after %.1fs" % (m, took))
        vm.key("esc")
        check("Esc closes the app afterwards", vm.wait("claude: closed", 20))
        vm.quit(); vm = None

        # Boot 2: nothing set up, the browser demo's case
        vm = VM(None, "open=claude", "claude: ring-3 window", extra=net, work=tmp)
        time.sleep(1.0)
        red0 = red(vm.frame())
        t0 = time.time()
        m = ask(vm, "hi", 0, None, 20)
        took = time.time() - t0
        check("no relay set: claude: error -13 at once", m == "claude: error -13" and took < 12, "%s after %.1fs" % (m, took))
        time.sleep(1.0)
        check("no relay set: the red line explains it", red(vm.frame()) - red0 > 40)
    finally:
        if vm: vm.quit()
        if relay: relay.stop()
        shutil.rmtree(tmp, ignore_errors=True)
    print("FAILED: " + ", ".join(fails) if fails else "ring3claude-check: all ok")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
