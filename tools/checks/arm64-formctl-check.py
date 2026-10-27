#!/usr/bin/env python3
"""Checkboxes, radio buttons and select menus in forms (6.28.0).

A stub server on the host serves a GET form: checkbox remember (unticked), checkbox news (value yes, ticked), radios
size s, m (picked), l, a select color (Red value r, Green selected and no value, Blue&amp;Gold), a text field q and a
submit button; /s echoes its query. At the Terminal:
  the form reads "[ ] [1]", "[x] [2]", "( ) [3]", "(o) [4]", "( ) [5]", "color: Green [6]", "q: ________ [7]", "Go [8]";
  open 1 ticks remember, open 2 unticks news, open 5 picks size l (and not m), fill 6 r picks Red by value, open 6 steps
  to Green, fill 6 blue&gold picks the third by its label in any case, fill 7 hello; open 8 sends exactly
  remember=on&size=l&color=Blue%26Gold&q=hello (no news, no m, the option's value being its label). Before it, `top`
  (6.31.0) shows the page redrawn: "[x] [1]", "[ ] [2]", "( ) [4]", "(o) [5]", "color: Blue&Gold [6]", "q: hello [7]".
In the Blackcomb window, Tab and Enter on the first checkbox ticks it in place and the UART says so.
Discriminating: drop the "unticked sends nothing" line in browser.c's submit and news=yes goes out; make field_show
return at once and `top` still shows the page as it came.
Skips (exit 0) when clang's aarch64 target, ld.lld or qemu-system-aarch64 is missing.
Usage: tools/checks/arm64-formctl-check.py   (from the repo root)
"""
import http.server, json, os, shutil, socket, subprocess, sys, threading, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from scratch import scratch_dir

root = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
arch = os.path.join(root, "arch", "arm64")
if not all(shutil.which(t) for t in ("clang", "ld.lld", "qemu-system-aarch64")):
    print("SKIP: clang, ld.lld or qemu-system-aarch64 not installed"); sys.exit(0)
tmp = scratch_dir("arm64-formctl")
FORM = b"""<html><head><title>Form</title></head><body><form action=/s>
<input type=checkbox name=remember> Remember me
<input type=checkbox name=news value=yes checked> News
<input type=radio name=size value=s> S <input type=radio name=size value=m checked> M <input type=radio name=size value=l> L
<select name=color><option value=r>Red</option><option selected>Green</option><option>Blue&amp;Gold</option></select>
<input name=q> <input type=submit value=Go></form></body></html>"""
queries = []
class Handler(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        if self.path.startswith("/s?"): queries.append(self.path[3:]); body = b"<html><body><p>sent</p></body></html>"
        else: body = FORM
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
                      "-netdev", "user,id=n", "-device", "virtio-net-device,netdev=n", "-device", "ramfb", "-device", "virtio-keyboard-device",
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
    QK = {" ": ("spc",), ".": ("dot",), "/": ("slash",), ":": ("shift", "semicolon"), "&": ("shift", "7")}
    def key(*k): command("send-key", keys=[{"type": "qcode", "data": x} for x in k]); time.sleep(0.12)
    def line(t):
        for ch in t: key(*QK.get(ch, (ch,)))
        key("ret")
    def run(t, want, secs=15):
        n = len(uart()); line(t); ok = wait_for(want, secs, n); return ok, uart()[n:]
    command("qmp_capabilities")
    check("DHCP leased an address", wait_for("net dhcp 10.0.2.15 gw 10.0.2.2", 30), uart()[-300:])
    key("f2"); wait_for("terminal open", 10)
    base = "http://10.0.2.2:%d" % port
    ok, out = run("browse %s/form" % base, "Go [8]", 20)
    flat = out.replace("\n", " ")
    check("the form reads its controls", ok and all(w in flat for w in ("[ ] [1]", "[x] [2]", "( ) [3]", "(o) [4]", "( ) [5]", "color: Green [6]", "q: ________ [7]")), out[-500:])
    check("open 1 ticks remember", run("open 1", "browser: remember ticked")[0])
    check("open 2 unticks news", run("open 2", "browser: news unticked")[0])
    check("open 5 picks size l", run("open 5", "browser: size is l")[0])
    check("fill 6 r picks Red by its value", run("fill 6 r", "browser: color is Red")[0])
    check("open 6 steps to Green", run("open 6", "browser: color is Green")[0])
    check("fill 6 blue&gold picks the third by its label, any case", run("fill 6 blue&gold", "browser: color is Blue&Gold")[0])
    run("fill 7 hello", "browser: field q set")
    ok, out = run("top", "Go [8]")
    flat = out.replace("\n", " ")
    check("the page shows every change without a reload", ok and all(w in flat for w in ("[x] [1]", "[ ] [2]", "( ) [4]", "(o) [5]", "color: Blue&Gold [6]", "q: hello [7]")), out[-500:])
    ok, out = run("open 8", "sent", 20)
    check("the form sends exactly what a browser would", queries[-1:] == ["remember=on&size=l&color=Blue%26Gold&q=hello"], queries[-1:])
    key("esc"); time.sleep(0.5)
    key("f1"); [key(c) for c in "blackcomb"]; key("ret"); wait_for("browser open", 10)
    n = len(uart()); [key(*QK.get(c, (c,))) for c in "%s/form" % base]; key("ret"); wait_for("browser: page ", 20, n)
    n = len(uart()); key("tab"); key("ret")
    check("in Blackcomb, Tab and Enter tick the first checkbox in place", wait_for("browser: remember ticked", 10, n) and "browser: editing field" not in uart()[n:], uart()[n:][-300:])
finally:
    q.kill(); q.wait(); server.shutdown()
    subprocess.run(["make", "-C", arch, "clean"], capture_output=True)
if fails: print("FAIL: " + ", ".join(fails)); sys.exit(1)
print("PASS: checkboxes tick, radios pick one of their name, a select steps or takes an option by value or label, and the form sends only what is ticked and picked")
