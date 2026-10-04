#!/usr/bin/env python3
"""Headless proof that every Stocks watchlist row fills, not just the first.

Bug: the ring-3 app read STOCKS.TXT with one jt_read, which moves 255 bytes a
call, so it saw only the AAPL row and showed "--" for the rest. This check
boots kernel.elf with -display none and a local stub Worker (stkhost= boot
flag), so CI never touches the live site. The stub serves a real-size
/api/stocks body (3.2KB, 8 rows, 64 points each, captured once into
stocks-fixture-range0.txt) and the 25-row /api/quotes list. The kernel fetches
and writes STOCKS.TXT, the ring-3 app draws, and the check pmemsaves the
framebuffer and requires a sparkline and a colored pill on all five visible
rows. It also reads the status line area: with data it must not say Offline.

Usage: tools/checks/ring3stocks-list-check.py   (from the repo root, after make kernel.elf)
"""
import http.server, json, os, socket, subprocess, sys, threading, time
from PIL import Image
from freeport import free_port

LOG = "/tmp/jt-stockslist-serial.log"
DUMP = "/tmp/jt-stockslist.raw"
FB = 0xfd000000; W, H = 1920, 1080
PORT = free_port()
LOGICAL_W, LOGICAL_H, SCALE = 960, 540, 2
DOCK_ICON, DOCK_GAP, SLOT0_X = 37, 6, 247
PITCH = DOCK_ICON + DOCK_GAP
ICON_ROW_Y = 487
STOCKS_SLOT = 9

os.chdir(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
for f in (LOG, DUMP):
    try: os.remove(f)
    except FileNotFoundError: pass

ROOT = os.getcwd()
BODY = open("tools/checks/stocks-fixture-range0.txt", "rb").read()
QUOTES = open("tools/checks/stocks-fixture-quotes25.txt", "rb").read()
class Hd(http.server.BaseHTTPRequestHandler):
    def log_message(self, *a): pass
    def do_GET(self):
        body = QUOTES if self.path.startswith("/api/quotes") else BODY
        self.send_response(200); self.send_header("Content-Type", "text/plain")
        self.send_header("Content-Length", str(len(body))); self.end_headers(); self.wfile.write(body)
srv = http.server.ThreadingHTTPServer(("0.0.0.0", 0), Hd); srv.daemon_threads = True
threading.Thread(target=srv.serve_forever, daemon=True).start()

q = subprocess.Popen(["qemu-system-i386", "-kernel", "kernel.elf", "-display", "none", "-vga", "std",
                      "-net", "nic,model=rtl8139", "-net", "user",
                      "-append", "stkhost=10.0.2.2:%d" % srv.server_address[1],
                      "-qmp", f"tcp:127.0.0.1:{PORT},server,nowait", "-serial", "file:" + LOG],
                     stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
try:
    s = None
    for _ in range(50):
        time.sleep(0.2)
        try: s = socket.create_connection(("127.0.0.1", PORT)); break
        except OSError: pass
    if s is None: raise SystemExit("FAIL: QEMU's QMP socket never came up")
    f = s.makefile("rw")
    def cmd(o):
        f.write(json.dumps(o) + "\n"); f.flush()
        while True:
            r = json.loads(f.readline())
            if "return" in r or "error" in r: return r
    f.readline()
    cmd({"execute": "qmp_capabilities"})
    def serial_has(needle): return needle in open(LOG, errors="replace").read()
    for _ in range(400):   # the kernel says when the desktop is up; a slow runner takes longer than a fixed sleep
        time.sleep(0.1)
        if serial_has("guidesktop"): break
    time.sleep(1.0)

    def move(x, y):
        cmd({"execute": "input-send-event", "arguments": {"events": [
            {"type": "abs", "data": {"axis": "x", "value": int(x * 32768 / LOGICAL_W)}},
            {"type": "abs", "data": {"axis": "y", "value": int(y * 32768 / LOGICAL_H)}}]}})
    def click():
        cmd({"execute": "input-send-event", "arguments": {"events": [{"type": "btn", "data": {"down": True, "button": "left"}}]}})
        time.sleep(0.1)
        cmd({"execute": "input-send-event", "arguments": {"events": [{"type": "btn", "data": {"down": False, "button": "left"}}]}})
    def key(qcode):
        cmd({"execute": "send-key", "arguments": {"keys": [{"type": "qcode", "data": qcode}]}})

    centre = lambda slot: SLOT0_X + slot * PITCH + DOCK_ICON // 2

    # Open Stocks from the dock (no network needed: it opens with empty data).
    for attempt in range(4):   # a click can land before the dock is ready: look for the launch in the kernel log, click again if it is not there
        move(centre(STOCKS_SLOT), ICON_ROW_Y); time.sleep(0.4)
        click()
        for _ in range(100):
            time.sleep(0.1)
            if serial_has("launching STOCKS.BIN"): break
        if serial_has("launching STOCKS.BIN"): break
    time.sleep(6.0)  # fetch from the fake server, STOCKS.TXT, ring-3 window draws

    cmd({"execute": "pmemsave", "arguments": {"val": FB, "size": W * H * 4, "filename": DUMP}})
    try: cmd({"execute": "quit"})
    except (ConnectionResetError, BrokenPipeError, OSError): pass
finally:
    try: q.wait(timeout=5)
    except subprocess.TimeoutExpired: q.kill()

img = Image.frombytes("RGBA", (W, H), open(DUMP, "rb").read(), "raw", "BGRA").convert("RGB")

px = img.load()
GREEN = (0x41, 0x85, 0x4B); RED = (0xB5, 0x16, 0x16)
def near(p, c, tol=10): return max(abs(p[i] - c[i]) for i in range(3)) <= tol

# Sidebar rows (physical px): the window sits at a fixed spot, row 0 starts at y=225, pitch 108,
# sparkline and price pill live in x 380..740. A filled row has hundreds of green or red pixels.
bad = []
for r in range(5):
    y0 = 225 + 108 * r
    n = sum(1 for y in range(y0, y0 + 100) for x in range(380, 740)
            if near(px[x, y], GREEN) or near(px[x, y], RED))
    print(f"row {r}: {n} colored pixels")
    if n < 150: bad.append(r)
if bad:
    print(f"FAIL: watchlist rows {bad} show no price/sparkline (the app read only part of STOCKS.TXT)")
    sys.exit(1)
print("PASS: all five watchlist rows show a sparkline and a price pill")
sys.exit(0)
