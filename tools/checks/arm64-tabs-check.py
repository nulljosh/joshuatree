#!/usr/bin/env python3
"""Tabs in the ARM64 browser (6.16.0): each tab keeps its own page and history, at the Terminal and in Blackcomb.

A stub server on the host serves /one, /two and /three. At the Terminal:
  browse /one; `tab new` -> "tab 2 of 2" and an empty page; browse /two; `tab 1` -> /one is fetched again and shown;
  `back` in tab 1 -> "nothing behind" (tab 2's page is not in its history); `tabs` lists both with the front one starred;
  two more `tab new` reach four, a fifth says "tabs full"; `tab close` leaves three.
In the Blackcomb window: load /one, Cmd+T opens an empty tab with the address bar ready, /three loads there and the status
line says "tab 2 of 2"; Ctrl+Tab brings /one back (fetched again); Cmd+W closes that tab and leaves /three in front with
the window open; Cmd+W on the last tab closes the window, as on a Mac.
Discriminating: drop tab_keep() from tab_take's callers in browser.c and `tab 1` comes back empty.
Skips (exit 0) when clang's aarch64 target, ld.lld or qemu-system-aarch64 is missing.
Usage: tools/checks/arm64-tabs-check.py   (from the repo root)
"""
import http.server, json, shutil, socket, subprocess, sys, threading, time
from pathlib import Path

root = Path(__file__).resolve().parents[2]
arch = root / "arch/arm64"
sys.path.insert(0, str(Path(__file__).resolve().parent))
from scratch import scratch_dir
if not all(shutil.which(t) for t in ("clang", "ld.lld", "qemu-system-aarch64")):
    print("SKIP: clang, ld.lld or qemu-system-aarch64 not installed"); sys.exit(0)
tmp = Path(scratch_dir("arm64-tabs"))

gets = []
class Handler(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        gets.append(self.path)
        name = self.path.strip("/").capitalize()
        body = ("<html><head><title>%s</title></head><body><p>This is page %s.</p></body></html>" % (name, name.lower())).encode()
        self.send_response(200); self.send_header("Content-Length", str(len(body))); self.end_headers(); self.wfile.write(body)
    def log_message(self, *a): pass
server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
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
        command("send-key", {"keys": [{"type": "qcode", "data": k} for k in names], "hold-time": 50}); time.sleep(0.2)
    QCODE = {":": ("shift", "semicolon"), "/": ("slash",), ".": ("dot",), " ": ("spc",)}
    def type_text(text):
        for ch in text: key(*QCODE.get(ch, (ch,)))
    def line(text): type_text(text); key("ret")
    command("qmp_capabilities")
    check("DHCP leased an address", wait_for("net dhcp 10.0.2.15 gw 10.0.2.2", 30), uart()[-300:])
    key("f2")
    check("the Terminal opened", wait_for("terminal open", 10), uart()[-300:])
    base = "http://10.0.2.2:%d" % port

    n = len(uart()); line("browse %s/one" % base)
    check("tab 1 loads page one", wait_for("This is page one.", 20, n), uart()[n:][-300:])
    n = len(uart()); line("tab new")
    check("tab new opens tab 2, empty", wait_for("browser: tab 2 of 2", 10, n), uart()[n:][-300:])
    n = len(uart()); line("browse %s/two" % base)
    check("tab 2 loads page two", wait_for("This is page two.", 20, n), uart()[n:][-300:])
    k = len(gets); n = len(uart()); line("tab 1")
    check("tab 1 comes back with page one, fetched again", wait_for("browser: tab 1 of 2", 10, n) and wait_for("This is page one.", 20, n) and "/one" in gets[k:], (gets[k:], uart()[n:][-300:]))
    n = len(uart()); line("back")
    check("tab 1's history is its own", wait_for("browser: nothing behind", 10, n), uart()[n:][-300:])
    n = len(uart()); line("tabs")
    check("tabs lists both, the front one starred", wait_for("* 1 %s/one" % base, 10, n) and "  2 %s/two" % base in uart()[n:], uart()[n:][-300:])
    line("tab new"); line("tab new")
    n = len(uart()); line("tab new")
    check("a fifth tab is refused", wait_for("browser: tabs full", 10, n), uart()[n:][-300:])
    n = len(uart()); line("tab close")
    check("tab close leaves three", wait_for("of 3", 10, n), uart()[n:][-300:])

    key("esc"); time.sleep(0.5)
    key("f1"); type_text("blackcomb"); key("ret")
    check("Spotlight opens Blackcomb", wait_for("browser open", 10), uart()[-400:])
    n = len(uart()); key("meta_l", "w"); wait_for("of 2", 20, n)   # the Terminal's three tabs are the browser's: Cmd+W down to one
    n = len(uart()); key("meta_l", "w")
    check("Cmd+W closes tabs while there are more than one", wait_for("browser: tab 1 of 1", 20, n) and "browser closed" not in uart()[n:], uart()[n:][-300:])
    time.sleep(1.0)
    n = len(uart()); type_text("%s/one" % base); key("ret")
    check("the window loads page one", wait_for("browser: page ", 20, n), uart()[n:][-300:])
    n = len(uart()); key("meta_l", "t")
    check("Cmd+T opens an empty tab", wait_for("browser: tab 2 of 2", 10, n), uart()[n:][-300:])
    type_text("%s/three" % base); key("ret")
    check("the new tab loads page three", wait_for("browser: load %s/three" % base, 10, n) and wait_for("browser: page ", 20, n), uart()[n:][-300:])
    k = len(gets); n = len(uart()); key("ctrl", "tab")
    check("Ctrl+Tab brings tab 1 back, page one fetched again", wait_for("browser: tab 1 of 2", 10, n) and wait_for("browser: page ", 20, n) and "/one" in gets[k:], (gets[k:], uart()[n:][-300:]))
    n = len(uart()); key("meta_l", "w")
    check("Cmd+W closes the tab, not the window", wait_for("browser: tab 1 of 1", 10, n) and "browser closed" not in uart()[n:] and "/three" in gets[-1], (gets[-3:], uart()[n:][-300:]))
    n = len(uart()); key("meta_l", "w")
    check("Cmd+W on the last tab closes the window", wait_for("browser closed", 10, n), uart()[n:][-300:])
finally:
    if q: q.kill(); q.wait()
    server.shutdown()
    subprocess.run(["make", "-C", str(arch), "clean"], capture_output=True, timeout=60)
if fails: print("FAIL: " + ", ".join(fails)); sys.exit(1)
print("PASS: the browser keeps up to four tabs, each with its own page and history, from the Terminal and from Blackcomb's Cmd+T, Ctrl+Tab and Cmd+W")
