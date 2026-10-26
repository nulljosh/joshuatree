#!/usr/bin/env python3
"""Blackcomb's bookmarks, Recent and search on the ARM64 desktop (arch/arm64/browser_ui.h), over a stub HTTP server on the host.

Boots the virt build with a keyboard, a tablet, a network card and the check card (sdcard.py) as a virtio disk.
  1. First boot: the Browser loads two stub pages; a click on the star in the address bar saves the first, Ctrl+D the
     second. After QEMU stops, JT/BOOKMARK.TXT read back off the card image with mtools holds one line a page: the
     address, a space, the title. (The roadmap says BOOKMARKS.TXT; FAT 8.3 allows 8 characters before the dot.)
  2. Second boot on the same card: the Browser opens empty and lists both pages (`browser: shows bookmark N URL`). Tab
     picks the first, Enter opens it (the stub server sees GET /one). A click on the star, now on a saved page, takes it
     off: the card's file keeps only the second line.
  3. 6.7.0, Blackcomb: the first boot leaves JT/HISTORY.TXT, the pages opened newest first; the second boot opens it from
     Spotlight by the name Blackcomb and lists them under Recent (`browser: shows recent N URL`). Typed words with a space
     search (`browser: load https://html.duckduckgo.com/html/?q=raspberry+pi`), and a bare host gets https:// in front.
Skips (exit 0) when clang's aarch64 target, ld.lld, qemu-system-aarch64 or mtools is missing.
Usage: tools/checks/arm64-bookmarks-check.py   (from the repo root)
"""
import http.server, json, os, shutil, socket, subprocess, sys, threading, time
from pathlib import Path

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from scratch import scratch_dir
from sdcard import PART, build_card

root = Path(__file__).resolve().parents[2]
arch = root / "arch/arm64"
if not all(shutil.which(t) for t in ("clang", "ld.lld", "qemu-system-aarch64", "mmd", "mtype")):
    print("SKIP: clang, ld.lld, qemu-system-aarch64 or mtools not installed"); sys.exit(0)
tmp = Path(scratch_dir("arm64-bookmarks"))

seen = []
PAGES = {
    "/one": "<html><head><title>One</title></head><body><h1>First page</h1><p>nothing else</p></body></html>",
    "/two": "<html><head><title>Two</title></head><body><h1>Second page</h1><p>nothing else</p></body></html>",
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
ONE, TWO = "http://10.0.2.2:%d/one" % port, "http://10.0.2.2:%d/two" % port

fails = []
def check(name, ok, detail=""):
    print(("  ok: " if ok else "  FAIL: ") + name + ("" if ok else "  " + repr(detail)[:600]))
    if not ok: fails.append(name)

card = str(tmp / "card.img")
build_card(card, False)
MT = dict(os.environ, MTOOLS_SKIP_CHECK="1")
def mt(*a): return subprocess.run([a[0], "-i", f"{card}@@{PART * 512}", *a[1:]], capture_output=True, env=MT)
if mt("mmd", "::JT").returncode: print("FAIL: mtools could not make JT/ on the check card"); sys.exit(1)
def on_card():
    r = mt("mtype", "::JT/BOOKMARK.TXT")
    return r.stdout.decode(errors="replace") if r.returncode == 0 else None

subprocess.run(["make", "-C", str(arch), "clean"], capture_output=True, timeout=60)
r = subprocess.run(["make", "-C", str(arch), "kernel8.elf"], capture_output=True, text=True, timeout=600)
if r.returncode: print(r.stdout[-2000:], r.stderr[-2000:]); print("FAIL: arch/arm64 does not build"); sys.exit(1)

QCODE = {":": ("shift", "semicolon"), "/": ("slash",), ".": ("dot",), " ": ("spc",)}
class Session:
    def __init__(self, tag):
        self.log, sock = tmp / (tag + ".uart"), tmp / (tag + ".qmp")
        self.q = subprocess.Popen(["qemu-system-aarch64", "-machine", "virt", "-cpu", "cortex-a72", "-m", "256",
                                   "-global", "virtio-mmio.force-legacy=false", "-device", "ramfb",
                                   "-netdev", "user,id=n", "-device", "virtio-net-device,netdev=n",
                                   "-device", "virtio-keyboard-device", "-device", "virtio-tablet-device",
                                   "-drive", "if=none,id=card,file=%s,format=raw" % card, "-device", "virtio-blk-device,drive=card",
                                   "-display", "none", "-serial", "file:" + str(self.log), "-qmp", "unix:%s,server,nowait" % sock,
                                   "-kernel", str(arch / "kernel8.elf")], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        self.sock, self.s, self.f = sock, None, None
    def uart(self): return self.log.read_text(errors="replace") if self.log.exists() else ""
    def wait_for(self, text, secs, count=1):
        end = time.time() + secs
        while time.time() < end:
            if self.uart().count(text) >= count: return True
            if self.q.poll() is not None: return False
            time.sleep(0.1)
        return False
    def connect(self):
        for _ in range(100):
            if self.sock.exists(): break
            time.sleep(0.1)
        self.s = socket.socket(socket.AF_UNIX); self.s.settimeout(30); self.s.connect(str(self.sock)); self.f = self.s.makefile("rw"); self.f.readline()
        self.command("qmp_capabilities")
    def command(self, name, args=None):
        self.f.write(json.dumps({"execute": name, **({"arguments": args} if args else {})}) + "\n"); self.f.flush()
        while True:
            reply = json.loads(self.f.readline())
            if "error" in reply: raise SystemExit("FAIL: QMP " + repr(reply))
            if "return" in reply: return reply["return"]
    def key(self, *names, hold=50):
        self.command("send-key", {"keys": [{"type": "qcode", "data": k} for k in names], "hold-time": hold}); time.sleep(hold / 1000 + 0.1)
    def type_text(self, text):
        for ch in text: self.key(*QCODE.get(ch, (ch,)))
    def click(self, x, y):
        self.command("input-send-event", {"events": [{"type": "abs", "data": {"axis": "x", "value": int(x * 32767 / 800)}},
                                                     {"type": "abs", "data": {"axis": "y", "value": int(y * 32767 / 600)}}]}); time.sleep(0.2)
        for down in (True, False):
            self.command("input-send-event", {"events": [{"type": "btn", "data": {"button": "left", "down": down}}]}); time.sleep(0.1)
    def open_browser(self, name="browser"):
        self.key("f1")
        for k in name: self.key(k)
        self.key("ret")
        return self.wait_for("browser open", 10)
    def stop(self):
        if self.f: self.f.close()
        if self.s: self.s.close()
        if self.q.poll() is None: self.q.terminate()
        try: self.q.wait(timeout=5)
        except subprocess.TimeoutExpired: self.q.kill(); self.q.wait()

STAR = (625, 145)   # the star in the address bar at 800x600: the window's right edge less 30, the toolbar row
a = b = None
try:
    a = Session("first"); a.connect()
    if not a.wait_for("M2 input ready", 40): print("FAIL: did not boot", a.uart()[-600:]); sys.exit(1)
    time.sleep(0.5)
    check("DHCP leased an address", a.wait_for("net dhcp 10.0.2.15 gw 10.0.2.2", 30), a.uart()[-300:])
    check("Spotlight opens the Browser", a.open_browser(), a.uart()[-400:])
    check("no card file yet: no bookmarks", a.wait_for("browser: bookmarks loaded 0", 5) and on_card() is None, a.uart()[-300:])
    a.type_text(ONE); a.key("ret")
    check("page one loads", a.wait_for("browser: page ", 40), a.uart()[-500:])
    a.click(*STAR)
    check("a click on the star saves page one", a.wait_for("browser: star saved " + ONE, 10), a.uart()[-400:])
    time.sleep(0.5)
    check("the card holds the one line: address, then title", on_card() == ONE + " One\n", on_card())
    a.key("ctrl", "l"); a.type_text(TWO); a.key("ret")
    check("page two loads", a.wait_for("browser: page ", 40, count=2), a.uart()[-500:])
    a.key("ctrl", "d")
    check("Ctrl+D stars page two", a.wait_for("browser: star saved " + TWO, 10), a.uart()[-400:])
    time.sleep(0.5)
    a.stop(); a = None
    check("BOOKMARK.TXT read off the card image: one line a page", on_card() == ONE + " One\n" + TWO + " Two\n", on_card())
    hist = mt("mtype", "::JT/HISTORY.TXT").stdout.decode(errors="replace")
    check("HISTORY.TXT holds the pages opened, newest first", hist == TWO + " Two\n" + ONE + " One\n", hist)

    seen.clear()
    b = Session("second"); b.connect()
    if not b.wait_for("M2 input ready", 40): print("FAIL: did not boot", b.uart()[-600:]); sys.exit(1)
    time.sleep(0.5)
    check("DHCP leased an address (second boot)", b.wait_for("net dhcp 10.0.2.15 gw 10.0.2.2", 30), b.uart()[-300:])
    check("Spotlight opens it by the name Blackcomb (second boot)", b.open_browser("blackcomb"), b.uart()[-400:])
    check("the card's two bookmarks load", b.wait_for("browser: bookmarks loaded 2", 5), b.uart()[-300:])
    check("the empty Browser lists both pages", b.wait_for("browser: shows bookmark 1 " + ONE, 5) and b.wait_for("browser: shows bookmark 2 " + TWO, 5), b.uart()[-500:])
    check("and Recent under them, newest first", b.wait_for("browser: shows recent 1 " + TWO, 5) and b.wait_for("browser: shows recent 2 " + ONE, 5), b.uart()[-500:])
    b.key("tab")
    check("Tab picks the first saved page", b.wait_for("browser: selected bookmark 1", 10), b.uart()[-300:])
    b.key("ret")
    check("Enter opens it", b.wait_for("browser: bookmark 1\n", 10) and b.wait_for("browser: page ", 40) and seen == ["/one"], (seen, b.uart()[-400:]))
    b.click(*STAR)
    check("a star on a saved page takes it off", b.wait_for("browser: star removed " + ONE, 10), b.uart()[-400:])
    time.sleep(0.5)
    b.key("ctrl", "l"); b.type_text("raspberry pi"); b.key("ret")
    check("words with a space search DuckDuckGo's page", b.wait_for("browser: search raspberry pi", 10) and b.wait_for("browser: load https://html.duckduckgo.com/html/?q=raspberry+pi", 10), b.uart()[-400:])
    at = b.uart().rfind("q=raspberry+pi")   # the search's load ends one way or the other (on CI's network a real page, images and all) before the next address is typed
    end = time.time() + 60
    while time.time() < end and "browser: page " not in b.uart()[at:] and "browser: status " not in b.uart()[at:]: time.sleep(0.2)
    time.sleep(0.5); b.key("ctrl", "l"); b.type_text("example.org"); b.key("ret")
    check("a bare host gets https:// in front", b.wait_for("browser: load https://example.org\n", 10), b.uart()[-400:])
    b.stop(); b = None
    check("the card keeps only the second line", on_card() == TWO + " Two\n", on_card())
finally:
    for x in (a, b):
        if x: x.stop()
    server.shutdown()
    subprocess.run(["make", "-C", str(arch), "clean"], capture_output=True, timeout=60)
if fails: print("FAIL: " + ", ".join(fails)); sys.exit(1)
print("PASS: the star saves a page to JT/BOOKMARK.TXT, a fresh boot lists it and Recent on Blackcomb's start page, Tab then Enter opens it, a star on a saved page removes it, and words search")
