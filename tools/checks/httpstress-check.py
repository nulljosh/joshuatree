#!/usr/bin/env python3
"""HTTP stress: a ring-3 app can make 72 sequential SYS_HTTP_GET calls and every one succeeds.

Headless only (-display none, never a window). Ring-3 Samantha loads her 72 face frames one
fetch per idle poll through SYS_HTTP_GET (the 64 KB caller-buffer path) from a loopback stub
at facehost=. Before 2.0.0 the desktop's Weather fetch (geocode, ip-api, forecast: each
able to sit out a 20 s wait) held net.c's one connection from about the 23rd frame on, the
guard in http.c turned that into -EIO, and every fetch after it failed the same way for the
rest of the session. A ring-3 fetch now queues behind the desktop for up to 3 s and, past
that, says -EBUSY (which the face loader retries without spending a skip) instead of
reporting a network error.

The `httpstat` boot flag makes the kernel print one census line per ring-3 fetch:
n=<bytes or -errno> free=<free heap bytes> largest=<biggest free block> blocks=<count>.

Asserts: all 72 frames were fetched by the stub (so none were skipped), her face line reads
idle=24 talk=48 end=24/48 skipped=0, no fetch came back -5 (EIO), the biggest free heap
block never drops below the 66 KB a face reply needs once the heap is warm, and the heap's free
bytes hold flat across the last 20 fetches (a leak per call would show as free bytes falling
call after call; other subsystems move it between phases, so the whole run is not compared).
"""
import http.server, io, os, re, subprocess, sys, tempfile, threading, time
from PIL import Image
from freeport import free_port

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
os.chdir(ROOT)
subprocess.run(["make", "-s", "kernel.elf"], check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

QMP_PORT = free_port()


def jpg(color):
    b = io.BytesIO(); Image.new("RGB", (320, 320), color).save(b, "JPEG", quality=90); return b.getvalue()


FRAMES = {f"/face/idle-{i}.jpg": jpg((220, 30, 30)) for i in range(24)}
FRAMES.update({f"/face/talk-{i}.jpg": jpg((30, 200, 30) if i % 2 == 0 else (30, 30, 220)) for i in range(48)})
seen = set()


class Stub(http.server.BaseHTTPRequestHandler):
    def log_message(self, *a): pass

    def do_GET(self):
        body = FRAMES.get(self.path)
        if body is None:
            self.send_response(404); self.send_header("Content-Length", "0"); self.end_headers(); return
        seen.add(self.path)
        self.send_response(200); self.send_header("Content-Type", "image/jpeg")
        self.send_header("Content-Length", str(len(body))); self.end_headers(); self.wfile.write(body)


srv = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Stub); srv.daemon_threads = True
threading.Thread(target=srv.serve_forever, daemon=True).start()
port = srv.server_address[1]

work = tempfile.mkdtemp(prefix="jt-httpstress-")
log = os.path.join(work, "serial.txt")
q = subprocess.Popen(["qemu-system-i386", "-kernel", "kernel.elf", "-display", "none", "-vga", "std", "-no-reboot",
                      "-qmp", f"tcp:127.0.0.1:{QMP_PORT},server,nowait", "-serial", "file:" + log,
                      "-net", "nic,model=rtl8139", "-net", "user",
                      "-append", f"samantha httpstat facehost=10.0.2.2:{port}"],
                     stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def serial():
    try: return open(log, encoding="latin-1").read()
    except FileNotFoundError: return ""


fails = []
try:
    done = False
    for _ in range(600):
        time.sleep(0.3)
        if re.search(r"face: idle=\d+ talk=\d+ end=\d+/\d+", serial()): done = True; break
finally:
    q.kill(); q.wait()

s = serial()
stats = [(int(m.group(1)), int(m.group(2)), int(m.group(3))) for m in re.finditer(r"httpstat: n=(-?\d+) free=(\d+) largest=(\d+) blocks=\d+", s)]
face = re.search(r"face: idle=(\d+) talk=(\d+) end=(\d+)/(\d+) skipped=(\d+) retried=(\d+)", s)
print("httpstress: " + (face.group(0) if face else "(no face: line)") + f", {len(stats)} fetches, stub served {len(seen)} of {len(FRAMES)} frames")
if not done: fails.append("her face never finished loading")
if len(seen) != len(FRAMES): fails.append(f"the stub was only asked for {len(seen)} of {len(FRAMES)} frames: fetches stopped succeeding")
if face and (face.group(1), face.group(2), face.group(5)) != ("24", "48", "0"):
    fails.append(f"expected idle=24 talk=48 skipped=0, got {face.group(0)}")
if any(n == -5 for n, _, _ in stats): fails.append(f"{sum(1 for n, _, _ in stats if n == -5)} fetches came back -EIO (-5)")
if stats:
    ok = [(fb, lg) for n, fb, lg in stats if n > 0]
    warm = ok[8:]   # the first few fetches grow the heap to fit their reply buffers
    if warm and min(lg for _, lg in warm) < 66 * 1024: fails.append(f"largest free heap block fell to {min(lg for _, lg in warm)} bytes, under a face reply buffer")
    tail = [fb for fb, _ in ok[-20:]]
    if len(tail) == 20 and max(tail) - min(tail) > 4096: fails.append(f"heap free bytes drift across the last 20 fetches ({min(tail)} to {max(tail)}): a buffer leaks per call")
else: fails.append("no httpstat lines in the serial log")
if "kheap: CORRUPT" in s: fails.append("kheap reported corruption")
for f in fails: print("FAIL: " + f)
if not fails: print("PASS: 72 sequential ring-3 GETs succeed, heap free stays at baseline")
sys.exit(1 if fails else 0)
