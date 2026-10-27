#!/usr/bin/env python3
"""History you can search (6.27.0): pages the browser loaded, found again by their words, most visited first on request.

A stub server on the host serves three pages: /desert ("Desert", granite lizard yucca), /coast ("Coast", ocean kelp
granite) and /city ("City", traffic lights). On a card with a JT folder, at the Terminal, the browser visits desert,
coast, city, then desert again. Then:
  `history` lists Desert, City, Coast, newest first (desert's second visit moved it up, once);
  `history granite` finds Desert and Coast, `history granite kelp` Coast alone, `history GRANITE` the same as lower case;
  `history top` puts Desert, visited twice, first;
  `history clear` empties it, and `history` then lists nothing.
Discriminating: drop hist_add from show() in browser.c and `history` finds nothing.
Skips (exit 0) when clang's aarch64 target, ld.lld, qemu-system-aarch64 or mtools is missing.
Usage: tools/checks/arm64-history-check.py   (from the repo root)
"""
import http.server, json, os, re, shutil, socket, subprocess, sys, threading, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from scratch import scratch_dir
from sdcard import PART, build_card

root = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
arch = os.path.join(root, "arch", "arm64")
if not all(shutil.which(t) for t in ("clang", "ld.lld", "qemu-system-aarch64", "mmd")):
    print("SKIP: clang, ld.lld, qemu-system-aarch64 or mtools not installed"); sys.exit(0)
tmp = scratch_dir("arm64-history")
card = os.path.join(tmp, "card.img"); build_card(card, False)
subprocess.run(["mmd", "-i", f"{card}@@{PART * 512}", "::JT"], capture_output=True, env=dict(os.environ, MTOOLS_SKIP_CHECK="1"))
PAGES = {"/desert": ("Desert", "Granite boulders, a lizard and a yucca in the sun."),
         "/coast": ("Coast", "Ocean swell over kelp and wet granite."),
         "/city": ("City", "Traffic lights at every corner.")}
class Handler(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        t, w = PAGES.get(self.path, ("Missing", "nothing"))
        body = ("<html><head><title>%s</title></head><body><p>%s</p></body></html>" % (t, w)).encode()
        self.send_response(200); self.send_header("Content-Length", str(len(body))); self.end_headers(); self.wfile.write(body)
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
                      "-drive", "if=none,id=card,file=%s,format=raw" % card, "-device", "virtio-blk-device,drive=card",
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
        for ch in t: key(*(("shift", ch.lower()) if ch.isupper() else QK.get(ch, (ch,))))
        key("ret")
    def run(t, want, secs=20):
        n = len(uart()); line(t); ok = wait_for(want, secs, n); time.sleep(0.3); return ok, uart()[n:]
    def listed(out): return re.findall(r"^\d+  (\w+)  http", out, re.M)
    command("qmp_capabilities")
    check("DHCP leased an address", wait_for("net dhcp 10.0.2.15 gw 10.0.2.2", 30), uart()[-300:])
    key("f2"); wait_for("terminal open", 10)
    base = "http://10.0.2.2:%d" % port
    for p in ("desert", "coast", "city", "desert"):
        ok, _ = run("browse %s/%s" % (base, p), "links: ", 20)
        check("visits /%s" % p, ok)
    ok, out = run("history", " pages\n")
    check("history lists Desert, City, Coast, newest first, each once", listed(out) == ["Desert", "City", "Coast"] and "browser: 3 pages" in out, out[-400:])
    ok, out = run("history granite", " pages\n")
    check("history granite finds Desert and Coast", sorted(listed(out)) == ["Coast", "Desert"], out[-400:])
    ok, out = run("history granite kelp", " page")
    check("history granite kelp finds Coast alone", listed(out) == ["Coast"], out[-400:])
    ok, out = run("history GRANITE", " pages\n")
    check("the search ignores case", sorted(listed(out)) == ["Coast", "Desert"], out[-400:])
    ok, out = run("history top", " pages\n")
    check("history top puts Desert, visited twice, first", listed(out)[:1] == ["Desert"], out[-400:])
    ok, out = run("history clear", "browser: history cleared")
    check("history clear empties it", ok, out[-200:])
    ok, out = run("history", " pages\n")
    check("and then history lists nothing", "browser: 0 pages" in out, out[-200:])
finally:
    q.kill(); q.wait(); server.shutdown()
    subprocess.run(["make", "-C", arch, "clean"], capture_output=True)
if fails: print("FAIL: " + ", ".join(fails)); sys.exit(1)
print("PASS: the browser keeps a searchable history: newest first, one line a page, words in any case, the most visited on request, and a clear that empties it")
