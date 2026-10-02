#!/usr/bin/env python3
"""Chat face: Samantha's face shows in her window and moves while she talks.

Headless only (-display none, never a window). A loopback stub stands in
for both hosts through QEMU's user NAT at 10.0.2.2: facehost= serves the
face frames AND her speech (ring-3 Samantha fetches both with SYS_HTTP_GET,
whose host is the fixed Worker host that facehost= overrides, see
jt_facehost_cmdline in kernel/syscall.c) and llmhost= answers her POSTs
(/api/pick with an empty object, /api/chat with the reply). The frames are
320x320 solid-color JPEGs so the screen can be read without guessing: every
idle frame is red, the talk frames alternate green (even) and blue (odd), and
talk-47 is garbage bytes (a bad frame must end its clip, not crash anything).

2.0.0: she is the ring-3 compositor window user/samantha.c. She opens with the
`samantha` boot flag and loads all 24 idle and 48 talk frames one fetch per idle
poll (a fetch never runs while typed text is pending, so the check waits for her
"face:" line before it types). A frame that fails is retried once and then
skipped, and the clip goes on: one flaky fetch never truncates her face. Her
"face:" line says how far each clip got ("end=24/48"), how many frames she gave
up on and how many she retried. The stub serves every frame; it fails the first
fetch of idle-7 and talk-10 (a flaky network: the retry must recover them) and
talk-47 is garbage bytes (a bad frame is skipped, not fatal). So the line must
read idle=24 talk=47 end=24/48 skipped=1 with retried at least 2. The mouth
follows playback progress (one cached talk frame per 160 ms of audio played), so
the colour alternation below is the mouth following the clip. Her serial lines
come through the kernel's "syscall: write(1) from ring 3: " prefix.

Scenario "face": her window opens, assert serial says "face: idle=24 talk=47 end=24/48 skipped=1"
and the face is red. Send a message; the stub answers /api/speak with 4s of
audio (the 64 KB the call can carry). While it plays the face must cycle
green and blue. After it ends, red again, and the machine must not have
rebooted.

Scenario "noface": facehost= points at a closed port. She must still answer,
serial says "face: idle=0 talk=0" (a dead host ends the idle clip after a few
skipped frames, it does not cost 24 timeouts), and the face square stays plain background
(no red). Speech comes from the same closed host, so it is not asserted here.

Discriminating: without the playback hook the square stays red the whole
reply and the talk-colour assertion fails by name; without the fetch it never
turns red at all.
"""
import http.server, io, json, math, os, re, socket, subprocess, sys, tempfile, threading, time
from PIL import Image
from freeport import free_port

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
os.chdir(ROOT)
subprocess.run(["make", "-s", "kernel.elf"], check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

FB = 0xfd000000; W, H = 1920, 1080; QMP_PORT = free_port()
CLOSE_X, CLOSE_Y = 188, 112; CLOSE_RED = (0xFF, 0x5F, 0x57)   # framebuffer pixels (the 2x desktop)
FACE_X, FACE_Y = 960, 280   # centre of her 60x60 face (120x120 on screen), centred under the title
IDLE, GREEN, BLUE = (220, 30, 30), (30, 200, 30), (30, 30, 220)
QUESTION = "hello there"
REPLY = "Hi, it is lovely to see you."
SECS = 4.0   # 64000 bytes at 16 kHz: one /api/speak GET, under the 64 KB the call carries
TONE = bytes(int(128 + 100 * math.sin(2 * math.pi * 440 * i / 16000)) for i in range(int(16000 * SECS)))


def jpg(color):
    b = io.BytesIO(); Image.new("RGB", (320, 320), color).save(b, "JPEG", quality=90); return b.getvalue()


FRAMES = {f"/face/idle-{i}.jpg": jpg(IDLE) for i in range(24)}
FRAMES.update({f"/face/talk-{i}.jpg": jpg(GREEN if i % 2 == 0 else BLUE) for i in range(47)})
FRAMES["/face/talk-47.jpg"] = b"\xff\xd8\xff not really a jpeg"


FLAKY = {"/face/idle-7.jpg", "/face/talk-10.jpg"}   # fail once, the loader's retry must recover them
failed_once = set()


class Stub(http.server.BaseHTTPRequestHandler):
    def log_message(self, *a): pass

    def reply(self, code, body, ctype):
        self.send_response(code); self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body))); self.end_headers(); self.wfile.write(body)

    def do_GET(self):
        if self.path.startswith("/api/speak"):  # her speech is a GET now: /api/speak?t=<sentence>
            self.reply(200, TONE, "application/octet-stream")
            return
        body = FRAMES.get(self.path)
        if self.path in FLAKY and self.path not in failed_once:
            failed_once.add(self.path); self.reply(503, b"try again", "text/plain"); return
        if body is None: self.reply(404, b"not found", "text/plain")
        else: self.reply(200, body, "image/jpeg")

    def do_POST(self):
        self.rfile.read(int(self.headers.get("Content-Length", "0")))
        if self.path == "/api/pick":
            self.reply(200, b"{}", "application/json")  # no tool named: the message falls through to /api/chat
        elif self.path == "/api/chat":
            self.reply(200, json.dumps({"model": "samantha", "message": {"role": "assistant", "content": REPLY}, "done": True}).encode(), "application/json")
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
    q = subprocess.Popen(["qemu-system-i386", "-kernel", "kernel.elf", "-display", "none", "-vga", "std", "-no-reboot",
                          "-qmp", f"tcp:127.0.0.1:{QMP_PORT},server,nowait", "-serial", "file:" + log,
                          "-net", "nic,model=rtl8139", "-net", "user",
                          "-audiodev", f"wav,id=snd,path={os.path.join(work, 'out.wav')}", "-device", "sb16,audiodev=snd",
                          "-append", f"samantha llmhost=10.0.2.2 llmport={port} facehost={facehost}"],
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

        def dump():
            cmd({"execute": "pmemsave", "arguments": {"val": FB, "size": W * H * 4, "filename": dump_path}})
            return Image.frombytes("RGBA", (W, H), open(dump_path, "rb").read(), "raw", "BGRA").convert("RGB")

        def face(): return dump().getpixel((FACE_X, FACE_Y))

        def keys(k): cmd({"execute": "send-key", "arguments": {"keys": [{"type": "qcode", "data": k}]}})

        def serial():
            try: return open(log, encoding="latin-1").read()
            except FileNotFoundError: return ""

        def face_lines(): return [m.group(0) for m in re.finditer(r"face: idle=\d+ talk=\d+ end=\d+/\d+ skipped=\d+ retried=\d+", serial())]

        for _ in range(200):
            time.sleep(0.2)
            if "samfocus" in serial(): break
        else: fails.append(tag + "her window never opened (no samfocus marker)"); return
        # She fetches her frames one per idle poll, never while typed text is
        # pending: wait for the load to finish before typing anything.
        for _ in range(300):
            time.sleep(0.3)
            if face_lines(): break
        # The face redraws asynchronously after the "face: " serial line
        # lands, so a fixed sleep here is a coin flip on a loaded host:
        # poll the actual pixel until it settles on the expected frame (or
        # a generous deadline elapses, still failing the assertions below).
        want_name = "idle" if scenario == "face" else "other"
        deadline = time.time() + 6
        before = face()
        while name(before) != want_name and time.time() < deadline:
            time.sleep(0.1); before = face()
        lines = face_lines()
        print(tag + (lines[-1] if lines else "(no face: line)"))
        want = "face: idle=24 talk=47 end=24/48 skipped=1" if scenario == "face" else "face: idle=0 talk=0"
        if not any(l.startswith(want + " ") for l in lines): fails.append(tag + f"serial did not say '{want}' (got {lines})")
        if scenario == "face" and lines:
            retried = int(lines[-1].rsplit("retried=", 1)[1])
            if retried < 2: fails.append(tag + f"the flaky frames were not retried (retried={retried}, want at least 2)")
            if failed_once != FLAKY: fails.append(tag + f"the stub never saw the flaky frames asked for ({sorted(failed_once)})")
        print(tag + f"face square before sending: {before} ({name(before)})")
        if scenario == "face" and name(before) != "idle": fails.append(tag + f"face square is not the idle frame before sending: {before}")
        if scenario == "noface" and name(before) != "other": fails.append(tag + f"face square drew something with no frames: {before}")

        for ch in QUESTION:
            keys("spc" if ch == " " else ch); time.sleep(0.05)
        time.sleep(0.3); keys("ret")
        # speak_text logs "speak: status=" once the audio is fetched, right
        # before it plays; sample until then, then through the clip and past it.
        seen, t_end = [], time.time() + 45
        while time.time() < t_end and "speak: status=" not in serial() and not (scenario == "noface" and "chatreply=" in serial()):
            seen.append(name(face())); time.sleep(0.1)
        # A fixed SECS+3 wall-clock window assumes the guest's audio DMA
        # keeps pace with real time; under a loaded host it can fall behind,
        # so poll for the real end-of-playback condition (idle, held for a
        # few samples, after having actually cycled through talk colors)
        # instead, with a generous deadline for a slow run.
        t_end = time.time() + SECS + 20
        after = face(); idle_streak = 0
        while time.time() < t_end:
            after = face(); n = name(after)
            seen.append(n)
            idle_streak = idle_streak + 1 if n == "idle" else 0
            if scenario == "noface" and "chatreply=" in serial() and len(seen) > 20: break
            if idle_streak >= 3 and ("green" in seen or "blue" in seen): break
            time.sleep(0.1)
        runs = [seen[0]] + [b for a, b in zip(seen, seen[1:]) if a != b]
        print(tag + "face colors over the reply: " + " > ".join(runs))
        print(tag + f"face square after the reply: {after} ({name(after)})")
        if "chatreply=" not in serial(): fails.append(tag + "Chat never got its reply")
        if scenario == "face" and "speak: status=200" not in serial(): fails.append(tag + "the reply was never spoken")
        if scenario == "face":
            if "green" not in seen or "blue" not in seen: fails.append(tag + "face never cycled talk frames while she spoke")
            if name(after) != "idle": fails.append(tag + f"face did not return to idle after the audio: {after}")
        else:
            if any(n in ("idle", "green", "blue") for n in seen): fails.append(tag + "a face appeared with no frames")
        if "timed out" in serial(): fails.append(tag + "a DMA transfer timed out")
        if serial().count("kmain boot start") > 1 or q.poll() is not None or "exception" in serial():
            fails.append(tag + "the machine crashed or rebooted around her reply")
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
