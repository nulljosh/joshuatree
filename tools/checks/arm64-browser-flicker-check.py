#!/usr/bin/env python3
"""The Browser repaints off screen (arch/arm64/browser_ui.h browser_paint, off_begin and off_end in main.c).

On the Pi the framebuffer is the live scanout, so a window cleared and redrawn in place flickers: every half-drawn state
is visible. The fix draws the whole window into a window-sized buffer and copies it to the screen in one pass. This runs
the BR_TRACE build (make brtrace-kernel8.elf), which hashes the real screen's window rectangle before a paint and again
just before the copy and logs whether it changed. Opening the Browser, loading, Tab, scrolling and typing in the address
bar must each log `paint offscreen ... 0` and never `paint direct` or a 1. A revert of the fix draws in place, logs
`paint direct` and fails here.
Skips (exit 0) when clang's aarch64 target, ld.lld, qemu-system-aarch64 or PIL is missing.
Usage: tools/checks/arm64-browser-flicker-check.py   (from the repo root)
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
tmp = Path(scratch_dir("browser-flicker"))

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
    PAINT = "browser: paint offscreen, screen changed before the copy: 0\n"
    def paints(): return uart().count(PAINT)
    check("opening paints off screen", paints() >= 1, uart()[-400:])
    check("the buffer was taken at boot, before any heap mark could rewind it", "offscreen buffer reserved at boot" in uart() and uart().index("offscreen buffer reserved") < uart().index("browser open"), uart()[:300])
    n0 = paints()
    type_text("http://10.0.2.2:%d/one" % port); key("ret")
    check("loading and the page paint off screen", wait_for("browser: page 2 lines", 40) and paints() >= n0 + 5, (paints(), uart()[-400:]))
    n1 = paints(); key("tab")
    check("Tab to a link paints off screen", wait_for("browser: selected link 1", 10) and paints() >= n1 + 1, (n1, paints(), uart()[-300:]))
    key("ctrl", "l"); type_text("http://10.0.2.2:%d/long" % port); key("ret")
    check("a long page loads", wait_for("browser: page 119 lines", 40), uart()[-300:])
    n2 = paints(); key("pgdn"); key("end"); key("home")
    time.sleep(0.5)
    check("scrolling paints off screen once per key", paints() >= n2 + 3, (n2, paints()))
    n3 = paints(); key("ctrl", "l"); type_text("abc"); key("backspace")
    time.sleep(0.5)
    check("typing in the address bar paints off screen", paints() >= n3 + 5, (n3, paints()))
    # a dock band copy is live once the pointer has hovered a slot; the Browser's buffer must never overlap it or be rewound by it
    def move(x, y):
        command("input-send-event", {"events": [{"type": "abs", "data": {"axis": "x", "value": int(x * 32767 / 800)}},
                                                {"type": "abs", "data": {"axis": "y", "value": int(y * 32767 / 600)}}]}); time.sleep(0.3)
    key("ctrl", "l"); key("esc"); time.sleep(0.3)   # the address bar closed again, the long page on screen
    move(400, 575); move(720, 300); ref = shot("ref")   # one hover first: the dock keeps its band copy from here on
    move(400, 575); move(400, 300); move(300, 250)
    key("pgdn"); key("end"); key("home")
    move(400, 575); move(180, 130); move(720, 300)
    after = shot("after")
    diff = ImageChops.difference(ref, after).getbbox()
    check("after dock hover, the pointer over the Browser and repaints, the screen is pixel for pixel what it was", diff is None, diff)
    log_text = uart()
    check("no paint ever drew straight onto the screen", "paint direct" not in log_text, log_text[-400:])
    check("the screen never changed before the copy", "screen changed before the copy: 1" not in log_text, log_text[-400:])
    key("esc"); key("esc")
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
print("PASS: every Browser paint (open, load, Tab, scroll, typing) drew off screen and left the real screen untouched until one copy")
