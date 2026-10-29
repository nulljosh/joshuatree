#!/usr/bin/env python3
"""What the page hides stays hidden (6.25.0): the browser skips elements a page keeps out of view.

A stub server on the host serves one page: a <style> block hiding .menu and #cookie-banner (one comma list), div.drawer,
an @media rule (left out), descendant selectors (6.33.0: "p .inner" and ".nav li" hide, "div p .deep" with three parts
hides only inside both, 6.50.0, and "section > div .pick" only under a direct div; 6.49.0: "nav.side > ul" hides the nav's own list and not a list a level deeper, with a script before it that holds
"<nav>"), a comment holding a fake rule; then a nav with class
"top menu" and a link in it, a cookie banner with nested divs, a drawer, a <p hidden>, a <p> with an inline display:none
among other declarations, an <img hidden>, and visible text with a link. At the Terminal, `browse` must show the visible
text, the @media one too, an .inner outside a p, a .deep in a p outside any div, a .pick
under a div that is not the section's own child, the list after the .nav list closes (nested lists
inside it count their own depth), a p right after an h3 hidden but not the next one or one after a div (6.86.0, "h3 + p"),
every div after a .ad sibling hidden but not one outside their parent (".ad ~ div"), number the visible link [1] (the hidden menu's link takes no number), and show none
of the hidden words.
Discriminating: make hidden_tag in browser.c always say no and every hidden word comes back; drop anc_track's call and
the descendant words come back; drop the middle-part tracking in anc_track and the three-part words come back; make
sib_track do nothing and the sibling words come back.
Skips (exit 0) when clang's aarch64 target, ld.lld or qemu-system-aarch64 is missing.
Usage: tools/checks/arm64-csshide-check.py   (from the repo root)
"""
import http.server, json, os, shutil, socket, subprocess, sys, threading, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from scratch import scratch_dir

root = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
arch = os.path.join(root, "arch", "arm64")
if not all(shutil.which(t) for t in ("clang", "ld.lld", "qemu-system-aarch64")):
    print("SKIP: clang, ld.lld or qemu-system-aarch64 not installed"); sys.exit(0)
tmp = scratch_dir("arm64-csshide")
PAGE = b"""<html><head><title>Hide</title><style>
/* a comment {display:none} .visible{display:none} */
.menu, #cookie-banner { display : none; }
@media (max-width: 600px) { .wide { display: none } }
div.drawer{color:red;display:none}
p .inner { display:none }
.nav  li { display:none }
div p .deep { display:none }
nav.side > ul { display:none }
section > div .pick { display:none }
h3 + p { display:none }
.ad ~ div { display:none }
</style></head><body>
<nav class="top menu"><a href=/x>Menu link</a> Menu words</nav>
<div id="cookie-banner"><div>We use cookies</div><div>nested crumbs</div></div>
<div class="drawer">Drawer text</div>
<p hidden>Hidden paragraph</p>
<p style="color: red; display: none">Inline hidden</p>
<p class="wide">Wide text stays</p>
<p class="visible"><span class="inner">Inner hidden</span> Para stays</p>
<div><span class="inner">Lone inner stays</span></div>
<div><p><span class="deep">Deep hidden</span></p></div>
<p><span class="deep">Deep outside stays</span></p>
<section><div><b class=pick>Pick hidden</b></div><article><div><b class=pick>Pick deeper stays</b></div></article></section>
<ul class="nav"><li>Nav item<ul><li>Sub item</li></ul></li><li>Nav two</li></ul>
<ul><li>After nav stays</li></ul>
<nav class=side><script>var x = "<nav>";</script><ul><li>Side menu</li></ul><div><ul><li>Deeper list stays</li></ul></div></nav>
<div><h3>Head stays</h3><p>Next hidden</p><p>Second p stays</p><h3>Head two</h3><div>Gap</div><p>Gap p stays</p></div>
<div><span class=ad>Ad stays</span><b>Bold stays</b><div>Sib div hidden</div></div><div>Outer div stays</div>
<img src=/a.png hidden>
<p>Visible text <a href=/y>visible link</a> and the end.</p>
</body></html>"""
class Handler(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        self.send_response(200); self.send_header("Content-Length", str(len(PAGE))); self.end_headers(); self.wfile.write(PAGE)
    def log_message(self, *a): pass
server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
port = server.server_address[1]
threading.Thread(target=server.serve_forever, daemon=True).start()
fails = []
def check(name, ok, detail=""):
    print(("  ok: " if ok else "  FAIL: ") + name + ("" if ok or not detail else " -- " + str(detail)[-500:]))
    if not ok: fails.append(name)
subprocess.run(["make", "-C", arch, "clean"], capture_output=True)
b = subprocess.run(["make", "-C", arch, "kernel8.elf"], capture_output=True, text=True, timeout=900)
if b.returncode: print((b.stdout + b.stderr)[-1500:]); print("FAIL: arch/arm64 does not build"); sys.exit(1)
log, sock = os.path.join(tmp, "uart"), os.path.join(tmp, "qmp")
q = subprocess.Popen(["qemu-system-aarch64", "-machine", "virt", "-cpu", "cortex-a72", "-m", "256", "-global", "virtio-mmio.force-legacy=false",
                      "-netdev", "user,id=n", "-device", "virtio-net-device,netdev=n", "-device", "virtio-keyboard-device",
                      "-display", "none", "-serial", "file:" + log, "-qmp", "unix:%s,server,nowait" % sock,
                      "-kernel", os.path.join(arch, "kernel8.elf")], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
def uart(): return open(log, errors="replace").read() if os.path.exists(log) else ""
def wait_for(text, secs, after=0):
    end = time.time() + secs
    while time.time() < end:
        if text in uart()[after:]: return True
        time.sleep(0.1)
    return False
try:
    if not wait_for("M2 input ready", 60): raise SystemExit("FAIL: did not boot: %r" % uart()[-600:])
    time.sleep(0.5)
    s = socket.socket(socket.AF_UNIX); s.settimeout(30); s.connect(sock); f = s.makefile("rw"); f.readline()
    def command(name, **a):
        f.write(json.dumps({"execute": name, "arguments": a}) + "\n"); f.flush()
        while True:
            r = json.loads(f.readline())
            if "return" in r or "error" in r: return r
    QK = {" ": ("spc",), ".": ("dot",), "/": ("slash",), ":": ("shift", "semicolon")}
    def key(*k): command("send-key", keys=[{"type": "qcode", "data": x} for x in k]); time.sleep(0.12)
    def line(t):
        for ch in t: key(*QK.get(ch, (ch,)))
        key("ret")
    command("qmp_capabilities")
    check("DHCP leased an address", wait_for("net dhcp 10.0.2.15 gw 10.0.2.2", 30), uart()[-300:])
    key("f2"); wait_for("terminal open", 10)
    n = len(uart()); line("browse http://10.0.2.2:%d/hide" % port)
    ok = wait_for("and the end.", 20, n)
    for _ in range(3):   # browse shows 16 lines; more shows the next 16
        if ok: break
        line("more"); ok = wait_for("and the end.", 10, n)
    check("the page loads", ok, uart()[n:][-500:])
    time.sleep(0.5)
    out = uart()[n:]
    for w in ("Wide text stays", "Para stays", "Lone inner stays", "Deep outside stays", "Pick deeper stays", "After nav stays", "Deeper list stays", "Head stays", "Second p stays", "Gap p stays", "Ad stays", "Bold stays", "Outer div stays", "Visible text", "visible link [1]"):
        check("shows: %s" % w, w in out, out[-600:])
    for w in ("Menu link", "Menu words", "cookies", "nested crumbs", "Drawer text", "Hidden paragraph", "Inline hidden", "Inner hidden", "Nav item", "Sub item", "Nav two", "Side menu", "Deep hidden", "Pick hidden", "Next hidden", "Sib div hidden"):
        check("hides: %s" % w, w not in out, out[-600:])
finally:
    q.kill(); q.wait(); server.shutdown()
    subprocess.run(["make", "-C", arch, "clean"], capture_output=True)
if fails: print("FAIL: " + ", ".join(fails)); sys.exit(1)
print("PASS: the browser skips what a page hides (simple display:none rules from <style>, the hidden attribute, inline display:none, three-part rules too) and keeps what @media and complex selectors would hide only sometimes")
