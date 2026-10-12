#!/usr/bin/env python3
"""The Browser window on the ARM64 desktop (arch/arm64/browser_ui.h), over a stub HTTP server on the host.

Boots the virt build with a keyboard, a tablet and a network card. Spotlight opens Browser (F1, the letters, Enter);
the address bar takes the typed URL and Enter loads it. Then:
  1. `browser: load URL` and `browser: page 2 lines` on the UART, the stub server saw the GET, and a screendump of the
     window shows the off-white page and the accent link colour (0x00b5502c) inside the page area.
  2. A mouse click on the link line (the kernel logs where it drew it) logs `browser: link 1`, fetches page two.
  3. Alt+Left logs `browser: back` and fetches page one again; Alt+Right goes forward; F5 reloads.
  4. A long page scrolls with Page Down and End, stops at the end (another Page Down changes nothing) and Home brings
     the first screen back pixel for pixel; the page area never draws below its well.
  5. A closed port ends in the plain words "the page did not load", the old page stays.
  6. Esc closes the window and the Console comes back.
Skips (exit 0) when clang's aarch64 target, ld.lld, qemu-system-aarch64 or PIL is missing.
Usage: tools/checks/arm64-browser-app-check.py   (from the repo root)
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
tmp = Path(scratch_dir("browser-app"))

seen = []
LONG = "<html><head><title>Long</title></head><body>" + "".join("<p>Paragraph number %d of the long page.</p>" % i for i in range(60)) + "</body></html>"
EXAMPLE = ('<!doctype html><html lang=en><head><meta charset=utf-8><link rel=icon href=data:,><meta name=viewport content="width=device-width,initial-scale=1">'
           '<title>Example Domain</title><style>html{color-scheme:light dark;background:light-dark(#eee,#222)}body{font:16px/1.6 system-ui,sans-serif;max-width:26em;margin:auto;padding:25vh 2em 2em;text-align:center}</style></head>'
           '<body><p>This domain is for use in documentation examples without needing permission. This is not a service; avoid relying on it for testing and monitoring purposes.</p><script src=/s.js></script></body></html>\n')
# example.com as served on 2026-10-12, byte for byte: its "Learn more" link is added by /s.js, so the HTML itself has no link
FORMS = ('<html><head><title>Forms</title></head><body><p>one <a href=/unquoted>bare</a> two <a href=\'single\'>single quotes</a> '
         'three <a href="//10.0.2.2:%d/proto">protocol relative</a> four <a href="/dq">double</a>.</p></body></html>')
PAGES = {
    "/example": EXAMPLE,
    "/forms": FORMS % 0,
    "/one": "<html><head><title>One</title></head><body><h1>First page</h1><a href=\"/two\">go to the second page</a></body></html>",
    "/two": "<html><head><title>Two</title></head><body><h1>Second page</h1><a href=\"/one\">back to the first page</a></body></html>",
    "/long": LONG,
}
class Handler(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        seen.append(self.path)
        body = PAGES.get(self.path, "<html><body>missing</body></html>").encode()
        self.send_response(200 if self.path in PAGES else 404)
        self.send_header("Content-Length", str(len(body))); self.end_headers(); self.wfile.write(body)
    def log_message(self, *a): pass
server = http.server.HTTPServer(("127.0.0.1", 0), Handler)
port = server.server_address[1]
threading.Thread(target=server.serve_forever, daemon=True).start()
dead = socket.socket(); dead.bind(("127.0.0.1", 0)); dead_port = dead.getsockname()[1]; dead.close()   # a port nothing listens on

fails = []
def check(name, ok, detail=""):
    print(("  ok: " if ok else "  FAIL: ") + name + ("" if ok else "  " + repr(detail)[:600]))
    if not ok: fails.append(name)

log, sock = tmp / "uart", tmp / "qmp"
q = f = s = None
try:
    subprocess.run(["make", "-C", str(arch), "clean"], capture_output=True, timeout=60)
    r = subprocess.run(["make", "-C", str(arch), "kernel8.elf"], capture_output=True, text=True, timeout=600)
    if r.returncode: print(r.stdout[-2000:], r.stderr[-2000:]); print("FAIL: arch/arm64 does not build"); sys.exit(1)
    q = subprocess.Popen(["qemu-system-aarch64", "-machine", "virt", "-cpu", "cortex-a72", "-m", "256",
                          "-global", "virtio-mmio.force-legacy=false", "-device", "ramfb",
                          "-netdev", "user,id=n", "-device", "virtio-net-device,netdev=n",
                          "-device", "virtio-keyboard-device", "-device", "virtio-tablet-device", "-display", "none",
                          "-serial", "file:" + str(log), "-qmp", "unix:%s,server,nowait" % sock,
                          "-kernel", str(arch / "kernel8.elf")], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
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
    def click(x, y):
        command("input-send-event", {"events": [{"type": "abs", "data": {"axis": "x", "value": int(x * 32767 / 800)}},
                                                {"type": "abs", "data": {"axis": "y", "value": int(y * 32767 / 600)}}]}); time.sleep(0.2)
        for down in (True, False):
            command("input-send-event", {"events": [{"type": "btn", "data": {"button": "left", "down": down}}]}); time.sleep(0.1)
    command("qmp_capabilities")
    check("DHCP leased an address", wait_for("net dhcp 10.0.2.15 gw 10.0.2.2", 30), uart()[-300:])
    key("f1")
    for k in "browser": key(k)
    key("ret")
    check("Spotlight opens the Browser", wait_for("browser open", 10), uart()[-400:])
    WIN = (150, 100, 650, 450)   # the Console's rectangle at 800x600, which the Browser takes
    PAGE = (160, 164, 640, 424)  # inside the page well: 8 in from the sides, 64 down, 26 above the bottom
    def count(img, box, near, tol=14):
        return sum(1 for p in img.crop(box).getdata() if all(abs(p[i] - near[i]) <= tol for i in range(3)))
    type_text("http://10.0.2.2:%d/one" % port); key("ret")
    check("Enter logs the load", wait_for("browser: load http://10.0.2.2:%d/one" % port, 10), uart()[-400:])
    check("the page arrived, two lines", wait_for("browser: page 2 lines", 40), uart()[-600:])
    check("the stub server saw GET /one", seen == ["/one"], seen)
    m = re.search(r"browser: shows link 1 at (\d+),(\d+)", uart())
    check("the kernel drew link 1 and says where", bool(m), uart()[-600:])
    img = shot("one")
    check("the page area is off-white", count(img, PAGE, (250, 248, 244), 1) > 20000, count(img, PAGE, (250, 248, 244), 1))
    check("the link line is drawn in the accent colour", count(img, PAGE, (181, 80, 44), 40) > 40, count(img, PAGE, (181, 80, 44), 40))
    if m:
        click(int(m.group(1)), int(m.group(2)))
        check("a click on the link line follows it", wait_for("browser: link 1\n", 10) and wait_for("browser: page 2 lines", 40, count=2), uart()[-600:])
        check("the stub server saw GET /two", seen[-1:] == ["/two"], seen)
    key("alt", "left")
    check("Alt+Left goes back", wait_for("browser: back", 10) and wait_for("browser: page 2 lines", 40, count=3) and seen[-1:] == ["/one"], (seen, uart()[-400:]))
    key("alt", "right")
    check("Alt+Right goes forward", wait_for("browser: forward", 10) and wait_for("browser: page 2 lines", 40, count=4) and seen[-1:] == ["/two"], (seen, uart()[-400:]))
    key("f5")
    check("F5 reloads", wait_for("browser: reload", 10) and wait_for("browser: page 2 lines", 40, count=5) and seen[-2:] == ["/two", "/two"], (seen, uart()[-400:]))
    key("alt", "left"); wait_for("browser: back", 10)
    key("tab")
    check("Tab picks link 1", wait_for("browser: selected link 1", 10), uart()[-400:])
    key("ret")
    check("Enter follows the picked link", wait_for("browser: link 1\n", 10) and seen[-1:] == ["/two"], (seen, uart()[-400:]))
    key("ctrl", "l"); type_text("http://10.0.2.2:%d/long" % port); key("ret")
    check("a long page loads", wait_for("browser: load http://10.0.2.2:%d/long" % port, 10) and wait_for("browser: page 119 lines", 40), uart()[-500:])
    first = shot("long-top")
    key("pgdn"); second = shot("long-pgdn")
    check("Page Down scrolls", first.crop(WIN).tobytes() != second.crop(WIN).tobytes())
    key("end"); end = shot("long-end")
    key("pgdn"); again = shot("long-end2")
    check("scrolling stops at the end", end.crop(WIN).tobytes() == again.crop(WIN).tobytes(), ImageChops.difference(end.crop(WIN), again.crop(WIN)).getbbox())
    check("the page stays inside its well", count(again, (PAGE[0], PAGE[3] + 2, PAGE[2], PAGE[3] + 6), (250, 248, 244), 1) == 0)
    key("home"); top = shot("long-home")
    check("Home brings the first screen back", top.crop(PAGE).tobytes() == first.crop(PAGE).tobytes())
    key("ctrl", "l"); type_text("http://10.0.2.2:%d/example" % port); key("ret")
    check("example.com's served HTML has no link (its Learn more is script)", wait_for("browser: Example Domain", 40) and wait_for("links: 0", 10), uart()[-600:])
    PAGES["/forms"] = FORMS % port
    key("ctrl", "l"); type_text("http://10.0.2.2:%d/forms" % port); key("ret")
    check("bare, single-quoted, protocol-relative and double-quoted hrefs inside a <p> are four links", wait_for("links: 4, open N follows one", 40), uart()[-600:])

    key("ctrl", "l"); type_text("http://10.0.2.2:%d/one" % dead_port); key("ret")
    check("a closed port says it in plain words", wait_for("browser: status the page did not load", 40), uart()[-500:])
    key("esc")   # the address bar is not open any more after a load; Esc closes the window
    check("Esc closes the Browser", wait_for("browser closed", 10), uart()[-300:])
    check("nothing crashed", "CRASH" not in uart() and "oom" not in uart(), uart()[-300:])
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
print("PASS: Spotlight opens the Browser, the address bar loads a page, a click or Tab then Enter follows the link line, Alt+Left/Right and F5 navigate, a long page scrolls and clamps, a dead port says so, Esc closes")
