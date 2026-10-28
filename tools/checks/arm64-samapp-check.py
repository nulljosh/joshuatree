#!/usr/bin/env python3
"""Samantha writes an app (6.59.0): [[app NAME]] builds her Plank program on the Mac and opens it on the Pi.

Boots the virt kernel with a throwaway relay token, a card and no accounts, against a fake relay on the host whose
/api/claude answers from a script and whose /api/build is relay.py's own build_app (tools/plankapp.sh, real Plank).
  1. "sam make me an app called sun": her first answer holds Plank with a mistake and [[app sun]]. The build fails, and
     the compiler's words ("unknown variable") go back to her as the next turn. Her second answer is fixed; the Pi
     fetches the image, writes JT/APPS/SUN.APP unsigned, opens it ("app: built sun, opened it") and it runs to its line.
     Her third turn hears "built and opened".
  2. "sam hello there" with an answer that still says [[app moon]]: refused, she was not asked for an app; nothing is
     built.
After QEMU stops the card holds JT/APPS/SUN.APP and no SUN.SIG.
Discriminating: make app_build skip the relay (no build call) and step 1 never opens sun; drop the "app" test from
app_build and step 2 builds moon.
Skips (exit 0) without clang's aarch64 target, ld.lld, qemu-system-aarch64, mtools, uv or Plank (PLANK=, as plankapp.sh).
"""
import http.server, json, os, shutil, socket, subprocess, sys, threading, time, urllib.parse
from pathlib import Path
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from freeport import free_port
from scratch import scratch_dir
from sdcard import PART, build_card

root = Path(__file__).resolve().parents[2]
arch = root / "arch/arm64"
plank = Path(os.environ.get("PLANK", Path.home() / "Documents/Code/plank/plank.py"))
if not all(shutil.which(t) for t in ("clang", "ld.lld", "qemu-system-aarch64", "mmd", "mtype", "uv")) or not plank.exists():
    print("SKIP: clang, ld.lld, qemu-system-aarch64, mtools, uv or Plank (%s) missing" % plank); sys.exit(0)
sys.path.insert(0, str(root / "tools/claude-relay"))
import relay as real_relay

TOKEN = "arm-samapp-check-token-0123456789abc"
tmp = Path(scratch_dir("arm64-samapp"))
for p in tmp.iterdir(): p.unlink() if p.is_file() else shutil.rmtree(p)
token_file = tmp / "token"; token_file.write_text(TOKEN + "\n"); os.chmod(token_file, 0o600)
fails = []
def check(name, ok, detail=""):
    print(("  ok: " if ok else "  FAIL: ") + name + ("" if ok else "  " + repr(detail)[-700:]), flush=True)
    if not ok: fails.append(name)

BROKEN = "fn main() {\n  print(nope)\n}\n"
GOOD = 'fn main() {\n  print("sun: made by samantha")\n}\n'
calls, builds, script = [], [], []
class Relay(http.server.BaseHTTPRequestHandler):
    def do_POST(self):
        n = int(self.headers.get("Content-Length", "0")); raw = self.rfile.read(n)
        if self.headers.get("Authorization") != "Bearer " + TOKEN:
            status, body, ctype = 401, b"Wrong or missing relay token.", "text/plain"
        elif self.path.startswith("/api/build?"):
            name = urllib.parse.parse_qs(self.path.split("?", 1)[1]).get("name", [""])[0]
            src = json.loads(raw).get("source", "")
            builds.append((name, src))
            status, out = real_relay.build_app(name, src, str(plank))
            body, ctype = (out, "application/octet-stream") if status == 200 else (out.encode(), "text/plain")
        else:
            d = json.loads(raw); calls.append(d)
            status, body, ctype = 200, ("S 12345678-1234-1234-1234-123456789abc\n" + script[min(len(calls), len(script)) - 1]).encode(), "text/plain"
        self.send_response(status); self.send_header("Content-Type", ctype); self.send_header("Content-Length", str(len(body)))
        self.send_header("Connection", "close"); self.end_headers(); self.wfile.write(body)
    def log_message(self, *a): pass
srv = http.server.ThreadingHTTPServer(("127.0.0.1", free_port()), Relay)
port = srv.server_address[1]
threading.Thread(target=srv.serve_forever, daemon=True).start()

subprocess.run(["make", "-C", str(arch), "clean"], capture_output=True, timeout=60)
q = None
try:
    env = dict({k: v for k, v in os.environ.items() if not k.startswith("CLAUDE_RELAY")}, CLAUDE_RELAY_HOST="10.0.2.2",
               CLAUDE_RELAY_PORT=str(port), CLAUDE_RELAY_TOKEN_FILE=str(token_file))
    r = subprocess.run(["make", "-C", str(arch)], env=env, capture_output=True, text=True, timeout=600)
    if r.returncode: print(r.stderr[-1500:]); print("FAIL: arch/arm64 does not build"); sys.exit(1)
    card = str(tmp / "card.img"); build_card(card, False)
    MT = dict(os.environ, MTOOLS_SKIP_CHECK="1")
    def mt(*a): return subprocess.run([a[0], "-i", f"{card}@@{PART * 512}", *map(str, a[1:])], capture_output=True, env=MT)
    log, sock = tmp / "uart", tmp / "qmp"
    q = subprocess.Popen(["qemu-system-aarch64", "-machine", "virt", "-cpu", "cortex-a72", "-m", "256", "-global", "virtio-mmio.force-legacy=false",
                          "-netdev", "user,id=n", "-device", "virtio-net-device,netdev=n", "-device", "ramfb", "-device", "virtio-keyboard-device",
                          "-drive", "if=none,id=card,file=%s,format=raw" % card, "-device", "virtio-blk-device,drive=card",
                          "-display", "none", "-serial", "file:%s" % log, "-qmp", "unix:%s,server,nowait" % sock, "-kernel", str(arch / "kernel8.elf")],
                         stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    def uart(): return log.read_text(errors="replace") if log.exists() else ""
    def wait_for(text, secs, after=0):
        end = time.time() + secs
        while time.time() < end:
            if text in uart()[after:]: return True
            if q.poll() is not None: return False
            time.sleep(0.1)
        return False
    if not wait_for("M2 input ready", 60): raise SystemExit("FAIL: did not boot: %r" % uart()[-600:])
    time.sleep(0.5)
    s = socket.socket(socket.AF_UNIX); s.settimeout(30); s.connect(str(sock)); f = s.makefile("rw"); f.readline()
    def command(name, **a):
        f.write(json.dumps({"execute": name, "arguments": a}) + "\n"); f.flush()
        while True:
            rr = json.loads(f.readline())
            if "return" in rr or "error" in rr: return rr
    command("qmp_capabilities")
    def key(*k): command("send-key", keys=[{"type": "qcode", "data": x} for x in k]); time.sleep(0.12)
    def ask(text):
        for ch in text: key("spc" if ch == " " else ch)
        key("ret")
    check("DHCP leased an address", wait_for("net dhcp 10.0.2.15", 30), uart()[-300:])
    key("f2")
    if not wait_for("terminal open", 10): raise SystemExit("FAIL: F2 did not open the Terminal")
    time.sleep(0.5)

    script[:] = ["Here is your app.\n```plank\n" + BROKEN + "```\n[[app sun]]",
                 "Fixed it.\n```plank\n" + GOOD + "```\n[[app sun]]",
                 "It is open. DONEWORD"]
    n = len(uart()); ask("sam make me an app called sun")
    wait_for("DONEWORD", 300, n); time.sleep(1.5)
    out = uart()[n:]
    check("her first program did not build, and the Pi said so", len(builds) >= 1 and builds[0] == ("sun", BROKEN) and "app: it did not build (422)" in out, out[-800:])
    check("the compiler's words went back to her as the next turn", len(calls) >= 2 and "unknown variable" in calls[1]["prompt"], [c["prompt"][:300] for c in calls[1:2]])
    check("her fixed program built, was written and opened", len(builds) == 2 and "app: built sun, opened it" in out, out[-800:])
    check("the app she wrote ran on the Pi", "sun: made by samantha" in out and "M3c app sun exit 0" in out and "not signed, an admin's" in out, out[-800:])
    check("her third turn heard it was built and opened", len(calls) == 3 and "built and opened" in calls[2]["prompt"], [c["prompt"][:200] for c in calls[2:]])

    del calls[:]; nb = len(builds)
    script[:] = ["Sure.\n```plank\n" + GOOD + "```\n[[app moon]]", "OK. SECONDWORD"]
    n = len(uart()); ask("sam hello there")
    wait_for("SECONDWORD", 120, n); time.sleep(1)
    out = uart()[n:]
    check("an app nobody asked for is refused and nothing is built", "app: you did not ask for an app, not built" in out and len(builds) == nb, out[-500:])
    check("the token never appears on the UART", TOKEN not in uart())
    check("no kernel crash", "KERNEL CRASH" not in uart(), uart()[-600:])
    q.kill(); q.wait(); q = None
    check("the card holds JT/APPS/SUN.APP, unsigned", mt("mtype", "::JT/APPS/SUN.APP").returncode == 0 and mt("mtype", "::JT/APPS/SUN.SIG").returncode != 0)
finally:
    if q: q.kill(); q.wait()
    srv.shutdown()
    subprocess.run(["make", "-C", str(arch), "clean"], capture_output=True, timeout=120)
if fails: print("FAIL: " + ", ".join(fails)); sys.exit(1)
print("PASS: Samantha writes an app in Plank, reads the compiler's errors, fixes them, and the Pi builds, installs and opens it")
