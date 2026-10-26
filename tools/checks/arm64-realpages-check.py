#!/usr/bin/env python3
"""Real pages (6.12.0): the ARM64 browser reads a page past the old 64 KiB cap, chunked and gzipped, whole.

A server on the host answers over plain HTTP on QEMU's user network (10.0.2.2):
  /big    a 2 MiB page of seeded words, HTTP/1.1, Transfer-Encoding: chunked, Content-Encoding: gzip (about 700 KiB
          on the wire, ten times the old cap). The UART must say "browser: body 2097152 bytes", show the page's title
          and its first sentence.
  /plain  300 KiB with a Content-Length and no encoding: "browser: body 307200 bytes".
  /cut    a chunked reply whose last chunk claims more than arrives: "browser: chunked reply cut short".
The server also sees the request ask for HTTP/1.1 and gzip.

Discriminating: drop the dechunk in browser.c's page_body and /big fails as "gzip reply is damaged"; put RAW_MAX back
to 64 KiB and /plain reads 65 KiB at most.
Skips (exit 0) when clang's aarch64 target, ld.lld or qemu-system-aarch64 is missing.
Usage: tools/checks/arm64-realpages-check.py   (from the repo root)
"""
import gzip, http.server, json, os, random, shutil, socket, subprocess, sys, threading, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from scratch import scratch_dir

root = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
arch = os.path.join(root, "arch/arm64")
if not all(shutil.which(t) for t in ("clang", "ld.lld", "qemu-system-aarch64")):
    print("SKIP: clang, ld.lld or qemu-system-aarch64 not installed"); sys.exit(0)

BIG = 2 << 20
rnd = random.Random(7)
words = [w.encode() for w in "joshua tree pi page reads whole chunked gzip desert granite wind lizard yucca dusk".split()]
head = b"<html><head><title>A big page</title></head><body><p>The first sentence of a very big page.</p><p>"
filler = bytearray(head)
while len(filler) < BIG - 20: filler += rnd.choice(words) + b" "
BIGPAGE = bytes(filler[:BIG - 18]) + b"</p></body></html>"
assert len(BIGPAGE) == BIG
GZ = gzip.compress(BIGPAGE, 6)
PLAIN = (b"<html><head><title>A plain page</title></head><body><p>" + b"x" * 307200)[:307200 - 18] + b"</p></body></html>"
assert len(PLAIN) == 307200
seen = []

class Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    def do_GET(self):
        seen.append((self.request_version, self.headers.get("Accept-Encoding", "")))
        if self.path == "/plain":
            self.send_response(200); self.send_header("Content-Length", str(len(PLAIN))); self.send_header("Connection", "close")
            self.end_headers(); self.wfile.write(PLAIN); return
        data = GZ if self.path == "/big" else b"<p>cut</p>" * 10
        self.send_response(200); self.send_header("Transfer-Encoding", "chunked"); self.send_header("Connection", "close")
        if self.path == "/big": self.send_header("Content-Encoding", "gzip")
        self.end_headers()
        for i in range(0, len(data), 16384):
            c = data[i:i + 16384]; self.wfile.write(b"%x\r\n%s\r\n" % (len(c), c))
        self.wfile.write(b"0\r\n\r\n" if self.path == "/big" else b"4000\r\nshort")
    def log_message(self, *a): pass

tmp = scratch_dir("arm64-realpages")
server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
port = server.server_address[1]
threading.Thread(target=server.serve_forever, daemon=True).start()

QCODE = {":": ("shift", "semicolon"), "/": ("slash",), ".": ("dot",), " ": ("spc",)}
fails = []
def check(name, ok, detail=""):
    print(("  ok: " if ok else "  FAIL: ") + name + ("" if ok else "  " + repr(detail)[-500:]))
    if not ok: fails.append(name)

q = None
try:
    subprocess.run(["make", "-C", arch, "clean"], capture_output=True, timeout=60)
    r = subprocess.run(["make", "-C", arch, "kernel8.elf"], capture_output=True, text=True, timeout=600)
    if r.returncode: print(r.stderr[-2000:]); print("FAIL: arch/arm64 does not build"); sys.exit(1)
    log, sock = os.path.join(tmp, "uart"), os.path.join(tmp, "qmp")
    q = subprocess.Popen(["qemu-system-aarch64", "-machine", "virt", "-cpu", "cortex-a72", "-m", "256",
                          "-global", "virtio-mmio.force-legacy=false", "-netdev", "user,id=n", "-device", "virtio-net-device,netdev=n",
                          "-device", "virtio-keyboard-device", "-display", "none", "-serial", "file:" + log,
                          "-qmp", "unix:%s,server,nowait" % sock, "-kernel", os.path.join(arch, "kernel8.elf")],
                         stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    def uart(): return open(log, errors="replace").read() if os.path.exists(log) else ""
    def wait_for(text, secs, after=0):
        end = time.time() + secs
        while time.time() < end:
            if text in uart()[after:]: return True
            time.sleep(0.1)
        return False
    if not wait_for("M2 input ready", 40): raise SystemExit("FAIL: did not boot: %r" % uart()[-600:])
    time.sleep(0.5)
    s = socket.socket(socket.AF_UNIX); s.settimeout(20); s.connect(sock); f = s.makefile("rw"); f.readline()
    def cmd(c, **a):
        f.write(json.dumps({"execute": c, "arguments": a}) + "\n"); f.flush()
        while True:
            rep = json.loads(f.readline())
            if "return" in rep or "error" in rep: return rep
    def key(*k): cmd("send-key", keys=[{"type": "qcode", "data": x} for x in k]); time.sleep(0.1)
    def type_line(t):
        for ch in t: key(*QCODE.get(ch, (ch,)))
        key("ret")
    cmd("qmp_capabilities")
    key("f2")
    check("the Terminal opened", wait_for("terminal open", 10), uart()[-300:])
    check("DHCP leased an address", wait_for("net dhcp 10.0.2.15 gw 10.0.2.2", 20), uart()[-300:])

    n = len(uart()); type_line("browse http://10.0.2.2:%d/big" % port)
    check("a 2 MiB chunked gzip page reads whole", wait_for("browser: body %d bytes" % BIG, 90, n), uart()[n:][-500:])
    check("and shows its title and first sentence", wait_for("A big page", 10, n) and wait_for("The first sentence of a very big page.", 10, n), uart()[n:][-500:])
    check("the request asked for HTTP/1.1 and gzip", bool(seen) and seen[0] == ("HTTP/1.1", "gzip"), seen[:1])
    check("the wire carried more than the old 64 KiB cap", len(GZ) > 65536, len(GZ))

    n = len(uart()); type_line("browse http://10.0.2.2:%d/plain" % port)
    check("a 300 KiB plain page reads whole", wait_for("browser: body %d bytes" % len(PLAIN), 60, n), uart()[n:][-500:])

    n = len(uart()); type_line("browse http://10.0.2.2:%d/cut" % port)
    check("a chunk cut short is refused, not shown", wait_for("browser: chunked reply cut short", 30, n), uart()[n:][-500:])
finally:
    if q: q.kill(); q.wait()
    server.shutdown()
    subprocess.run(["make", "-C", arch, "clean"], capture_output=True, timeout=60)
if fails: print("FAIL: " + ", ".join(fails)); sys.exit(1)
print("PASS: the ARM64 browser reads a 2 MiB chunked gzip page and a 300 KiB plain one whole, asks for HTTP/1.1 and gzip, and refuses a chunk cut short")
