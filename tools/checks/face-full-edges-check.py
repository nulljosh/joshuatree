#!/usr/bin/env python3
"""Portfolio face: the wall beside his face is clean: flat above his shoulders, his sweater carried sideways below.

Headless only (-display none, never a window). Boots "portfolio samantha"
with facehost= at a loopback stub serving /face-joshua/ frames whose left
edge is the worst case: source column 0..1 stripes black and white every 8
rows, and the bottom-left corner is a dark block (his sweater reaching the
edge). face_blit_full (kernel/chat_face.h) paints the side from a smoothed
per-row color: the wall rows must come out flat (the stripes averaged away),
and the sweater rows must carry on dark, with no light seam in between.

Discriminating: the old fill took each row's own edge pixel, so the stripes
became stripes across the side and the dark corner a dark bar; the spread
assertion fails by name with either.
"""
import http.server, io, os, socket, subprocess, sys, tempfile, threading, time, json
from PIL import Image, ImageDraw
from freeport import free_port

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
os.chdir(ROOT)
subprocess.run(["make", "-s", "kernel.elf"], check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

FB = 0xfd000000; W, H = 1920, 1080; QMP_PORT = free_port()
WALL = (214, 204, 184)


def frame():
    im = Image.new("RGB", (320, 320), WALL); d = ImageDraw.Draw(im)
    for y in range(0, 320, 16): d.rectangle([0, y, 1, y + 7], fill=(0, 0, 0)); d.rectangle([0, y + 8, 1, y + 15], fill=(255, 255, 255))
    d.rectangle([0, 250, 40, 319], fill=(20, 20, 30))
    d.ellipse([110, 60, 210, 200], fill=(200, 150, 120))   # a face, so it is not all wall
    b = io.BytesIO(); im.save(b, "JPEG", quality=95); return b.getvalue()


F = frame()
FRAMES = {f"/face-joshua/{k}-{i}.jpg": F for k in ("idle", "talk") for i in range(4)}


class Stub(http.server.BaseHTTPRequestHandler):
    def log_message(self, *a): pass

    def do_GET(self):
        body = FRAMES.get(self.path)
        self.send_response(200 if body else 404); self.send_header("Content-Type", "image/jpeg" if body else "text/plain")
        body = body or b"not found"
        self.send_header("Content-Length", str(len(body))); self.end_headers(); self.wfile.write(body)


srv = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Stub); srv.daemon_threads = True
threading.Thread(target=srv.serve_forever, daemon=True).start()
port = srv.server_address[1]
work = tempfile.mkdtemp(prefix="jt-facefull-")
log, dump_path = os.path.join(work, "serial.txt"), os.path.join(work, "fb.raw")
q = subprocess.Popen(["qemu-system-i386", "-kernel", "kernel.elf", "-display", "none", "-vga", "std", "-no-reboot",
                      "-qmp", f"tcp:127.0.0.1:{QMP_PORT},server,nowait", "-serial", "file:" + log,
                      "-net", "nic,model=rtl8139", "-net", "user",
                      "-append", f"portfolio samantha facehost=10.0.2.2:{port}"],
                     stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
fails = []
try:
    s = None
    for _ in range(50):
        time.sleep(0.2)
        try: s = socket.create_connection(("127.0.0.1", QMP_PORT)); break
        except OSError: pass
    if s is None: sys.exit("FAIL: no QMP")
    f = s.makefile("rw")

    def cmd(o):
        f.write(json.dumps(o) + "\n"); f.flush()
        while True:
            r = json.loads(f.readline())
            if "return" in r or "error" in r: return r
    f.readline(); cmd({"execute": "qmp_capabilities"})

    def serial():
        try: return open(log, encoding="latin-1").read()
        except FileNotFoundError: return ""

    for _ in range(300):
        if "face: idle=" in serial(): break
        time.sleep(0.2)
    lines = [l[l.index("face: "):] for l in serial().splitlines() if "face: idle=" in l]
    print("serial: " + (lines[-1] if lines else "(no face: line)"))
    if not any(l.startswith("face: idle=4 talk=4") for l in lines): fails.append(f"frames did not load (got {lines})")
    time.sleep(3)
    cmd({"execute": "pmemsave", "arguments": {"val": FB, "size": W * H * 4, "filename": dump_path}})
    img = Image.frombytes("RGBA", (W, H), open(dump_path, "rb").read(), "raw", "BGRA").convert("RGB")
    # Left of the frame (it starts past x=400 at 1920x1080), above the glass bar.
    px = [img.getpixel((x, y)) for x in range(10, 300, 20) for y in range(120, 740, 4)]
    spread = max(max(p[i] for p in px) - min(p[i] for p in px) for i in range(3))
    print(f"side wall: {px[0]}, spread {spread}")
    if spread > 8: fails.append(f"the wall beside the face is not one color: channel spread {spread} (stripes or a dark bar)")
    if max(abs(px[0][i] - WALL[i]) for i in range(3)) > 40: fails.append(f"side is not wall-colored: {px[0]}")
    low = img.getpixel((100, 880))
    print(f"side below the shoulders: {low}")
    if max(low) > 90: fails.append(f"the sweater does not carry on beside the face: {low}")
finally:
    q.kill(); srv.shutdown()

if fails:
    for m in fails: print("FAIL: " + m)
    sys.exit(1)
print("PASS: portfolio face side wall is one clean color")
