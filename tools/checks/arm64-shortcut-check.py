#!/usr/bin/env python3
"""Shortcuts on the Pi (11.4.0, arch/arm64/cmd.c shortcut_command, ask.c's shortcut plugin): a few Terminal commands under one
name, kept in JT/SHORTCUT.TXT. Headless QEMU virt with a card, a keyboard, a network card and a fake Claude relay.

  1. `shortcut` with none says how to make one. `shortcut loud = vol 30; vol` saves it, `shortcut loud` runs both
     commands, each shown first (`shortcut> vol 30`), and the volume is 30.
  2. A shortcut that runs a shortcut is refused, and so is a bad name.
  3. Samantha: asked for a shortcut called night, the relay answers `[[shortcut night = vol 10]]`: it is saved, and
     `shortcut night` sets the volume to 10. The first call told the model about the plugin.
  4. Asked without naming one, a reply of `[[shortcut evil = vol 100]]` is refused, not saved (the name was not typed).
  5. `shortcut rm night` drops it; the card's file holds only `loud = vol 30; vol`.

Discriminating: drop the named_in test in shortcut_act and step 4 saves evil.
Usage: tools/checks/arm64-shortcut-check.py   (from the repo root)
"""
import http.server, json, os, shutil, socket, subprocess, sys, threading, time
from pathlib import Path
root = Path(__file__).resolve().parents[2]
arch = root / "arch/arm64"
sys.path.insert(0, str(Path(__file__).resolve().parent))
from scratch import scratch_dir
from sdcard import PART, build_card
if not all(shutil.which(t) for t in ("clang", "ld.lld", "qemu-system-aarch64", "mmd", "mtype")):
    print("SKIP: clang, ld.lld, qemu-system-aarch64 or mtools not installed"); sys.exit(0)
tmp = Path(scratch_dir("arm64-shortcut"))
TOKEN = "arm-shortcut-check-token-0123456789ab"
token_file = tmp / "token"
token_file.write_text(TOKEN + "\n"); os.chmod(token_file, 0o600)
card = str(tmp / "card.img")
build_card(card, False)
MT = dict(os.environ, MTOOLS_SKIP_CHECK="1")
def mt(*a): return subprocess.run([a[0], "-i", f"{card}@@{PART * 512}", *a[1:]], capture_output=True, env=MT)
if mt("mmd", "::JT").returncode: print("FAIL: mtools could not make JT/ on the check card"); sys.exit(1)
fails = []
def check(name, ok, detail=""):
    print(("  ok: " if ok else "  FAIL: ") + name + ("" if ok else "  " + repr(detail)[:600]))
    if not ok: fails.append(name)
calls = []
class Relay(http.server.BaseHTTPRequestHandler):
    def do_POST(self):
        n = int(self.headers.get("Content-Length", "0")); d = json.loads(self.rfile.read(n)); calls.append(d)
        t = json.dumps(d)
        text = ("Saved it.\n[[shortcut night = vol 10]]" if "called night" in t else
                "Here you go.\n[[shortcut evil = vol 100]]" if "make one quick" in t else "All done. SHORTDONE")
        body = ("S 12345678-1234-1234-1234-123456789abc\n" + text).encode()
        self.send_response(200); self.send_header("Content-Type", "text/plain"); self.send_header("Content-Length", str(len(body)))
        self.send_header("Connection", "close"); self.end_headers(); self.wfile.write(body)
    def log_message(self, *a): pass
relay = http.server.HTTPServer(("127.0.0.1", 0), Relay)
threading.Thread(target=relay.serve_forever, daemon=True).start()
class Boot:
    def __init__(self, name, net=True):
        self.log, sock = str(tmp / (name + ".uart")), str(tmp / (name + ".qmp"))
        args = ["qemu-system-aarch64", "-machine", "virt", "-cpu", "cortex-a72", "-m", "256", "-global", "virtio-mmio.force-legacy=false",
                "-device", "ramfb", "-device", "virtio-keyboard-device", "-device", "virtio-tablet-device", "-display", "none",
                "-drive", "if=none,id=card,file=%s,format=raw" % card, "-device", "virtio-blk-device,drive=card",
                "-serial", "file:" + self.log, "-qmp", "unix:%s,server,nowait" % sock, "-kernel", str(arch / "kernel8.elf")]
        if net: args += ["-netdev", "user,id=n", "-device", "virtio-net-device,netdev=n"]
        self.q = subprocess.Popen(args, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        if not self.wait_for("M2 input ready", 40): raise SystemExit("FAIL: %s did not boot: %r" % (name, self.uart()[-600:]))
        time.sleep(0.5)
        self.s = socket.socket(socket.AF_UNIX); self.s.settimeout(30); self.s.connect(sock); self.f = self.s.makefile("rw"); self.f.readline()
        self.cmd("qmp_capabilities")
        if net and not self.wait_for("net dhcp 10.0.2.15", 30): raise SystemExit("FAIL: no DHCP lease: %r" % self.uart()[-300:])
    def uart(self): return open(self.log, errors="replace").read() if os.path.exists(self.log) else ""
    def wait_for(self, text, secs, count=1):
        end = time.time() + secs
        while time.time() < end:
            if self.uart().count(text) >= count: return True
            if self.q.poll() is not None: return False
            time.sleep(0.1)
        return False
    def cmd(self, c, **a):
        self.f.write(json.dumps({"execute": c, **({"arguments": a} if a else {})}) + "\n"); self.f.flush()
        while True:
            r = json.loads(self.f.readline())
            if "error" in r: raise SystemExit("FAIL: QMP " + repr(r))
            if "return" in r: return r["return"]
    def key(self, *names, hold=50):
        self.cmd("send-key", **{"keys": [{"type": "qcode", "data": k} for k in names], "hold-time": hold}); time.sleep(hold / 1000 + 0.1)
    def type(self, text):
        for ch in text: self.key({" ": "spc", "=": "equal", ";": "semicolon", "-": "minus", ".": "dot"}.get(ch, ch))
    def shot(self, name):
        path = tmp / (name + ".ppm"); self.cmd("screendump", filename=str(path))
        return Image.open(path).convert("RGB")
    def click(self, x, y):
        self.cmd("input-send-event", events=[{"type": "abs", "data": {"axis": "x", "value": int(x * 32767 / 800)}},
                                              {"type": "abs", "data": {"axis": "y", "value": int(y * 32767 / 600)}}]); time.sleep(0.2)
        for down in (True, False):
            self.cmd("input-send-event", events=[{"type": "btn", "data": {"button": "left", "down": down}}]); time.sleep(0.1)
    def close(self):
        try: self.f.close(); self.s.close()
        except OSError: pass
        if self.q.poll() is None: self.q.terminate()
        try: self.q.wait(timeout=5)
        except subprocess.TimeoutExpired: self.q.kill(); self.q.wait()


b = None
try:
    env = dict({k: v for k, v in os.environ.items() if not k.startswith("CLAUDE_RELAY")}, CLAUDE_RELAY_HOST="10.0.2.2",
               CLAUDE_RELAY_PORT=str(relay.server_address[1]), CLAUDE_RELAY_TOKEN_FILE=str(token_file))
    subprocess.run(["make", "-C", str(arch), "clean"], capture_output=True, timeout=60)
    r = subprocess.run(["make", "-C", str(arch), "kernel8.elf"], env=env, capture_output=True, text=True, timeout=600)
    if r.returncode: print(r.stdout[-2000:], r.stderr[-2000:]); print("FAIL: arch/arm64 does not build"); sys.exit(1)
    b = Boot("main")
    b.key("f2"); check("F2 opens the Terminal", b.wait_for("terminal open", 10), b.uart()[-300:]); time.sleep(0.4)
    def run(line, want, secs=15):
        n = b.uart().count(want); b.type(line); b.key("ret"); return b.wait_for(want, secs, n + 1)
    check("`shortcut` with none says how to make one", run("shortcut", "shortcut: none yet"), b.uart()[-300:])
    check("`shortcut loud = vol 30; vol` saves it", run("shortcut loud = vol 30; vol", "shortcut: saved loud"), b.uart()[-300:])
    n0 = len(b.uart())
    ok = run("shortcut loud", "shortcut> vol", 15) and b.wait_for("volume 30\nshortcut> vol\nvolume 30", 10)
    check("`shortcut loud` runs both, each shown first, and the volume is 30", ok, b.uart()[n0:][-400:])
    check("a shortcut that runs a shortcut is refused", run("shortcut loop = shortcut loud", "a shortcut cannot run another"), b.uart()[-300:])
    check("a bad name is refused", run("shortcut x.y = vol", "a name is 1 to 12"), b.uart()[-300:])
    check("Samantha writes the night shortcut when asked by name", run("sam make a shortcut called night that sets volume 10", "shortcut: saved night", 40), b.uart()[-600:])
    check("the first call told the model about the plugin", bool(calls) and "[[shortcut NAME = CMD; CMD]] save a shortcut" in json.dumps(calls[0]), json.dumps(calls[:1])[-400:])
    b.wait_for("SHORTDONE", 30)
    check("`shortcut night` sets the volume to 10", run("shortcut night", "volume 10"), b.uart()[-300:])
    n0 = len(b.uart())
    run("sam make one quick", "not saved", 40)
    check("a shortcut under a name nobody typed is refused, not saved", "evil was not in your question, not saved" in b.uart()[n0:] and "saved evil" not in b.uart()[n0:], b.uart()[n0:][-500:])
    b.wait_for("SHORTDONE", 30, 2)
    check("`shortcut rm night` drops it", run("shortcut rm night", "shortcut: dropped night"), b.uart()[-300:])
    check("no fault, no crash", "KERNEL CRASH" not in b.uart() and "oom" not in b.uart(), b.uart()[-300:])
except SystemExit as e:
    if str(e): check("boot", False, str(e))
finally:
    if b: b.close()
    relay.shutdown()
    subprocess.run(["make", "-C", str(arch), "clean"], capture_output=True)
text = mt("mtype", "::JT/SHORTCUT.TXT").stdout.decode(errors="replace")
check("the card's JT/SHORTCUT.TXT holds only loud", text == "loud = vol 30; vol\n", text)
if fails: print("FAIL: %d shortcut checks" % len(fails)); sys.exit(1)
print("PASS: shortcuts save, list, run each command shown first, refuse loops and bad names; Samantha writes one only under a name the person typed; rm drops it")
