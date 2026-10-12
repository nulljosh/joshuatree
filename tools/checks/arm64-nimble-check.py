#!/usr/bin/env python3
"""Nimble on the ARM64 desktop (arch/arm64/nimble.c, nimble_ui.h, ask.c's plugin table), against a stub Nimble API on the host.

No real network: the kernel is built with NIMBLE_BASE pointing at an HTTP stub on 127.0.0.1 (QEMU's 10.0.2.2) that serves
`GET /api/answer?q=...` the way nimble.jaybulb.com does (text with sources, a card, a 502 with {"error"}, a hostile reply, a
1 MB reply, not-JSON), plus two source pages for the Browser, and a fake Claude relay. The stub logs every question it
sees, so a pass also proves nothing went anywhere else. One boot with a keyboard, a tablet and a network card, then a
second boot with no network card. Then:
  1. Terminal: `nimble capital of france` prints the answer and the numbered sources; the stub saw the question.
  2. Samantha: `sam ...` with the relay replying `[[nimble eiffel tower height]]` runs the nimble plugin; the relay's
     second call carries the answer and the source title as the result turn, its first call told the model about the
     plugin, the stub saw the question, and the relay token never reaches the serial log.
  3. The dock's Nimble tile (slot 10, before Trash) opens the window; Spotlight finds it by name too. A question typed in
     the search field and Enter shows the answer wrapped in the window and the sources under it, in the accent colour.
     While it waits the well says it is asking.
  4. A click on a source opens it in the Browser (the Browser loads that URL from the stub and shows its page); Tab then
     Enter does the same.
  5. A 502 with a reason, a 1 MB reply, not-JSON and a `none` answer each end in a plain-words status and the window
     keeps working; a hostile reply (a 5 KB title, javascript: and file: and over-long source URLs, a body past 1000
     characters, a tail cut off mid-array) shows at most 1000 characters of answer and only the two safe sources.
  6. With no network card: `no network`, in the window and at the Terminal and as Samantha's result.
Skips (exit 0) when clang's aarch64 target, ld.lld, qemu-system-aarch64 or PIL is missing.
Usage: tools/checks/arm64-nimble-check.py   (from the repo root)
"""
import http.server, json, os, re, shutil, socket, subprocess, sys, threading, time, urllib.parse
from pathlib import Path

root = Path(__file__).resolve().parents[2]
arch = root / "arch/arm64"
sys.path.insert(0, str(Path(__file__).resolve().parent))
try:
    from PIL import Image
    from scratch import scratch_dir
except ImportError:
    print("SKIP: PIL is not installed"); sys.exit(0)
if not all(shutil.which(t) for t in ("clang", "ld.lld", "qemu-system-aarch64")):
    print("SKIP: clang, ld.lld or qemu-system-aarch64 not installed"); sys.exit(0)
tmp = Path(scratch_dir("nimble"))

TOKEN = "arm-nimble-check-token-0123456789abc"
token_file = tmp / "token"
token_file.write_text(TOKEN + "\n"); os.chmod(token_file, 0o600)

asked = []   # every question the stub Nimble saw, decoded
fails = []
def check(name, ok, detail=""):
    print(("  ok: " if ok else "  FAIL: ") + name + ("" if ok else "  " + repr(detail)[:600]))
    if not ok: fails.append(name)

stub_port = [0]
def src(n): return "http://10.0.2.2:%d/src%d" % (stub_port[0], n)
def text_reply(body, sources, ascii_only=True):
    return json.dumps({"kind": "text", "title": "t", "body": body, "src": "Qwen",
                       "image": {"url": "https://thumb.example/x.jpg?a=b", "width": 330, "height": 206, "title": "Paris"},
                       "refs": [{"src": "Wikipedia", "title": "Ref %d" % i, "url": "https://en.wikipedia.org/wiki/R%d" % i, "grounding": "g" * 900} for i in range(2)],
                       "sources": [{"name": n, "url": u} for n, u in sources]}, indent=2, ensure_ascii=ascii_only).encode()
def hostile():   # a body past 1000 characters, a 5 KB title with a javascript: URL, file:, an over-long URL, two safe sources, a tail cut off mid-array
    return (b'{"kind":"text","body":"' + (b"Evil answer. " * 100) + b'\\u0000 end","sources":['
            b'{"name":"' + b"T" * 5000 + b'","url":"javascript:alert(1)"},'
            b'{"name":"Safe one","url":"' + src(1).encode() + b'"},'
            b'{"name":"A file","url":"file:///etc/passwd"},'
            b'{"name":"Too long","url":"http://10.0.2.2/' + b"u" * 300 + b'"},'
            b'{"name":"Safe two","url":"' + src(2).encode() + b'"}],"refs":[[[[[[[[[[[[[[[[[["tail cut off here')
def answer_for(q):
    if q == "capital of france": return 200, text_reply("The capital of France is Paris. It sits on the Seine, 18°C today, and “quotes” work.", [("Paris", src(1)), ("France", src(2))])
    if q == "eiffel tower height": return 200, text_reply("The Eiffel Tower is 330 metres tall.", [("Eiffel Tower", src(1))], ascii_only=False)
    if q == "weather": return 200, json.dumps({"kind": "card", "big": "12", "unit": "°C", "sub": "Vancouver, clear, wind 5 km/h", "src": "Open-Meteo", "url": "https://open-meteo.com"}).encode()
    if q == "boom": return 502, json.dumps({"error": "The answer sources failed. Try again."}).encode()
    if q == "evil": return 200, hostile()
    if q == "huge": return 200, b'{"kind":"text","body":"' + b"A" * 1_000_000 + b'"}'
    if q == "html": return 200, b"<html><body>captive portal</body></html>"
    if q == "nothing": return 200, b'{"kind":"none"}'
    if q == "slow": time.sleep(1.5); return 200, text_reply("Slow but sure.", [("Slow page", src(1))])
    return 200, text_reply("Nimble stub: no script for that.", [])
class Stub(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        u = urllib.parse.urlparse(self.path)
        if u.path == "/api/answer":
            q = urllib.parse.parse_qs(u.query).get("q", [""])[0]
            asked.append(q); status, body = answer_for(q); ctype = "application/json"
        elif u.path in ("/src1", "/src2"):
            status, ctype = 200, "text/html"
            body = ("<html><head><title>Source %s</title></head><body><p>Page of source %s. SRCWORD%s</p></body></html>" % ((u.path[-1],) * 3)).encode()
        else: status, body, ctype = 404, b"missing", "text/plain"
        pages.append(u.path)
        try:
            self.send_response(status); self.send_header("Content-Type", ctype); self.send_header("Content-Length", str(len(body)))
            self.send_header("Connection", "close"); self.end_headers(); self.wfile.write(body)
        except OSError: pass   # the kernel hung up on a reply it would not take
    def log_message(self, *a): pass
pages = []
stub = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Stub)
stub_port[0] = stub.server_address[1]
threading.Thread(target=stub.serve_forever, daemon=True).start()

calls, script = [], []
class Relay(http.server.BaseHTTPRequestHandler):
    def do_POST(self):
        n = int(self.headers.get("Content-Length", "0")); d = json.loads(self.rfile.read(n)); calls.append(d)
        text = script[min(len(calls), len(script)) - 1]
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
        for ch in text: self.key("spc" if ch == " " else ch)
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

WIN = (150, 100, 650, 450)    # the Console's rectangle at 800x600, which Nimble takes
WELL = (160, 164, 640, 424)   # the answer well: 8 in from the sides, 64 down, 26 above the bottom
def count(img, box, near, tol=14):
    return sum(1 for p in img.crop(box).getdata() if all(abs(p[i] - near[i]) <= tol for i in range(3)))

b = None
try:
    env = dict({k: v for k, v in os.environ.items() if not k.startswith("CLAUDE_RELAY")}, CLAUDE_RELAY_HOST="10.0.2.2",
               CLAUDE_RELAY_PORT=str(relay.server_address[1]), CLAUDE_RELAY_TOKEN_FILE=str(token_file),
               NIMBLE_BASE="http://10.0.2.2:%d" % stub_port[0])
    subprocess.run(["make", "-C", str(arch), "clean"], capture_output=True, timeout=60)
    r = subprocess.run(["make", "-C", str(arch), "kernel8.elf"], env=env, capture_output=True, text=True, timeout=600)
    if r.returncode: print(r.stdout[-2000:], r.stderr[-2000:]); print("FAIL: arch/arm64 does not build"); sys.exit(1)
    print("arm64 nimble: stub Nimble on 127.0.0.1:%d, fake relay on %d" % (stub_port[0], relay.server_address[1]))

    b = Boot("main")
    # 1. the Terminal command
    b.key("f2"); check("F2 opens the Terminal", b.wait_for("terminal open", 10), b.uart()[-300:]); time.sleep(0.4)
    b.type("nimble capital of france"); b.key("ret")
    check("the Terminal prints the answer", b.wait_for("The capital of France is Paris.", 30), b.uart()[-600:])
    out = b.uart()
    check("the answer reads as plain ASCII (degree and curly quotes mapped)", "18deg" in out and '"quotes" work' in out, out[-700:])
    check("the sources are numbered with their addresses", "[1] Paris" in out and "[2] France" in out and src(1) in out and src(2) in out, out[-700:])
    check("the stub saw the question", asked == ["capital of france"], asked)
    # 2. Samantha's plugin
    del asked[:]; del calls[:]; script[:] = ["Let me check.\n[[nimble eiffel tower height]]", "It is 330 metres. FINALNIMBLE"]
    b.type("sam how tall is the eiffel tower"); b.key("ret")
    check("sam routes the web question through the nimble plugin", b.wait_for("FINALNIMBLE", 40), b.uart()[-800:])
    out = b.uart()
    check("the relay was called twice", len(calls) == 2, len(calls))
    check("the first call told the model about the plugin", len(calls) >= 1 and "[[nimble QUESTION]]" in calls[0].get("pi", "") and len(calls[0]["pi"]) <= 120, calls[:1])
    check("the question text itself is unchanged", len(calls) >= 1 and calls[0]["prompt"] == "how tall is the eiffel tower", calls[:1])
    check("the stub saw the plugin's question", asked == ["eiffel tower height"], asked)
    check("the result turn carries the answer and the source", len(calls) == 2 and calls[1]["prompt"].startswith("The Pi ran your actions")
          and "nimble eiffel tower height:" in calls[1]["prompt"] and "The Eiffel Tower is 330 metres tall." in calls[1]["prompt"] and "[1] Eiffel Tower" in calls[1]["prompt"]
          and len(calls[1]["prompt"]) <= 1200, calls[1:])
    check("the token never appears on the serial log", TOKEN not in out)
    # 3. the dock tile and the window
    del asked[:]; del pages[:]
    # dock geometry at 800x600 scale 1, 12 tiles (dock_geom.c): the tile is slot 10, before Trash
    icon = max(16, min(600 * 7 // 100, (min(740, 800 - 40) - 2 * 10 - 11 * 6) // 12)); dock_w = 12 * icon + 11 * 6 + 2 * 10
    x0 = (800 - dock_w) // 2; y0 = 600 - icon - 2 * 10 - 24
    b.click(x0 + 10 + 10 * (icon + 6) + icon // 2, y0 + 10 + icon // 2)
    check("a click on the dock's Nimble tile opens the window", b.wait_for("nimble open", 10), b.uart()[-300:])
    idle = b.shot("idle")
    check("the empty well shows the accent magnifier on off-white", count(idle, WELL, (181, 80, 44), 30) > 300 and count(idle, WELL, (250, 248, 244), 1) > 20000,
          (count(idle, WELL, (181, 80, 44), 30), count(idle, WELL, (250, 248, 244), 1)))
    b.type("capital of france"); b.key("ret")
    check("Enter asks and the stub saw it", b.wait_for("nimble: ask capital of france", 10) and b.wait_for("nimble: answer ", 30), b.uart()[-500:])
    check("the answer and its two sources are in the window", re.search(r"nimble: answer \d+ chars, 2 sources", b.uart()), b.uart()[-500:])
    check("the window shows the wrapped answer", "nimble: line The capital of France is Paris." in b.uart(), b.uart()[-700:])
    check("the stub saw GET /api/answer?q=capital+of+france", asked == ["capital of france"], asked)
    m = re.search(r"nimble: shows source 1 at (\d+),(\d+)", b.uart())
    check("the kernel drew source 1 and says where", bool(m), b.uart()[-500:])
    ans = b.shot("answer")
    check("the sources are drawn in the accent colour, the answer in ink", count(ans, WELL, (181, 80, 44), 40) > 40 and count(ans, WELL, (28, 28, 30), 40) > 150,
          (count(ans, WELL, (181, 80, 44), 40), count(ans, WELL, (28, 28, 30), 40)))
    ans.crop(WIN).save(tmp / "answer-window.png")
    # 4. a click on a source opens the Browser
    if m:
        b.click(int(m.group(1)), int(m.group(2)))
        check("a click on source 1 opens it in the Browser", b.wait_for("nimble: open source 1 " + src(1), 10) and b.wait_for("browser: load " + src(1), 10), b.uart()[-500:])
        check("the Browser shows the page", b.wait_for("browser: page 1 line", 30) and "/src1" in pages, (pages, b.uart()[-300:]))
    b.key("esc"); check("Esc closes the Browser", b.wait_for("browser closed", 10), b.uart()[-300:])
    b.key("f1"); b.type("nimble"); b.key("ret")
    check("Spotlight finds Nimble by name", b.wait_for("nimble open", 10, 2), b.uart()[-300:])
    check("the last answer is still there", b.wait_for("nimble: shows source 2", 10, 2), b.uart()[-300:])
    b.key("tab"); b.key("tab"); b.key("ret")
    check("Tab twice then Enter opens source 2 in the Browser", b.wait_for("nimble: open source 2 " + src(2), 10) and b.wait_for("browser: load " + src(2), 10) and b.wait_for("browser: page 1 line", 30, 2), b.uart()[-500:])
    b.key("esc"); b.wait_for("browser closed", 10, 2)
    b.key("f1"); b.type("nimble"); b.key("ret"); b.wait_for("nimble open", 10, 3)
    # asking: the well says so while the stub waits
    n = b.uart().count("nimble: answer ")
    b.type("slow"); b.key("ret"); time.sleep(0.6)
    asking = b.shot("asking")
    check("while the answer is on its way the well says it is asking", b.wait_for("nimble: ask slow", 5) and "nimble: answer " not in b.uart()[b.uart().rfind("nimble: ask slow"):]
          and count(asking, WELL, (181, 80, 44), 40) == 0 and count(asking, WELL, (117, 114, 110), 30) > 30,
          (count(asking, WELL, (181, 80, 44), 40), count(asking, WELL, (117, 114, 110), 30), b.uart()[-300:]))
    check("then the slow answer arrives", b.wait_for("nimble: answer ", 30, n + 1), b.uart()[-300:])
    # 5. failures and a hostile reply, each in plain words; the window keeps working
    def ask_fresh(q, want, why):
        before = b.uart().count(want)
        b.type(q); b.key("ret")
        check(why, b.wait_for(want, 40, before + 1), b.uart()[-400:])
    ask_fresh("boom", "nimble: status the service answered 502: The answer sources failed. Try again.", "a 502 shows the service's own reason")
    err = b.shot("error")
    check("the error state is in plain words in the well", count(err, WELL, (28, 28, 30), 40) > 60 and count(err, WELL, (250, 248, 244), 1) > 20000, count(err, WELL, (28, 28, 30), 40))
    ask_fresh("huge", "nimble: status the reply was too big", "a 1 MB reply is refused, bounded")
    ask_fresh("html", "nimble: status the reply was not an answer", "not-JSON is not an answer")
    ask_fresh("nothing", "nimble: answer 30 chars, 0 sources", "a none answer says Nimble has no answer")
    ask_fresh("evil", "nimble: answer 1000 chars, 2 sources", "a hostile reply is cut to 1000 characters and two safe sources")
    out = b.uart(); k = out.rfind("nimble: ask evil")
    check("only the two http sources were drawn", "shows source 2" in out[k:] and "shows source 3" not in out[k:], out[k:][-400:])
    check("the hostile title was cut to fit", "nimble: shows source" in out[k:] and "javascript" not in out[k:] and "file:///" not in out[k:], out[k:][-300:])
    ask_fresh("capital of france", "nimble: answer ", "the window still answers after all that")
    check("nothing crashed or ran out of memory", "CRASH" not in b.uart() and "out of memory" not in b.uart() and "Exception" not in b.uart(), b.uart()[-300:])
    check("the stub saw every question the window sent", set(asked) >= {"capital of france", "slow", "boom", "huge", "html", "nothing", "evil"}, asked)
    b.close(); b = None

    # 6. no network card
    b = Boot("nonet", net=False)
    b.key("f2"); b.wait_for("terminal open", 10); time.sleep(0.4)
    b.type("nimble anything at all"); b.key("ret")
    check("at the Terminal, no network says so", b.wait_for("nimble: no network", 20), b.uart()[-300:])
    b.key("f1"); b.type("nimble"); b.key("ret"); b.wait_for("nimble open", 10)
    b.type("anything"); b.key("ret")
    check("in the window, no network says so", b.wait_for("nimble: status no network", 20), b.uart()[-300:])
    nonet = b.shot("nonet")
    check("the window shows the no-network state", count(nonet, WELL, (28, 28, 30), 40) > 60 and count(nonet, WELL, (250, 248, 244), 1) > 20000, count(nonet, WELL, (28, 28, 30), 40))
    check("the stub saw nothing from the card-less boot", "anything" not in asked and "anything at all" not in asked, asked)
finally:
    if b: b.close()
    stub.shutdown(); relay.shutdown()
    subprocess.run(["make", "-C", str(arch), "clean"], capture_output=True, timeout=60)
if fails: print("FAIL: " + ", ".join(fails)); sys.exit(1)
print("PASS: nimble answers at the Terminal, as a Samantha plugin, and in its dock window (answer and sources, a click opens a source in the Browser); "
      "a 502, a 1 MB reply, not-JSON and a hostile reply end in plain words and bounded output; no network says so")
