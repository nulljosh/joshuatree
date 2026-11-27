#!/usr/bin/env python3
"""The README's screenshot: the Pi desktop on its own park painting with Weather open, taken headless.

One boot of the ARM kernel on QEMU's virt machine (ramfb, virtio keyboard and tablet), built with PARK_WALL so it shows
the Pi's wallpaper rather than the checks' flat blue, and with feedsfix so Weather reads a fixed reading from this
script's own server (illustration data, as the landing clips use), not the live feed. It closes the boot Console,
opens Weather from the dock, parks the pointer, takes a QMP screendump and writes docs/brand/readme-weather.png with
the hero card's rounded corners. Never opens a window.
Usage: python3 tools/gen/readme-shots.py   (from anywhere; needs clang's aarch64 target, ld.lld, qemu-system-aarch64, Pillow)
"""
import http.server, json, os, shutil, socket, subprocess, sys, tempfile, threading, time
from PIL import Image, ImageDraw

ROOT = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
ARCH = os.path.join(ROOT, "arch", "arm64")
OUT = os.path.join(ROOT, "docs", "brand", "readme-weather.png")
W, H, R = 800, 600, 14
ICON = max(16, min(H * 7 // 100, (min(740, W - 40) - 2 * 10 - 11 * 6) // 12))
DX0, DY0 = (W - (12 * ICON + 11 * 6 + 2 * 10)) // 2, H - ICON - 2 * 10 - 24
def slot(i): return DX0 + 10 + i * (ICON + 6) + ICON // 2, DY0 + 10 + ICON // 2
READING = b"city Vancouver, BC\nhave 1\ntemp 14\ncode 2\nd 0 2 15 8\nd 1 0 16 9\nd 2 3 13 7\nd 3 61 12 7\nd 4 2 14 8\n"

class Reading(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        self.send_response(200); self.send_header("Content-Type", "text/plain"); self.send_header("Content-Length", str(len(READING)))
        self.end_headers(); self.wfile.write(READING)
    def log_message(self, *a): pass

def main():
    tmp = tempfile.mkdtemp(prefix="jt-readme-shots-")
    log, sock, shot = tmp + "/uart", tmp + "/qmp", tmp + "/shot.ppm"
    server = http.server.HTTPServer(("127.0.0.1", 18471), Reading)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    subprocess.run(["make", "-s", "-C", ARCH, "clean"], capture_output=True)
    q = None
    try:
        subprocess.run(["make", "-s", "-C", ARCH, "feedsfixtest", "KFLAGS=-DPARK_WALL"], check=True, capture_output=True)
        q = subprocess.Popen(["qemu-system-aarch64", "-machine", "virt", "-cpu", "cortex-a72", "-m", "256",
                              "-netdev", "user,id=n", "-device", "virtio-net-device,netdev=n",
                              "-global", "virtio-mmio.force-legacy=false", "-device", "ramfb",
                              "-device", "virtio-keyboard-device", "-device", "virtio-tablet-device", "-display", "none",
                              "-serial", "file:" + log, "-qmp", "unix:%s,server,nowait" % sock,
                              "-kernel", os.path.join(ARCH, "feedsfix-kernel8.elf")], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        def uart(): return open(log, errors="replace").read() if os.path.exists(log) else ""
        def wait_for(text, secs, after=0):
            end = time.time() + secs
            while time.time() < end:
                if text in uart()[after:]: return True
                time.sleep(0.05)
            return False
        if not (wait_for("M2 input ready", 60) and wait_for("net dhcp ", 30)): sys.exit("readme-shots: the desktop did not boot")
        s = socket.socket(socket.AF_UNIX); s.settimeout(20); s.connect(sock); f = s.makefile("rw"); f.readline()
        def cmd(c, **a):
            f.write(json.dumps({"execute": c, "arguments": a}) + "\n"); f.flush()
            while True:
                r = json.loads(f.readline())
                if "return" in r or "error" in r: return r
        cmd("qmp_capabilities"); time.sleep(0.5)
        def point(x, y):
            cmd("input-send-event", events=[{"type": "abs", "data": {"axis": "x", "value": x * 32767 // (W - 1)}},
                                            {"type": "abs", "data": {"axis": "y", "value": y * 32767 // (H - 1)}}]); time.sleep(0.3)
        def click(x, y):
            point(x, y)
            for down in (True, False): cmd("input-send-event", events=[{"type": "btn", "data": {"down": down, "button": "left"}}])
            time.sleep(0.3)
        n = len(uart()); click((W - 500) // 2 + 24, 100 + 16)                   # the boot Console's red dot
        if not wait_for("console closed", 6, n): sys.exit("readme-shots: the boot Console did not close")
        n = len(uart()); click(*slot(8))                                          # Weather
        if not wait_for("weather: temp", 15, n): sys.exit("readme-shots: Weather did not open: %r" % uart()[n:][-300:])
        point(W - 1, H - 1); time.sleep(1.5)                                       # the pointer out of the picture, the tooltip gone
        cmd("screendump", filename=shot)
        for _ in range(30):
            if os.path.exists(shot) and os.path.getsize(shot) >= W * H * 3: break
            time.sleep(0.1)
        if "KERNEL CRASH" in uart(): sys.exit("readme-shots: the kernel crashed")
    finally:
        if q:
            q.terminate()
            try: q.wait(5)
            except subprocess.TimeoutExpired: q.kill()
        server.shutdown()
        subprocess.run(["make", "-s", "-C", ARCH, "clean"], capture_output=True)   # PARK_WALL objects must not outlive this: the checks read blue
    im = Image.open(shot).convert("RGB")
    mask = Image.new("L", (W * 4, H * 4), 0)
    ImageDraw.Draw(mask).rounded_rectangle((0, 0, W * 4 - 1, H * 4 - 1), R * 4, fill=255)
    im.putalpha(mask.resize((W, H), Image.LANCZOS))
    a = im.getchannel("A")
    im = im.convert("RGB").quantize(256, method=Image.Quantize.MEDIANCUT, dither=Image.Dither.NONE).convert("RGBA"); im.putalpha(a)
    out = sys.argv[1] if len(sys.argv) > 1 else OUT
    im.save(out, optimize=True)
    shutil.rmtree(tmp, ignore_errors=True)
    print("saved %s %s %d bytes" % (os.path.relpath(out, ROOT), im.size, os.path.getsize(out)))

if __name__ == "__main__":
    main()
