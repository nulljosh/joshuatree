#!/usr/bin/env python3
"""Pointer motion over a long Browser page repaints nothing but the arrow (arch/arm64/browser_ui.h, main.c pointer_moved).

3.7.2: Joshua saw the Browser window glitch on the Pi while the pointer moved over a Wikipedia page. This serves a long
page (hundreds of lines, links among them) from a stubbed server, loads it in the Browser, then sends N pointer motion
events back and forth across the window on the BR_TRACE build (make brtrace-kernel8.elf). That build logs
`browser: paint ...` for every full Browser paint and `browser: wrap` every time the page is laid out again. Across the
motion both counts must not move at all, the pointer must really have moved (`mouse x,y` lines), and with the pointer
back where it started the screen must equal the reference pixel for pixel. A control then scrolls a page down: one
more paint, still no new wrap (scrolling reuses the layout). Counts, not wall-clock time, so CI is stable.
Skips (exit 0) when clang's aarch64 target, ld.lld, qemu-system-aarch64 or PIL is missing.
Usage: tools/checks/arm64-browser-mouse-check.py   (from the repo root)
"""
import http.server, json, os, re, shutil, socket, subprocess, sys, threading, time
from pathlib import Path

root = Path(__file__).resolve().parents[2]
arch = root / "arch/arm64"
try:
    from PIL import Image, ImageChops
    from scratch import scratch_dir
except ImportError:
    print("SKIP: PIL is not installed"); sys.exit(0)
if not all(shutil.which(t) for t in ("clang", "ld.lld", "qemu-system-aarch64")):
    print("SKIP: clang, ld.lld or qemu-system-aarch64 not installed"); sys.exit(0)
tmp = Path(scratch_dir("browser-mouse"))

seen = []
ROWS = "".join("<p>Row %d of a very long page about nothing in particular.</p>" % i for i in range(300))
LINKS = "".join("<p><a href=/page%d>link number %d</a></p>" % (i, i) for i in range(28))
LONG = "<html><head><title>Long</title></head><body>" + LINKS + ROWS + "</body></html>"
class Handler(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        seen.append(self.path)
        body = (LONG if self.path == "/long" else "<html><body>missing</body></html>").encode()
        self.send_response(200 if self.path == "/long" else 404)
        self.send_header("Content-Length", str(len(body))); self.end_headers(); self.wfile.write(body)
    def log_message(self, *a): pass
server = http.server.HTTPServer(("127.0.0.1", 0), Handler)
port = server.server_address[1]
threading.Thread(target=server.serve_forever, daemon=True).start()

fails = []
def check(name, ok, detail=""):
    print(("  ok: " if ok else "  FAIL: ") + name + ("" if ok else "  " + repr(detail)[:600]))
    if not ok: fails.append(name)

log, sock = tmp / "uart", tmp / "qmp"
q = f = s = None
try:
    subprocess.run(["make", "-C", str(arch), "clean"], capture_output=True, timeout=60)
    r = subprocess.run(["make", "-C", str(arch), "brtrace-kernel8.elf"], capture_output=True, text=True, timeout=600)
    if r.returncode: print(r.stdout[-2000:], r.stderr[-2000:]); print("FAIL: arch/arm64 does not build"); sys.exit(1)
    q = subprocess.Popen(["qemu-system-aarch64", "-machine", "virt", "-cpu", "cortex-a72", "-m", "256",
                          "-global", "virtio-mmio.force-legacy=false", "-device", "ramfb",
                          "-netdev", "user,id=n", "-device", "virtio-net-device,netdev=n",
                          "-device", "virtio-keyboard-device", "-device", "virtio-tablet-device", "-display", "none",
                          "-serial", "file:" + str(log), "-qmp", "unix:%s,server,nowait" % sock,
                          "-kernel", str(arch / "brtrace-kernel8.elf")], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    def uart(): return log.read_text(errors="replace") if log.exists() else ""
    def wait_for(text, secs, count=1):
        end = time.time() + secs
        while time.time() < end:
            if uart().count(text) >= count: return True
            if q.poll() is not None: return False
            time.sleep(0.1)
        return False
    if not wait_for("M2 input ready", 40): print("FAIL: did not boot", uart()[-600:]); sys.exit(1)
    time.sleep(0.5)
    s = socket.socket(socket.AF_UNIX); s.settimeout(30); s.connect(str(sock)); f = s.makefile("rw"); f.readline()
    def command(name, args=None):
        f.write(json.dumps({"execute": name, **({"arguments": args} if args else {})}) + "\n"); f.flush()
        while True:
            reply = json.loads(f.readline())
            if "error" in reply: raise SystemExit("FAIL: QMP " + repr(reply))
            if "return" in reply: return reply["return"]
    def key(*names, hold=50):
        command("send-key", {"keys": [{"type": "qcode", "data": k} for k in names], "hold-time": hold}); time.sleep(hold / 1000 + 0.1)
    QCODE = {":": ("shift", "semicolon"), "/": ("slash",), ".": ("dot",)}
    def type_text(text):
        for ch in text: key(*QCODE.get(ch, (ch,)))
    def shot(name):
        path = tmp / (name + ".ppm"); command("screendump", {"filename": str(path)})
        return Image.open(path).convert("RGB")
    def move(x, y, pause=0.3):
        command("input-send-event", {"events": [{"type": "abs", "data": {"axis": "x", "value": int(x * 32767 / 800)}},
                                                {"type": "abs", "data": {"axis": "y", "value": int(y * 32767 / 600)}}]}); time.sleep(pause)
    def paints(): return uart().count("browser: paint ")
    def wraps(): return uart().count("browser: wrap\n")
    def moves(): return len(re.findall(r"^mouse \d+,\d+$", uart(), re.M))
    command("qmp_capabilities")
    check("DHCP leased an address", wait_for("net dhcp 10.0.2.15 gw 10.0.2.2", 30), uart()[-300:])
    key("f1")
    for k in "browser": key(k)
    key("ret")
    check("Spotlight opens the Browser", wait_for("browser open", 10), uart()[-400:])
    type_text("http://10.0.2.2:%d/long" % port); key("ret")
    check("a long page loads", wait_for("browser: page", 60), uart()[-300:])
    m = re.search(r"browser: page (\d+) lines", uart())
    check("the page is hundreds of lines", m and int(m.group(1)) >= 300, m and m.group(0))
    key("ctrl", "l"); key("esc")
    move(400, 575); move(5, 300); time.sleep(1.0)   # one dock hover first (the dock keeps its band copy), then the arrow rests left of the window
    ref = shot("ref")
    p0, w0, m0 = paints(), wraps(), moves()
    N = 120
    for i in range(N):   # across the window, zigzagging over the toolbar, the page and the status line, then the dock and back
        x = 150 + (i * 37) % 500; y = 110 + (i * 53) % 320
        move(x, y, 0.02)
    move(400, 575, 0.2); move(5, 300, 0.5)
    time.sleep(1.0)
    check("the pointer really moved (%d reports)" % N, moves() - m0 >= N, (m0, moves()))
    check("pointer motion painted the Browser zero times", paints() == p0, (p0, paints()))
    check("pointer motion laid the page out zero times", wraps() == w0, (w0, wraps()))
    after = shot("after")
    diff = ImageChops.difference(ref, after).getbbox()
    check("with the arrow back where it was, the screen is pixel for pixel the reference", diff is None, diff)
    key("pgdn"); time.sleep(0.5)
    check("control: a page down paints once", paints() == p0 + 1, (p0, paints()))
    check("control: scrolling reuses the layout", wraps() == w0, (w0, wraps()))
    key("home"); key("esc")
    log_text = uart()
    check("no paint ever drew straight onto the screen", "paint direct" not in log_text, log_text[-400:])
    check("nothing crashed", "CRASH" not in log_text and "oom" not in log_text, log_text[-300:])
finally:
    if f: f.close()
    if s: s.close()
    if q and q.poll() is None: q.terminate()
    if q:
        try: q.wait(timeout=5)
        except subprocess.TimeoutExpired: q.kill(); q.wait()
    server.shutdown()
    subprocess.run(["make", "-C", str(arch), "clean"], capture_output=True, timeout=60)
if fails: print("FAIL: " + ", ".join(fails)); sys.exit(1)
print("PASS: %d pointer reports over a long Browser page painted nothing, laid nothing out and left the screen as it was" % N)
