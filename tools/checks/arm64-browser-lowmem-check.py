#!/usr/bin/env python3
"""7.5.6: the Browser loads a small page when less than 4 MiB of heap is free (arch/arm64/browser.c raw_alloc).

On the 27 inch ASUS a 19 KB page said "browser: out of memory": every fetch asked for one 4 MiB reply buffer, and the
desktop (the magnified dock keeps a copy of its band) had left less than that. raw_alloc now takes the biggest buffer
the heap allows, 4 MiB down to 256 KiB.

The lowmem build fills the heap at each fetch until only 3 to 4 MiB is free, then `browse` fetches a page from a plain
HTTP server on the host through QEMU's user network. Its sentence must reach the UART, with no out-of-memory line.
Discriminating: put back `kmalloc(RAW_MAX)` in show() and it prints "browser: out of memory".

Usage: tools/checks/arm64-browser-lowmem-check.py   (from the repo root)
"""
import http.server, json, os, shutil, socket, subprocess, sys, threading, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from scratch import scratch_dir

root = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
arch = os.path.join(root, "arch", "arm64")
if not all(shutil.which(t) for t in ("clang", "ld.lld", "qemu-system-aarch64")):
    print("SKIP: no aarch64 toolchain"); sys.exit(0)
subprocess.run(["make", "-s", "-C", arch, "lowmemtest"], check=True)

PAGE = b"<html><head><title>Low</title></head><body><p>Small pages load on a full heap.</p></body></html>"
class Handler(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        self.send_response(200); self.send_header("Content-Type", "text/html"); self.send_header("Content-Length", str(len(PAGE)))
        self.end_headers(); self.wfile.write(PAGE)
    def log_message(self, *a): pass
server = http.server.HTTPServer(("127.0.0.1", 0), Handler)
threading.Thread(target=server.serve_forever, daemon=True).start()
port = server.server_address[1]

tmp = scratch_dir("arm64-browser-lowmem")
log, sock = tmp + "/uart", tmp + "/qmp"
q = subprocess.Popen(["qemu-system-aarch64", "-machine", "virt", "-cpu", "cortex-a72", "-m", "256",
                      "-global", "virtio-mmio.force-legacy=false", "-netdev", "user,id=n", "-device", "virtio-net-device,netdev=n",
                      "-device", "virtio-keyboard-device", "-display", "none", "-serial", "file:" + log,
                      "-qmp", "unix:%s,server,nowait" % sock, "-kernel", os.path.join(arch, "lowmem-kernel8.elf")],
                     stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
def uart(): return open(log, errors="replace").read() if os.path.exists(log) else ""
def wait_for(text, secs):
    end = time.time() + secs
    while time.time() < end:
        if text in uart(): return True
        time.sleep(0.1)
    return False
QCODE = {":": ("shift", "semicolon"), "/": ("slash",), ".": ("dot",), " ": ("spc",)}
try:
    if not wait_for("M2 input ready", 60): print("FAIL: did not boot\n" + uart()[-600:]); sys.exit(1)
    time.sleep(0.5)
    s = socket.socket(socket.AF_UNIX); s.settimeout(20); s.connect(sock); f = s.makefile("rw"); f.readline()
    def cmd(c, **a):
        f.write(json.dumps({"execute": c, "arguments": a}) + "\n"); f.flush()
        while True:
            r = json.loads(f.readline())
            if "return" in r or "error" in r: return r
    cmd("qmp_capabilities")
    cmd("send-key", keys=[{"type": "qcode", "data": "f2"}])
    if not wait_for("terminal open", 10): print("FAIL: F2 did not open the Terminal"); sys.exit(1)
    for ch in "browse http://10.0.2.2:%d/low" % port:
        cmd("send-key", keys=[{"type": "qcode", "data": k} for k in QCODE.get(ch, (ch,))]); time.sleep(0.1)
    cmd("send-key", keys=[{"type": "qcode", "data": "ret"}])
    ok = wait_for("Small pages load on a full heap", 60)
    out = uart()
finally:
    q.kill(); q.wait(); server.shutdown()
if "browser: out of memory" in out: print("FAIL: the Browser ran out of memory with 3 to 4 MiB free"); sys.exit(1)
if not ok: print("FAIL: the page never showed\n" + out[-800:]); sys.exit(1)
print("PASS: with less than 4 MiB of heap free the Browser still loads a small page")
