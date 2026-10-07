#!/usr/bin/env python3
"""ARM64 network stack: the aarch64 kernel leases an address over DHCP and POSTs to the host, through the same
drivers/net.c and drivers/http.c the i386 kernel uses, sitting on QEMU's virtio-net card (drivers/nic.h is the seam).

Starts a throwaway HTTP server on a free localhost port, builds arch/arm64 with NETPORT set to it, and boots the kernel
on QEMU's user-mode network, where the guest reaches the host at 10.0.2.2 (no hostfwd, no guestfwd). Checks:
  - "net dhcp 10.0.2.15 gw 10.0.2.2": printed only when the stack really parsed a lease out of QEMU's DHCP server
    (a broken parse falls back to the fixed address, prints "net dhcp FAIL", and this check fails);
  - the server got the kernel's POST body, byte for byte;
  - "net http 200 N bytes", N being the exact length of the body the server sent back.
Skips (exit 0) when clang's aarch64 target, ld.lld or qemu-system-aarch64 is missing.
Usage: tools/checks/arm64-net-check.py   (from the repo root)
"""
import http.server, os, shutil, subprocess, sys, tempfile, threading, time

root = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..")
arch = os.path.join(root, "arch/arm64")
if not all(shutil.which(t) for t in ("clang", "ld.lld", "qemu-system-aarch64")):
    print("SKIP: clang, ld.lld or qemu-system-aarch64 not installed"); sys.exit(0)

REPLY = b"hello from the host, joshua tree arm64 reads this\n" * 3
WANT_BODY = b'{"from":"joshua tree arm64"}'
posted = []
class Handler(http.server.BaseHTTPRequestHandler):
    def do_POST(self):
        posted.append((self.path, self.rfile.read(int(self.headers.get("Content-Length", 0)))))
        self.send_response(200)
        self.send_header("Content-Length", str(len(REPLY)))
        self.end_headers()
        self.wfile.write(REPLY)
    def log_message(self, *a): pass
server = http.server.HTTPServer(("127.0.0.1", 0), Handler)
port = server.server_address[1]
threading.Thread(target=server.serve_forever, daemon=True).start()

fails = []
tmp = tempfile.mkdtemp()
log = tmp + "/uart"
def uart(): return open(log, errors="replace").read() if os.path.exists(log) else ""
q = None
try:
    subprocess.run(["make", "-C", arch, "clean"], capture_output=True, timeout=60)   # NETPORT is baked in: no stale objects
    if subprocess.run(["make", "-C", arch, "NETPORT=%d" % port], capture_output=True, timeout=300).returncode:
        print("FAIL: arch/arm64 `make NETPORT=%d` does not build" % port); sys.exit(1)
    q = subprocess.Popen(["qemu-system-aarch64", "-machine", "virt", "-cpu", "cortex-a72", "-m", "256",
                          "-global", "virtio-mmio.force-legacy=false",
                          "-netdev", "user,id=n", "-device", "virtio-net-device,netdev=n",
                          "-display", "none", "-serial", "file:" + log, "-monitor", "none",
                          "-kernel", os.path.join(arch, "kernel8.elf")], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    want_http = "net http 200 %d bytes" % len(REPLY)
    deadline = time.time() + 40
    while time.time() < deadline and "net http " not in uart(): time.sleep(0.2)
    time.sleep(0.3)
    out = uart()
    lines = [l for l in out.splitlines() if l.startswith("net ")]
    if "net dhcp 10.0.2.15 gw 10.0.2.2" in out: print("  ok: DHCP lease parsed: net dhcp 10.0.2.15 gw 10.0.2.2")
    else: fails.append("no DHCP lease line, net lines were %r" % lines)
    if posted and posted[0] == ("/jt", WANT_BODY): print("  ok: the host server got POST /jt with %r" % WANT_BODY.decode())
    else: fails.append("the host server never got the POST body, it saw %r" % posted)
    if want_http in out: print("  ok: reply read back: " + want_http)
    else: fails.append("no %r, net lines were %r" % (want_http, lines))
    for l in lines:
        if len(l) > 53: fails.append("%r is %d columns, the Pi console fits 53" % (l, len(l)))
finally:
    if q: q.kill(); q.wait()
    server.shutdown()
    shutil.rmtree(tmp, ignore_errors=True)
    subprocess.run(["make", "-C", arch, "clean"], capture_output=True, timeout=60)
for m in fails: print("FAIL: " + m)
if fails: sys.exit(1)
print("PASS: the shared IP stack runs on ARM64: a DHCP lease from QEMU, then an HTTP POST to the host and its 200 reply")
