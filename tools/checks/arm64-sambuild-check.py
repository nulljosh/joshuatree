#!/usr/bin/env python3
"""Samantha's apps built on the Pi (6.94.0): with plankc and its runtime on the card, [[app NAME]] needs no Mac.

The same harness as arm64-samapp-check.py (a fake relay answers /api/claude from a script), but the card carries
JT/APPS/PLANKC.APP (tools/plankapp.sh --plankc) and JT/PLANKC/RT.PK, and the relay's /api/build answers 500 and counts.
  1. "sam make me an app called sun": her first answer has a mistake. The Pi writes JT/SUN.PK, runs plankc on it, which
     fails and leaves its reason in JT/APPS/SUN.ERR; those words go back to her as the next turn. Her fixed answer builds
     on the Pi ("app: built sun on the Pi, opened it") and the app runs to its line.
  2. The relay's /api/build was never called.
After QEMU stops the card holds JT/SUN.PK and JT/APPS/SUN.APP.
Discriminating: drop app_build_here's call and the build goes to the relay, which answers 500, so sun never opens.
arm64-samapp-check.py keeps a card with no plankc, where the relay still builds as before.
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

TOKEN = "arm-sambuild-check-token-0123456789a"
tmp = Path(scratch_dir("arm64-sambuild"))
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
            status, body, ctype = 500, b"the Mac is not building today", "text/plain"
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
    r = subprocess.run([str(root / "tools/plankapp.sh"), "--plankc", str(root / "user/plankc/plankc.pk"), str(tmp / "PLANKC.APP")], capture_output=True, text=True, timeout=900)
    if r.returncode: print((r.stdout + r.stderr)[-1500:]); print("FAIL: plankc does not build into PLANKC.APP"); sys.exit(1)
    mt("mmd", "::JT"); mt("mmd", "::JT/APPS"); mt("mmd", "::JT/PLANKC")
    mt("mcopy", "-o", tmp / "PLANKC.APP", "::JT/APPS/PLANKC.APP")
    mt("mcopy", "-o", root / "user/plankc/rt.pk", "::JT/PLANKC/RT.PK")
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
    wait_for("DONEWORD", 600, n); time.sleep(1.5)
    out = uart()[n:]
    check("her first program was built on the Pi and did not build", "app: building sun on the Pi" in out and "app: it did not build on the Pi" in out, out[-800:])
    check("plankc's words went back to her as the next turn", len(calls) >= 2 and "nope" in calls[1]["prompt"] and "app:" in calls[1]["prompt"], [c["prompt"][:300] for c in calls[1:2]])
    check("her fixed program built on the Pi and opened", "app: built sun on the Pi, opened it" in out, out[-800:])
    check("the app plankc built on the Pi ran", "sun: made by samantha" in out and "M3c app sun exit 0" in out, out[-800:])
    check("her third turn heard it was built and opened", len(calls) == 3 and "built and opened" in calls[2]["prompt"], [c["prompt"][:200] for c in calls[2:]])
    check("the Mac was never asked to build", builds == [], builds)
    check("the token never appears on the UART", TOKEN not in uart())
    check("no kernel crash", "KERNEL CRASH" not in uart(), uart()[-600:])
    q.kill(); q.wait(); q = None
    check("the card holds her code, JT/SUN.PK, and JT/APPS/SUN.APP", mt("mtype", "::JT/SUN.PK").returncode == 0 and mt("mtype", "::JT/APPS/SUN.APP").returncode == 0)
finally:
    if q: q.kill(); q.wait()
    srv.shutdown()
    subprocess.run(["make", "-C", str(arch), "clean"], capture_output=True, timeout=120)
if fails: print("FAIL: " + ", ".join(fails)); sys.exit(1)
print("PASS: Samantha's app is built on the Pi by plankc, with no Mac: its errors go back to her, the fixed one opens and runs")
