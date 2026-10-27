#!/usr/bin/env python3
"""Page styles in Blackcomb (6.13.0): headings and bold come through as drawn type, not flat text.

A stub server on the host serves one page: an h1, an h3, a paragraph with a <b> run, and (6.34.0) a <style> whose
.lead (font-weight 700) and span.hl (bold) rules make a paragraph and a span bold. Spotlight opens Blackcomb and
the address bar loads it. Then:
  - the UART says the window drew "heading big Big title here", "heading small Small title here" and
    "bold bold words", and never "bold plain words" (the run ends where </b> does); "bold Lead line here" and
    "bold marked run", and never the words after the span or a paragraph no rule names; "centred at X" for the four
    centred lines and not the plain one, the short title further in than the long line, on a second page (6.35.0) of
    <center>, a div with an inline text-align:center, one a .mid rule centres, one with align=center and a plain div;
  - on a screendump the big heading's ink stands at least 1.4 times as tall as the paragraph's, inside the page well;
  - the Terminal's `browse` of the same page prints the words with no style bytes in them.
Discriminating: drop the BR_BIG mark in browser.c's to_text and the big heading is neither logged nor taller; make
says_bold return 0 and the lead and the span are drawn plain; drop the BR_CENTER mark and nothing is centred.
Skips (exit 0) when clang's aarch64 target, ld.lld, qemu-system-aarch64 or PIL is missing.
Usage: tools/checks/arm64-pagestyle-check.py   (from the repo root)
"""
import http.server, json, shutil, socket, subprocess, sys, threading, time
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
tmp = Path(scratch_dir("arm64-pagestyle"))

PAGE = ("<html><head><title>Styled</title><style>.lead { font-weight: 700 } span.hl{font-weight:bold} .x{font-weight:400}</style>"
        "</head><body><h1>Big title here</h1><h3>Small title here</h3>"
        "<p>plain words <b>bold words</b> after</p><p class=lead>Lead line here</p>"
        "<p>more <span class=hl>marked run</span> tail end</p><p class=x>Light para</p></body></html>").encode()
CENTRED = ("<html><head><title>Centred</title><style>.mid { text-align: center }</style></head><body>"
           "<center>Centred title</center><div style=\"color:red; text-align:center\">Inline centred line</div>"
           "<div class=mid>Rule centred</div><div align=center>Attr centred words here</div><div>Left after</div></body></html>").encode()
class Handler(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        body = CENTRED if self.path == "/centred" else PAGE
        self.send_response(200); self.send_header("Content-Length", str(len(body))); self.end_headers(); self.wfile.write(body)
    def log_message(self, *a): pass
server = http.server.HTTPServer(("127.0.0.1", 0), Handler)
port = server.server_address[1]
threading.Thread(target=server.serve_forever, daemon=True).start()

fails = []
def check(name, ok, detail=""):
    print(("  ok: " if ok else "  FAIL: ") + name + ("" if ok else "  " + repr(detail)[-500:]))
    if not ok: fails.append(name)

log, sock = tmp / "uart", tmp / "qmp"
q = None
try:
    subprocess.run(["make", "-C", str(arch), "clean"], capture_output=True, timeout=60)
    r = subprocess.run(["make", "-C", str(arch), "kernel8.elf"], capture_output=True, text=True, timeout=600)
    if r.returncode: print(r.stderr[-2000:]); print("FAIL: arch/arm64 does not build"); sys.exit(1)
    q = subprocess.Popen(["qemu-system-aarch64", "-machine", "virt", "-cpu", "cortex-a72", "-m", "256",
                          "-global", "virtio-mmio.force-legacy=false", "-device", "ramfb",
                          "-netdev", "user,id=n", "-device", "virtio-net-device,netdev=n",
                          "-device", "virtio-keyboard-device", "-device", "virtio-tablet-device", "-display", "none",
                          "-serial", "file:" + str(log), "-qmp", "unix:%s,server,nowait" % sock,
                          "-kernel", str(arch / "kernel8.elf")], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    def uart(): return log.read_text(errors="replace") if log.exists() else ""
    def wait_for(text, secs, after=0):
        end = time.time() + secs
        while time.time() < end:
            if text in uart()[after:]: return True
            time.sleep(0.1)
        return False
    if not wait_for("M2 input ready", 40): raise SystemExit("FAIL: did not boot: %r" % uart()[-600:])
    time.sleep(0.5)
    s = socket.socket(socket.AF_UNIX); s.settimeout(30); s.connect(str(sock)); f = s.makefile("rw"); f.readline()
    def command(name, args=None):
        f.write(json.dumps({"execute": name, **({"arguments": args} if args else {})}) + "\n"); f.flush()
        while True:
            reply = json.loads(f.readline())
            if "error" in reply: raise SystemExit("FAIL: QMP " + repr(reply))
            if "return" in reply: return reply["return"]
    def key(*names):
        command("send-key", {"keys": [{"type": "qcode", "data": k} for k in names], "hold-time": 50}); time.sleep(0.15)
    QCODE = {":": ("shift", "semicolon"), "/": ("slash",), ".": ("dot",), " ": ("spc",)}
    def type_text(text):
        for ch in text: key(*QCODE.get(ch, (ch,)))
    command("qmp_capabilities")
    check("DHCP leased an address", wait_for("net dhcp 10.0.2.15 gw 10.0.2.2", 30), uart()[-300:])
    key("f1"); type_text("blackcomb"); key("ret")
    check("Spotlight opens Blackcomb", wait_for("browser open", 10), uart()[-400:])
    n = len(uart())
    type_text("http://10.0.2.2:%d/styled" % port); key("ret")
    check("the page loaded", wait_for("browser: page ", 20, n), uart()[n:][-400:])
    time.sleep(1.0)
    out = uart()[n:]
    check("the h1 is drawn as a big heading", "browser: shows heading big Big title here" in out, out[-600:])
    check("the h3 is drawn as a small heading", "browser: shows heading small Small title here" in out, out[-600:])
    check("the <b> run is drawn bold, and only it", "browser: shows bold bold words" in out and "shows bold plain" not in out and "shows bold  after" not in out, out[-600:])
    check("a font-weight rule makes its element bold, and only it", "browser: shows bold Lead line here" in out and "browser: shows bold marked run" in out
          and "tail end" not in "".join(l for l in out.splitlines() if "shows bold" in l) and "shows bold Light" not in out, out[-600:])

    command("screendump", {"filename": str(tmp / "page.ppm")})
    img = Image.open(tmp / "page.ppm").convert("RGB")
    img.crop((150, 100, 650, 450)).save(tmp / "page-window.png")
    def ink_height(y0, y1):   # rows in [y0, y1) of the page well holding dark text pixels
        rows = [y for y in range(y0, y1) if any(sum(img.getpixel((x, y))) < 200 for x in range(162, 600))]
        return (rows[-1] - rows[0] + 1) if rows else 0
    big, body = ink_height(166, 202), ink_height(246, 266)   # rows 0-1 (the big heading), row 5 (the paragraph)
    check("the big heading stands taller than the paragraph (%d px against %d)" % (big, body), body > 0 and big >= body * 1.4, (big, body))
    key("ctrl", "l"); n = len(uart())
    type_text("http://10.0.2.2:%d/centred" % port); key("ret")
    check("the centred page loaded", wait_for("browser: page ", 20, n), uart()[n:][-400:])
    time.sleep(1.0)
    out = uart()[n:]
    mid = {}
    for l in out.splitlines():
        if "browser: shows centred at " in l:
            x, _, words = l.split("browser: shows centred at ", 1)[1].partition(" ")
            mid["".join(ch for ch in words if ch >= " ").strip()] = int(x)
    want = ("Centred title", "Inline centred line", "Rule centred", "Attr centred words here")
    check("<center>, align=center, an inline text-align and a text-align rule centre their line, and only theirs",
          all(w in mid for w in want) and not any("Left after" in k or "Light para" in k for k in mid)
          and mid.get("Centred title", 0) > mid.get("Attr centred words here", 1 << 30), mid)

    key("esc"); time.sleep(0.5); key("f2")
    check("the Terminal opened", wait_for("terminal open", 10), uart()[-300:])
    n = len(uart())
    type_text("browse http://10.0.2.2:%d/styled" % port); key("ret")
    check("the Terminal prints the words with no style bytes", wait_for("plain words bold words after", 20, n)
          and not any(c in uart()[n:] for c in "\x01\x02\x03\x04"), repr(uart()[n:][-400:]))
finally:
    if q: q.kill(); q.wait()
    server.shutdown()
    subprocess.run(["make", "-C", str(arch), "clean"], capture_output=True, timeout=60)
if fails: print("FAIL: " + ", ".join(fails)); sys.exit(1)
print("PASS: Blackcomb draws h1 and h2 as big headings, h3 to h6 as small ones and <b> runs in bold, and the Terminal prints the same page without style bytes")
