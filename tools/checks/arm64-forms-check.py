#!/usr/bin/env python3
"""Forms and cookies (6.15.0): the ARM64 browser fills and sends a form, and keeps a host's cookie for its next request.

A stub server on the host (plain HTTP on 10.0.2.2):
  /login   a POST form: user, a password, a hidden token, a submit button. POST /login answers 303 to /home with
           Set-Cookie: sid=s3cret (HttpOnly) and Set-Cookie: sec=1 (Secure, which a plain http page may not set).
  /home    "Welcome back" when the request carries sid=s3cret, else "Not signed in".
  /search  a GET form, a search box and a <button>; /s echoes the query.
At the Terminal: `fill 1 joshua`, `fill 2 hunter2`, `open 3` -> the server reads exactly user=joshua&pass=hunter2&token=abc,
the redirect to /home carries sid and not sec, and the page says Welcome back; `fill` never prints a value. The search form
sends GET /s?q=hello+world. In the Blackcomb window: Tab to the user field, Enter, type, Enter, the same for the password
(the address bar shows stars), Tab to the button and Enter -> the server reads user=win&pass=pw&token=abc.
Discriminating: drop take_cookies() from fetch in browser.c and /home says Not signed in.
Skips (exit 0) when clang's aarch64 target, ld.lld, qemu-system-aarch64 or PIL is missing.
Usage: tools/checks/arm64-forms-check.py   (from the repo root)
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
tmp = Path(scratch_dir("arm64-forms"))

LOGIN = (b"<html><head><title>Sign in</title></head><body><form method=post action=/login>"
         b"<input name=user> <input type=password name=pass><input type=hidden name=token value=abc>"
         b"<input type=submit value=\"Sign in\"></form></body></html>")
SEARCH = b"<html><head><title>Search</title></head><body><form action=/s><input type=search name=q><button>Go</button></form></body></html>"
posts, cookies, gets = [], [], []
class Handler(http.server.BaseHTTPRequestHandler):
    def reply(self, body, extra=()):
        self.send_response(200)
        for k, v in extra: self.send_header(k, v)
        self.send_header("Content-Length", str(len(body))); self.end_headers(); self.wfile.write(body)
    def do_GET(self):
        gets.append(self.path); cookies.append(self.headers.get("Cookie", ""))
        if self.path == "/login": return self.reply(LOGIN)
        if self.path == "/search": return self.reply(SEARCH)
        if self.path.startswith("/s?"): return self.reply(b"<html><body><p>you asked " + self.path[3:].encode() + b"</p></body></html>")
        if self.path == "/home":
            ok = "sid=s3cret" in self.headers.get("Cookie", "")
            return self.reply(b"<html><head><title>Home</title></head><body><p>" + (b"Welcome back" if ok else b"Not signed in") + b"</p></body></html>")
        self.send_response(404); self.send_header("Content-Length", "0"); self.end_headers()
    def do_POST(self):
        posts.append(self.rfile.read(int(self.headers.get("Content-Length", 0))).decode())
        self.send_response(303); self.send_header("Location", "/home")
        self.send_header("Set-Cookie", "sid=s3cret; Path=/; HttpOnly"); self.send_header("Set-Cookie", "sec=1; Path=/; Secure")
        self.send_header("Content-Length", "0"); self.end_headers()
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
        command("send-key", {"keys": [{"type": "qcode", "data": k} for k in names], "hold-time": 50}); time.sleep(0.15)
    QCODE = {":": ("shift", "semicolon"), "/": ("slash",), ".": ("dot",), " ": ("spc",)}
    def type_text(text):
        for ch in text: key(*QCODE.get(ch, (ch,)))
    def line(text): type_text(text); key("ret")
    command("qmp_capabilities")
    check("DHCP leased an address", wait_for("net dhcp 10.0.2.15 gw 10.0.2.2", 30), uart()[-300:])
    key("f2")
    check("the Terminal opened", wait_for("terminal open", 10), uart()[-300:])

    base = "http://10.0.2.2:%d" % port
    n = len(uart()); line("browse %s/login" % base)
    check("the form reads as numbered fields and a button", wait_for("Sign in [3]", 20, n) and "user: ________ [1]" in uart()[n:] and "pass: ________ [2]" in uart()[n:], uart()[n:][-500:])
    n = len(uart()); line("fill 1 joshua")
    check("fill sets a field and says so", wait_for("browser: field user set", 10, n), uart()[n:][-300:])
    n = len(uart()); line("fill 2 hunter2")
    check("fill never prints the value", wait_for("browser: field pass set", 10, n) and "hunter2" not in uart()[n:].split("browser: field pass set")[-1], uart()[n:][-300:])
    n = len(uart()); line("open 3")
    check("the form is posted, hidden token and all", wait_for("Welcome back", 30, n) and posts[:1] == ["user=joshua&pass=hunter2&token=abc"], (posts, uart()[n:][-400:]))
    check("the redirect carried the session cookie and not the Secure one", "/home" in gets and cookies[gets.index("/home")] == "sid=s3cret", list(zip(gets, cookies)))
    check("the jar took sid and refused sec over http", "browser: cookie sid" in uart()[n:] and "browser: cookie sec" not in uart()[n:], uart()[n:][-400:])

    n = len(uart()); line("browse %s/search" % base)
    check("the search form reads", wait_for("Go [2]", 20, n), uart()[n:][-400:])
    line("fill 1 hello world"); time.sleep(0.5)
    n = len(uart()); line("open 2")
    check("a GET form sends its query in the address", wait_for("you asked q=hello+world", 20, n) and "/s?q=hello+world" in gets, (gets, uart()[n:][-300:]))

    key("esc"); time.sleep(0.5)
    key("f1"); type_text("blackcomb"); key("ret")
    check("Spotlight opens Blackcomb", wait_for("browser open", 10), uart()[-400:])
    n = len(uart()); type_text("%s/login" % base); key("ret")
    check("the window loads the form", wait_for("browser: page ", 20, n), uart()[n:][-300:])
    key("tab"); key("ret"); type_text("win"); key("ret")
    check("Tab and Enter edit the user field, Enter fills it", wait_for("browser: filled field 1", 10, n), uart()[n:][-300:])
    key("tab"); key("ret"); type_text("pw")
    command("screendump", {"filename": str(tmp / "pw.ppm")})
    img = Image.open(tmp / "pw.ppm").convert("RGB"); img.crop((150, 100, 650, 450)).save(tmp / "pw-window.png")
    key("ret")
    check("the password field is filled too", wait_for("browser: filled field 2", 10, n), uart()[n:][-300:])
    posts.clear(); key("tab"); key("ret")
    check("Tab to the button and Enter posts the window's values", wait_for("Welcome back", 30, n) and posts[:1] == ["user=win&pass=pw&token=abc"], (posts, uart()[n:][-300:]))
finally:
    if q: q.kill(); q.wait()
    server.shutdown()
    subprocess.run(["make", "-C", str(arch), "clean"], capture_output=True, timeout=60)
if fails: print("FAIL: " + ", ".join(fails)); sys.exit(1)
print("PASS: the browser fills and posts a form with its hidden field, follows the redirect with the session cookie, refuses a Secure cookie over http, sends a GET form as a query, and does it all from the Blackcomb window too")
