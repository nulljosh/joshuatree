#!/usr/bin/env python3
"""ARM64 HTTPS: the aarch64 kernel fetches a page over TLS 1.2 (arch/arm64/tls.c, BearSSL) from a server on the host.

Makes a throwaway CA and signed certificate for 10.0.2.2 (QEMU's user-mode address for the host), starts a Python HTTPS
server on it, builds arch/arm64 with TLSPORT set to that port and TLS_TA pointing at the CA, and boots on
QEMU's user network. Checks:
  - the server saw the GET, so the handshake really finished (the server would refuse a bad one);
  - "tls 200 N bytes", N being the exact length of the body it sent;
  - wrong-name, expired and not-yet-valid certificates are refused before HTTP data;
  - a second build WITHOUT the throwaway certificate in the trust anchors prints "tls FAIL 62" (BearSSL's
    "certificate not trusted") and the server never sees a request: the trust check is real, not decoration.
Skips (exit 0) when clang's aarch64 target, ld.lld, openssl or qemu-system-aarch64 is missing.
Usage: tools/checks/arm64-tls-check.py   (from the repo root)
"""
import datetime, http.server, os, shutil, ssl, subprocess, sys, tempfile, threading, time

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
ca, cakey = tmp + "/ca.pem", tmp + "/ca.key"
cert, key, csr = tmp + "/cert.pem", tmp + "/key.pem", tmp + "/leaf.csr"
def openssl(*args):
    subprocess.run(["openssl", *args], check=True, capture_output=True)
openssl("req", "-x509", "-newkey", "rsa:2048", "-nodes", "-days", "4", "-subj", "/CN=JT test CA",
        "-addext", "basicConstraints=critical,CA:TRUE", "-keyout", cakey, "-out", ca)
openssl("req", "-new", "-newkey", "rsa:2048", "-nodes", "-subj", "/CN=10.0.2.2", "-keyout", key, "-out", csr)
open(tmp + "/index", "w").close(); open(tmp + "/serial", "w").write("01\n")
now = datetime.datetime.now(datetime.timezone.utc)
def leaf(host, before, after):
    config = tmp + "/ca.cnf"
    open(config, "w").write(f"""[ca]
default_ca=issuer
[issuer]
database={tmp}/index
serial={tmp}/serial
new_certs_dir={tmp}
certificate={ca}
private_key={cakey}
default_md=sha256
policy=names
unique_subject=no
x509_extensions=leaf
[names]
commonName=supplied
[leaf]
basicConstraints=critical,CA:FALSE
keyUsage=critical,digitalSignature,keyEncipherment
extendedKeyUsage=serverAuth
subjectAltName=DNS:{host}
""")
    fmt = lambda days: (now + datetime.timedelta(days=days)).strftime("%Y%m%d%H%M%SZ")
    openssl("ca", "-batch", "-notext", "-config", config, "-in", csr, "-out", cert,
            "-startdate", fmt(before), "-enddate", fmt(after))
leaf("10.0.2.2", -1, 2)
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
    build(ca)
    out = boot("trusted", 60)
    lines = [l for l in out.splitlines() if l.startswith("tls ") or l.startswith("net ")]
    want = "tls 200 %d bytes" % len(REPLY)
    if seen == ["/hello"]: print("  ok: the server got GET /hello through the TLS handshake")
    else: fails.append("the server never saw the request, it saw %r; lines %r" % (seen, lines))
    if want in out: print("  ok: " + want)
    else: fails.append("no %r, lines were %r" % (want, lines))
    for name, host, before, after, error in (("hostname", "wrong.example", -1, 2, 56),
                                            ("expired", "10.0.2.2", -3, -2, 54),
                                            ("future", "10.0.2.2", 1, 2, 54)):
        seen.clear(); leaf(host, before, after); ctx.load_cert_chain(cert, key)
        out = boot(name, 60)
        if "tls FAIL %d" % error in out and not seen:
            print("  ok: %s certificate refused before HTTP data" % name)
        else: fails.append("%s: expected error %d, got %r, server saw %r" % (name, error, out[-500:], seen))
    leaf("10.0.2.2", -1, 2); ctx.load_cert_chain(cert, key)
    seen.clear()
    # Replace only virt's seed provider: failed entropy must stop before any HTTP request.
    bad_rng = tmp + "/no-entropy.c"
    open(bad_rng, "w").write("int tls_entropy(unsigned char *p, unsigned n) {(void)p;(void)n;return 0;}\n")
    subprocess.run(["clang", "-target", "aarch64-none-elf", "-ffreestanding", "-c", bad_rng,
                    "-o", os.path.join(arch, "rng.o")], check=True)
    subprocess.run(["make", "-C", arch, "kernel8.elf"], check=True, capture_output=True, timeout=600)
    seen.clear()
    out = boot("entropy-failure", 60)
    if "tls FAIL -4" in out and not seen: print("  ok: failed entropy refuses TLS before HTTP")
    else: fails.append("entropy failure did not fail closed: " + repr(out[-1000:]))
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
