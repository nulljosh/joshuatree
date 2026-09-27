#!/usr/bin/env python3
"""Chat face: Samantha's face shows in the Chat window and moves while she talks.

Headless only (-display none, never a window). A loopback stub stands in
for both hosts through QEMU's user NAT at 10.0.2.2: facehost= serves the
face frames, llmhost= answers /api/chat and /api/speak. The frames are
solid colors so the screen can be read without guessing: idle is red, talk
frames alternate green and blue, and talk-7 is garbage bytes (a bad frame
must be skipped, not crash anything).

Scenario "face": open Chat from the dock, assert serial says
"face: idle=4 talk=7" and the face square is red. Send a message; the stub
replies and hands back 3s of tone. While it plays, the square must show
green and blue; once it ends, red again.

Scenario "noface": facehost= points at a closed port. Chat must still
answer, serial says "face: idle=0 talk=0", and the square stays plain
background (no red).

Discriminating: without the sb16 progress hook the square stays red the
whole reply and the talk-color assertion fails by name; without the fetch
it never turns red at all.
"""
import http.server, io, json, math, os, socket, subprocess, sys, tempfile, threading, time
from PIL import Image

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
os.chdir(ROOT)
subprocess.run(["make", "-s", "kernel.elf"], check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

FB = 0xfd000000; W, H = 1920, 1080; QMP_PORT = 4471
LOGICAL_W, LOGICAL_H, SCALE = 960, 540, 2
DOCK_ICON, DOCK_GAP, SLOT0_X = 37, 6, 247; PITCH = DOCK_ICON + DOCK_GAP; ICON_ROW_Y = 487
CLOSE_X, CLOSE_Y = 94, 56; CLOSE_RED = (0xFF, 0x5F, 0x57)
VX, VY, VW = 78, 72, 804
FACE_CX, FACE_CY = VX + VW - 20 - 30, VY + (-32 + 44) + 30   # centre of the 60x60 face square
IDLE, GREEN, BLUE = (220, 30, 30), (30, 200, 30), (30, 30, 220)
QUESTION = "hello there"
REPLY = "Hi, it is lovely to see you."
SECS = 3.0
TONE = bytes(int(128 + 100 * math.sin(2 * math.pi * 440 * i / 16000)) for i in range(int(16000 * SECS)))


def png(color):
    b = io.BytesIO(); Image.new("RGB", (120, 120), color).save(b, "PNG"); return b.getvalue()


FRAMES = {f"/face/idle-{i}.png": png(IDLE) for i in range(4)}
FRAMES.update({f"/face/talk-{i}.png": png(GREEN if i % 2 == 0 else BLUE) for i in range(7)})
FRAMES["/face/talk-7.png"] = b"\x89PNG\r\n\x1a\nnot really a png"


class Stub(http.server.BaseHTTPRequestHandler):
    def log_message(self, *a): pass

    def reply(self, code, body, ctype):
        self.send_response(code); self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body))); self.end_headers(); self.wfile.write(body)

    def do_GET(self):
        body = FRAMES.get(self.path)
        if body is None: self.reply(404, b"not found", "text/plain")
        else: self.reply(200, body, "image/png")

    def do_POST(self):
        self.rfile.read(int(self.headers.get("Content-Length", "0")))
        if self.path == "/api/chat":
            self.reply(200, json.dumps({"model": "samantha", "message": {"role": "assistant", "content": REPLY}, "done": True}).encode(), "application/json")
        elif self.path == "/api/speak":
            self.reply(200, TONE, "application/octet-stream")
        else:
            self.reply(404, b"not found", "text/plain")


srv = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Stub); srv.daemon_threads = True
threading.Thread(target=srv.serve_forever, daemon=True).start()
port = srv.server_address[1]
fails = []


def near(p, c, tol=24): return max(abs(p[i] - c[i]) for i in range(3)) <= tol


def name(p):
    for n, c in (("idle", IDLE), ("green", GREEN), ("blue", BLUE)):
        if near(p, c): return n
    return "other"


def run(scenario, facehost):
    work = tempfile.mkdtemp(prefix="jt-face-" + scenario + "-")
    log, dump_path = os.path.join(work, "serial.txt"), os.path.join(work, "fb.raw")
    q = subprocess.Popen(["qemu-system-i386", "-kernel", "kernel.elf", "-display", "none", "-vga", "std",
                          "-qmp", f"tcp:127.0.0.1:{QMP_PORT},server,nowait", "-serial", "file:" + log,
                          "-net", "nic,model=rtl8139", "-net", "user",
                          "-audiodev", f"wav,id=snd,path={os.path.join(work, 'out.wav')}", "-device", "sb16,audiodev=snd",
                          "-append", f"llmhost=10.0.2.2 llmport={port} facehost={facehost}"],
                         stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    tag = scenario + ": "
    try:
        s = None
        for _ in range(50):
            time.sleep(0.2)
            try: s = socket.create_connection(("127.0.0.1", QMP_PORT)); break
            except OSError: pass
        if s is None: fails.append(tag + "no QMP"); return
        f = s.makefile("rw")

        def cmd(o):
            f.write(json.dumps(o) + "\n"); f.flush()
            while True:
                r = json.loads(f.readline())
                if "return" in r or "error" in r: return r
        f.readline(); cmd({"execute": "qmp_capabilities"})

        def move(x, y):
            cmd({"execute": "input-send-event", "arguments": {"events": [
                {"type": "abs", "data": {"axis": "x", "value": int(x * 32768 / LOGICAL_W)}},
                {"type": "abs", "data": {"axis": "y", "value": int(y * 32768 / LOGICAL_H)}}]}})

        def click():
            for down in (True, False):
                cmd({"execute": "input-send-event", "arguments": {"events": [{"type": "btn", "data": {"down": down, "button": "left"}}]}})
                time.sleep(0.1)

        def dump():
            cmd({"execute": "pmemsave", "arguments": {"val": FB, "size": W * H * 4, "filename": dump_path}})
            return Image.frombytes("RGBA", (W, H), open(dump_path, "rb").read(), "raw", "BGRA").convert("RGB")

        def pixel(img, x, y): return img.getpixel((x * SCALE + 1, y * SCALE + 1))

        def face(): return pixel(dump(), FACE_CX, FACE_CY)

        def keys(k): cmd({"execute": "send-key", "arguments": {"keys": [{"type": "qcode", "data": k}]}})

        def serial():
            try: return open(log, encoding="latin-1").read()
            except FileNotFoundError: return ""

        for _ in range(120):
            if pixel(dump(), 480, 511) == (0xEF, 0xEB, 0xE4): break
            time.sleep(0.25)
        else: fails.append(tag + "desktop never appeared"); return
        time.sleep(0.5)
        move(SLOT0_X + 7 * PITCH + DOCK_ICON // 2, ICON_ROW_Y); time.sleep(0.3); click()
        for _ in range(300):
            time.sleep(0.2)
            if "face: " in serial(): break
        time.sleep(1.0)
        lines = [l for l in serial().splitlines() if l.startswith("face: ")]
        print(tag + (lines[-1] if lines else "(no face: line)"))
        want = "face: idle=4 talk=7" if scenario == "face" else "face: idle=0 talk=0"
        if want not in lines: fails.append(tag + f"serial did not say '{want}' (got {lines})")
        before = face()
        print(tag + f"face square before sending: {before} ({name(before)})")
        if scenario == "face" and name(before) != "idle": fails.append(tag + f"face square is not the idle frame before sending: {before}")
        if scenario == "noface" and name(before) != "other": fails.append(tag + f"face square drew something with no frames: {before}")

        for ch in QUESTION:
            keys("spc" if ch == " " else ch); time.sleep(0.05)
        time.sleep(0.3); keys("ret")
        # speak_text logs "speak: status=" once the audio is fetched, right
        # before it plays; sample until then, then through the clip and past it.
        seen, t_end = [], time.time() + 45
        while time.time() < t_end and "speak: status=" not in serial():
            seen.append(name(face())); time.sleep(0.1)
        t_end = time.time() + SECS + 3
        while time.time() < t_end:
            seen.append(name(face())); time.sleep(0.1)
        after = face()
        runs = [seen[0]] + [b for a, b in zip(seen, seen[1:]) if a != b]
        print(tag + "face colors over the reply: " + " > ".join(runs))
        print(tag + f"face square after the reply: {after} ({name(after)})")
        if "chatreply=" not in serial(): fails.append(tag + "Chat never got its reply")
        if "speak: status=200" not in serial(): fails.append(tag + "the reply was never spoken")
        if scenario == "face":
            if "green" not in seen or "blue" not in seen: fails.append(tag + "face never cycled talk frames while she spoke")
            if name(after) != "idle": fails.append(tag + f"face did not return to idle after the audio: {after}")
        else:
            if any(n in ("idle", "green", "blue") for n in seen): fails.append(tag + "a face appeared with no frames")
        if "timed out" in serial(): fails.append(tag + "a DMA transfer timed out")
    finally:
        q.kill(); q.wait()


try:
    run("face", f"10.0.2.2:{port}")
    run("noface", "10.0.2.2:1")
finally:
    srv.shutdown()

if fails:
    for x in fails: print("FAIL: " + x)
    sys.exit(1)
print("chat-face-check: OK, idle face before, talk frames cycled while she spoke, idle after; no frames means no face")
