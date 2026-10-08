#!/usr/bin/env python3
"""ARM64 HTTPS: the aarch64 kernel fetches a page over TLS 1.2 (arch/arm64/tls.c, BearSSL) from a server on the host.

Makes a throwaway self-signed certificate for 10.0.2.2 (QEMU's user-mode address for the host), starts a Python HTTPS
server on it, builds arch/arm64 with TLSPORT set to that port and TLS_TA pointing at the certificate, and boots on
QEMU's user network. Checks:
  - the server saw the GET, so the handshake really finished (the server would refuse a bad one);
  - "tls 200 N bytes", N being the exact length of the body it sent;
  - a second build WITHOUT the throwaway certificate in the trust anchors prints "tls FAIL 62" (BearSSL's
    "certificate not trusted") and the server never sees a request: the trust check is real, not decoration.
Skips (exit 0) when clang's aarch64 target, ld.lld, openssl or qemu-system-aarch64 is missing.
Usage: tools/checks/arm64-tls-check.py   (from the repo root)
"""
import http.server, os, shutil, ssl, subprocess, sys, tempfile, threading, time

root = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..")
arch = os.path.join(root, "arch/arm64")
if not all(shutil.which(t) for t in ("clang", "ld.lld", "qemu-system-aarch64", "openssl")):
    print("SKIP: clang, ld.lld, openssl or qemu-system-aarch64 not installed"); sys.exit(0)

REPLY = b"hello over tls, joshua tree arm64 reads this\n" * 2
seen = []
class Handler(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        seen.append(self.path)
        self.send_response(200)
        self.send_header("Content-Length", str(len(REPLY)))
        self.end_headers()
        self.wfile.write(REPLY)
    def log_message(self, *a): pass

tmp = tempfile.mkdtemp()
cert, key = tmp + "/cert.pem", tmp + "/key.pem"
subprocess.run(["openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes", "-days", "2", "-subj", "/CN=10.0.2.2",
                "-addext", "subjectAltName=DNS:10.0.2.2", "-keyout", key, "-out", cert], check=True, capture_output=True)
ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
ctx.load_cert_chain(cert, key)
server = http.server.HTTPServer(("127.0.0.1", 0), Handler)
server.socket = ctx.wrap_socket(server.socket, server_side=True)
port = server.server_address[1]
threading.Thread(target=server.serve_forever, daemon=True).start()

fails = []
def build(ta):
    subprocess.run(["make", "-C", arch, "clean"], capture_output=True, timeout=60)   # TLSPORT and the anchors are baked in
    r = subprocess.run(["make", "-C", arch, "kernel8.elf", "TLSPORT=%d" % port, "TLS_TA=" + ta], capture_output=True, text=True, timeout=600)
    if r.returncode: print(r.stdout[-2000:], r.stderr[-2000:]); print("FAIL: arch/arm64 does not build"); sys.exit(1)
def boot(name, secs):
    log = "%s/%s.uart" % (tmp, name)
    q = subprocess.Popen(["qemu-system-aarch64", "-machine", "virt", "-cpu", "cortex-a72", "-m", "256",
                          "-global", "virtio-mmio.force-legacy=false",
                          "-netdev", "user,id=n", "-device", "virtio-net-device,netdev=n",
                          "-display", "none", "-serial", "file:" + log, "-monitor", "none",
                          "-kernel", os.path.join(arch, "kernel8.elf")], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        deadline = time.time() + secs
        out = ""
        while time.time() < deadline:
            out = open(log, errors="replace").read() if os.path.exists(log) else ""
            if "tls " in out: break
            time.sleep(0.2)
        time.sleep(0.3)
        return open(log, errors="replace").read() if os.path.exists(log) else ""
    finally:
        q.kill(); q.wait()
try:
    build(cert)
    out = boot("trusted", 60)
    lines = [l for l in out.splitlines() if l.startswith("tls ") or l.startswith("net ")]
    want = "tls 200 %d bytes" % len(REPLY)
    if seen == ["/hello"]: print("  ok: the server got GET /hello through the TLS handshake")
    else: fails.append("the server never saw the request, it saw %r; lines %r" % (seen, lines))
    if want in out: print("  ok: " + want)
    else: fails.append("no %r, lines were %r" % (want, lines))
    seen.clear()
    build("")
    out = boot("untrusted", 60)
    lines = [l for l in out.splitlines() if l.startswith("tls ")]
    if "tls FAIL 62" in out and not seen: print("  ok: without the anchor the kernel refuses the certificate: tls FAIL 62")
    else: fails.append("expected 'tls FAIL 62' and no request, got lines %r, server saw %r" % (lines, seen))
finally:
    server.shutdown()
    shutil.rmtree(tmp, ignore_errors=True)
    subprocess.run(["make", "-C", arch, "clean"], capture_output=True, timeout=60)
for m in fails: print("FAIL: " + m)
if fails: sys.exit(1)
print("PASS: HTTPS on ARM64: a TLS 1.2 page from the host with an embedded anchor, refused without it")
