#!/usr/bin/env python3
"""The ARM64 text browser (arch/arm64/browser.c): `browse URL` typed at the Terminal's prompt fetches a page over
HTTPS from a server on the host, follows a redirect, prints readable text with numbered links, and `open N` follows
link N. All through QEMU's virtio keyboard (QMP send-key) and user network; the result is read off the UART log.

The server has a throwaway self-signed certificate for 10.0.2.2, built into the kernel's trust anchors with TLS_TA=.
  1. browse 10.0.2.2:PORT/start (a bare host: https is the default) -> 302 to /dir/page; the UART shows the title,
     the paragraph text, no script or style body, &#NN; decoded, "text [N]" links, "- " list items, a blank line
     before the heading, and the status line "URL  line 1 of Y".
  2. open 3 -> the relative link sub/rel resolves to /dir/sub/rel. back -> /dir/page again (no new fetch). forward.
  3. open 2 -> the absolute link /third; links lists both URLs; find grows jumps to that line; more at the end says so.
  4. browse .../heavy -> a page that is mostly script and style prints its one sentence.
  5. browse .../loop -> a redirect to itself stops at "browser: too many redirects"; the server saw 4 GETs at most.
  6. open 9 -> "browser: no such link". help lists the commands. All through cmd.c's one cmd_run.
Nothing typed here ever reaches Claude: the build has no relay token and "claude:" never appears.
Skips (exit 0) when clang's aarch64 target, ld.lld, openssl or qemu-system-aarch64 is missing.
Usage: tools/checks/arm64-browser-check.py   (from the repo root)
"""
import http.server, json, os, re, shutil, socket, ssl, subprocess, sys, tempfile, threading, time

root = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..")
arch = os.path.join(root, "arch/arm64")
if not all(shutil.which(t) for t in ("clang", "ld.lld", "qemu-system-aarch64", "openssl")):
    print("SKIP: clang, ld.lld, openssl or qemu-system-aarch64 not installed"); sys.exit(0)

seen = []
PAGE = (b"<html><head><title>Joshua &amp; the Tree</title><style>body{color:red}</style></head><body>"
        b"<script>alert('nope')</script><h1>Desert news</h1>\n<p>The   tree   grows slowly.</p>"
        b"<p>Read <a href='/second'>the second page</a> or <a href=\"https://10.0.2.2:%d/third\">the third</a> "
        b"or <a href='sub/rel'>a relative one</a>.</p><p>&#74;oshua&#x27;s &lt;tree&gt;&nbsp;&quot;grows&quot;</p>"
        b"<!-- hidden --><ul><li>one</li><li>two</li></ul></body></html>")
SECOND = b"<html><head><title>Second Page</title></head><body>Made it.</body></html>"
HEAVY = (b"<html><head><title>Heavy</title><script>var a = '<p>fake</p>';</script></head><body><style>p{x:y}</style>"
         + b"<script>" + b"if (a < b) { c(); }\n" * 400 + b"</script><p>One real sentence.</p></body></html>")
class Handler(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        seen.append(self.path)
        if self.path in ("/start", "/loop"):
            self.send_response(302); self.send_header("Location", "/dir/page" if self.path == "/start" else "/loop"); self.end_headers(); return
        body = PAGE % port if self.path == "/dir/page" else HEAVY if self.path == "/heavy" else SECOND
        self.send_response(200); self.send_header("Content-Length", str(len(body))); self.end_headers(); self.wfile.write(body)
    def log_message(self, *a): pass

tmp = tempfile.mkdtemp()
cert, key = tmp + "/cert.pem", tmp + "/key.pem"
subprocess.run(["openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes", "-days", "2", "-subj", "/CN=10.0.2.2",
                "-addext", "subjectAltName=DNS:10.0.2.2", "-keyout", key, "-out", cert], check=True, capture_output=True)
ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER); ctx.load_cert_chain(cert, key)
server = http.server.HTTPServer(("127.0.0.1", 0), Handler)
server.socket = ctx.wrap_socket(server.socket, server_side=True)
port = server.server_address[1]
threading.Thread(target=server.serve_forever, daemon=True).start()

QCODE = {":": ("shift", "semicolon"), "/": ("slash",), ".": ("dot",), " ": ("spc",), "-": ("minus",), "?": ("shift", "slash")}
class Boot:
    def __init__(self):
        self.log, sock = tmp + "/uart", tmp + "/qmp"
        self.q = subprocess.Popen(["qemu-system-aarch64", "-machine", "virt", "-cpu", "cortex-a72", "-m", "256",
                                   "-global", "virtio-mmio.force-legacy=false", "-netdev", "user,id=n", "-device", "virtio-net-device,netdev=n",
                                   "-device", "virtio-keyboard-device", "-display", "none", "-serial", "file:" + self.log,
                                   "-qmp", "unix:%s,server,nowait" % sock, "-kernel", os.path.join(arch, "kernel8.elf")],
                                  stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        if not self.wait_for("M2 input ready", 40): raise SystemExit("FAIL: did not boot: %r" % self.uart()[-600:])
        time.sleep(0.5)
        self.s = socket.socket(socket.AF_UNIX); self.s.settimeout(20); self.s.connect(sock); self.f = self.s.makefile("rw")
        self.f.readline(); self.cmd("qmp_capabilities")
        self.cmd("send-key", keys=[{"type": "qcode", "data": "f2"}])   # typing lives in the Terminal (docs/TERMINAL.md)
        if not self.wait_for("terminal open", 10): raise SystemExit("FAIL: F1 did not open the Terminal: %r" % self.uart()[-300:])
    def uart(self): return open(self.log, errors="replace").read() if os.path.exists(self.log) else ""
    def wait_for(self, text, secs, count=1):
        end = time.time() + secs
        while time.time() < end:
            if self.uart().count(text) >= count: return True
            time.sleep(0.1)
        return False
    def cmd(self, c, **a):
        self.f.write(json.dumps({"execute": c, "arguments": a}) + "\n"); self.f.flush()
        while True:
            r = json.loads(self.f.readline())
            if "return" in r or "error" in r: return r
    def key(self, *qcodes):
        self.cmd("send-key", keys=[{"type": "qcode", "data": k} for k in qcodes]); time.sleep(0.1)
    def type(self, text):
        for ch in text: self.key(*QCODE.get(ch, (ch,)))
        self.key("ret")
    def close(self): self.q.kill(); self.q.wait()

fails = []
def check(name, ok, detail=""):
    print(("  ok: " if ok else "  FAIL: ") + name + ("" if ok else "  " + repr(detail)[:600]))
    if not ok: fails.append(name)
PROMPT_RE = re.compile(r"Claude(?: Haiku 5\.5)? \$ ")   # ask.c's prompt: the default model, or Claude alone in a build with no relay token
def lines_after(out, n):   # the browser's output after the n-th "browser: fetching"
    return [l for l in PROMPT_RE.split(out.split("browser: fetching\n")[n])[0].splitlines() if l and not l.startswith("key ")]

b = None
try:
    subprocess.run(["make", "-C", arch, "clean"], capture_output=True, timeout=60)
    r = subprocess.run(["make", "-C", arch, "kernel8.elf", "TLS_TA=" + cert], capture_output=True, text=True, timeout=600)
    if r.returncode: print(r.stdout[-2000:], r.stderr[-2000:]); print("FAIL: arch/arm64 does not build"); sys.exit(1)
    b = Boot()
    check("DHCP leased an address", b.wait_for("net dhcp 10.0.2.15 gw 10.0.2.2", 20), b.uart()[-300:])
    b.type("browse 10.0.2.2:%d/start" % port)
    check("the page arrived through the redirect", b.wait_for("links: ", 60), b.uart()[-800:])
    page = lines_after(b.uart(), 1); text = "\n".join(page); flat = text.replace("\n", " ")
    raw = b.uart().split("browser: fetching\n")[1].split("ask> ")[0]
    check("a bare host defaults to https, and /start redirected to /dir/page", seen == ["/start", "/dir/page"], seen)
    check("the title line, entity decoded", "browser: Joshua & the Tree" in page, page)
    check("the heading and paragraph text, spaces collapsed", "Desert news" in page and "The tree grows slowly." in text, page)
    check("a blank line between the heading and the paragraph", "Desert news\n\nThe tree grows slowly." in raw, raw)
    check("script, style and comment bodies are gone", "alert" not in text and "color" not in text and "hidden" not in text, page)
    check("links are numbered after their text", "the second page [1]" in flat and "the third [2]" in flat and "a relative one [3]" in flat, page)
    check("numeric and named entities decode", "Joshua's <tree> \"grows\"" in text, page)
    check("list items on their own lines", "- one" in page and "- two" in page, page)
    check("the status line says where we are", "/dir/page" in text and "line 1 of" in text, page)
    check("link count", "links: 3, open N follows one" in page, page)
    check("every line fits the 53-column Pi console", all(len(l) <= 53 for l in page), [l for l in page if len(l) > 53])
    seen.clear()
    b.type("open 3")
    check("open 3 fetched the relative link as /dir/sub/rel", b.wait_for("links: 0", 60) and seen == ["/dir/sub/rel"], (seen, b.uart()[-400:]))
    seen.clear()
    b.type("back")
    check("back refetched /dir/page", b.wait_for("links: 3", 60) and seen == ["/dir/page"], (seen, b.uart()[-400:]))
    seen.clear()
    b.type("forward")
    check("forward went to /dir/sub/rel again", b.wait_for("links: 0", 60, count=2) and seen == ["/dir/sub/rel"], (seen, b.uart()[-400:]))
    b.type("forward")
    check("nothing ahead after the newest page", b.wait_for("browser: nothing ahead", 10), b.uart()[-300:])
    b.type("back"); b.wait_for("links: 3", 60, count=2)
    seen.clear()
    b.type("open 2")
    check("open 2 fetched the absolute link /third", b.wait_for("links: 0", 60, count=3) and seen == ["/third"], (seen, b.uart()[-400:]))
    check("the second title printed", "browser: Second Page" in b.uart() and "Made it." in b.uart(), b.uart()[-400:])
    b.type("back"); b.wait_for("links: 3", 60, count=3)
    b.type("links")
    check("links lists every URL with its number", b.wait_for("links: 3\n", 10) and "[3] https://10.0.2.2:%d/dir/sub/rel" % port in b.uart(), b.uart()[-500:])
    b.type("find grows")
    check("find is case blind and lands on the line", b.wait_for("browser: found", 10) and b.wait_for("line 3 of", 5), b.uart()[-400:])
    b.type("more")
    check("more past the end says so", b.wait_for("browser: end of page", 10), b.uart()[-300:])
    seen.clear()
    b.type("browse https://10.0.2.2:%d/heavy" % port)
    check("a script-heavy page prints its one sentence", b.wait_for("One real sentence.", 60) and "fake" not in b.uart() and "if (a" not in b.uart(), b.uart()[-500:])
    seen.clear()
    b.type("browse https://10.0.2.2:%d/loop" % port)
    check("a redirect loop stops", b.wait_for("browser: too many redirects", 90), b.uart()[-400:])
    check("at most 4 hops were fetched", 1 <= len(seen) <= 4, seen)
    b.type("open 9")
    check("open 9 says no such link", b.wait_for("browser: no such link", 10), b.uart()[-300:])
    b.type("help")
    check("help lists the commands", b.wait_for("find WORD", 10), b.uart()[-300:])
    check("nothing went to Claude", "claude:" not in b.uart())
finally:
    if b: b.close()
    server.shutdown()
    shutil.rmtree(tmp, ignore_errors=True)
    subprocess.run(["make", "-C", arch, "clean"], capture_output=True, timeout=60)
if fails: print("FAIL: " + ", ".join(fails)); sys.exit(1)
print("PASS: the ARM64 browser reads pages, follows links, goes back and forward, finds words and pages through text")
