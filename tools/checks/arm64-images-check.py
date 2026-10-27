#!/usr/bin/env python3
"""Inline images in Blackcomb (6.14.0): a page's PNG and JPEG are fetched, decoded, scaled to fit and drawn in place.

A stub server on the host serves a page with a line of text, a 120x80 PNG (red, a blue band across its middle), an
800x600 baseline JPEG (green) and an image that is not there. Spotlight opens Blackcomb and the address bar loads it:
  - the UART says "browser: image 1 120x80", "browser: image 2 400x300" (scaled down to fit, aspect kept) and
    "browser: image 3 failed";
  - the window logs where it drew each image, and a screendump there shows red with the blue band 40 px down, and green;
  - the server saw all three image requests after the page;
  - a second page with ten small PNGs loads the first eight (6.36.0) and never asks for the ninth.
Discriminating: drop load_images() from show() in browser.c and nothing is logged or drawn; set IMG_MAX back to 4 and
the eighth image never loads.
Skips (exit 0) when clang's aarch64 target, ld.lld, qemu-system-aarch64 or PIL is missing.
Usage: tools/checks/arm64-images-check.py   (from the repo root)
"""
import http.server, io, json, re, shutil, socket, subprocess, sys, threading, time
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
tmp = Path(scratch_dir("arm64-images"))

png = Image.new("RGB", (120, 80), (220, 30, 30))
png.paste((30, 60, 220), (0, 36, 120, 44))
b = io.BytesIO(); png.save(b, "PNG"); PNG = b.getvalue()
b = io.BytesIO(); Image.new("RGB", (800, 600), (40, 170, 60)).save(b, "JPEG", quality=90); JPG = b.getvalue()
PAGE = (b"<html><head><title>Pictures</title></head><body><p>Pictures follow.</p>"
        b"<img src=/red.png alt=red><img src='pic.jpg'><img src=\"/missing.png\"><p>After the pictures.</p></body></html>")
b = io.BytesIO(); Image.new("RGB", (24, 16), (200, 200, 40)).save(b, "PNG"); DOT = b.getvalue()
MANY = b"<html><body><p>Ten small pictures.</p>" + b"".join(b"<img src=/d%d.png>" % k for k in range(1, 11)) + b"<p>After them.</p></body></html>"
FILES = {"/pics": (PAGE, "text/html"), "/red.png": (PNG, "image/png"), "/pic.jpg": (JPG, "image/jpeg"), "/many": (MANY, "text/html")}
FILES.update({"/d%d.png" % k: (DOT, "image/png") for k in range(1, 11)})
seen = []
class Handler(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        seen.append(self.path)
        body, kind = FILES.get(self.path, (b"no", "text/plain"))
        self.send_response(200 if self.path in FILES else 404); self.send_header("Content-Type", kind)
        self.send_header("Content-Length", str(len(body))); self.end_headers(); self.wfile.write(body)
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
    QCODE = {":": ("shift", "semicolon"), "/": ("slash",), ".": ("dot",)}
    def type_text(text):
        for ch in text: key(*QCODE.get(ch, (ch,)))
    command("qmp_capabilities")
    check("DHCP leased an address", wait_for("net dhcp 10.0.2.15 gw 10.0.2.2", 30), uart()[-300:])
    key("f1"); type_text("blackcomb"); key("ret")
    check("Spotlight opens Blackcomb", wait_for("browser open", 10), uart()[-400:])
    n = len(uart())
    type_text("http://10.0.2.2:%d/pics" % port); key("ret")
    check("the PNG loads at its own size", wait_for("browser: image 1 120x80", 40, n), uart()[n:][-500:])
    check("the JPEG loads scaled down to fit, aspect kept", wait_for("browser: image 2 400x300", 30, n), uart()[n:][-500:])
    check("a missing image says it failed", wait_for("browser: image 3 failed", 30, n), uart()[n:][-500:])
    check("the server saw the page, then each image", seen[-4:] == ["/pics", "/red.png", "/pic.jpg", "/missing.png"], seen)
    check("the window drew both images", wait_for("browser: shows image 2 at", 10, n), uart()[n:][-500:])
    time.sleep(0.5)
    out = uart()[n:]
    at = {int(m.group(1)): (int(m.group(2)), int(m.group(3))) for m in re.finditer(r"browser: shows image (\d) at (\d+),(\d+)", out)}
    command("screendump", {"filename": str(tmp / "pics.ppm")})
    img = Image.open(tmp / "pics.ppm").convert("RGB")
    img.crop((150, 100, 650, 450)).save(tmp / "pics-window.png")
    def near(p, c, tol=40): return all(abs(p[i] - c[i]) <= tol for i in range(3))
    if 1 in at:
        x, y = at[1]
        check("image 1 is red where it was drawn, blue 40 px down", near(img.getpixel((x + 10, y + 10)), (220, 30, 30)) and near(img.getpixel((x + 10, y + 40)), (30, 60, 220)),
              (img.getpixel((x + 10, y + 10)), img.getpixel((x + 10, y + 40))))
    else: check("image 1's place was logged", False, out[-400:])
    if 2 in at:
        x, y = at[2]
        check("image 2 is green where it was drawn", near(img.getpixel((x + 20, y + 10)), (40, 170, 60)), img.getpixel((x + 20, y + 10)))
    else: check("image 2's place was logged", False, out[-400:])
    key("ctrl", "l"); n = len(uart())
    type_text("http://10.0.2.2:%d/many" % port); key("ret")
    ok = wait_for("browser: image 8 24x16", 40, n); time.sleep(1.0)
    check("a page's first eight images load (6.36.0), and no more", ok and "browser: image 9" not in uart()[n:] and "/d9.png" not in seen, uart()[n:][-500:])
finally:
    if q: q.kill(); q.wait()
    server.shutdown()
    subprocess.run(["make", "-C", str(arch), "clean"], capture_output=True, timeout=60)
if fails: print("FAIL: " + ", ".join(fails)); sys.exit(1)
print("PASS: Blackcomb fetches a page's PNG and JPEG after its text, scales them to fit, draws them in place, and says so when one is missing")
