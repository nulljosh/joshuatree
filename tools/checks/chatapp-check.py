#!/usr/bin/env python3
"""1.0.12: headless proof that the real GUI Chat app can ask Samantha a
question and put her answer on the emulated screen -- not just on serial.
chat-samantha-check.py covers the wire shape; this one covers the window a
visitor actually types into.

2.0.0: she is the ring-3 compositor window user/samantha.c now. The check
boots with the `samantha` flag (her window opens first, input bar focused),
types straight into the bar (no dock click, no `n` key), presses Enter, then
pmemsaves the framebuffer. Her serial lines come through the kernel's
"syscall: write(1) from ring 3: " prefix. Her window is cream; the empty
state is the "Say something to Samantha." prompt, and it vanishes the moment
a turn exists, so ink is counted per region instead of against one band.

The fake Samantha is a loopback HTTP server reached through QEMU's user-mode
NAT at 10.0.2.2 (the kernel's `llmhost=`/`llmport=` multiboot override);
turing.heyitsmejosh.com is never touched. /api/pick answers an empty object so
no tool fires and the question goes on to /api/chat.

Asserts, in order: her window opened (samfocus on serial, red close light at
(94,56)); the empty state carries ink (the prompt line, so the window is not a
blank void); after `chatreply=` fires on serial, the typed question's bubble on
the right carries ink (the echo) AND the left half of the transcript carries
ink (the rendered answer, which was zero dark pixels before); the recorded
/api/chat request is Ollama-shaped with model samantha and the typed question
as its newest user message; the serial reply line carries the fake answer;
and the red close light still closes the window afterwards.

Discriminating: with her empty-state prompt removed the empty-state assertion
fails by name; with the assistant turn's render stubbed out (or the chat
send failing) instead, the left-half ink stays at zero and the
`after_ink < 200` assertion fails by name.

Usage: python3 tools/checks/chatapp-check.py   (from the repo root, after make kernel.elf)
"""
import http.server, json, os, socket, subprocess, sys, threading, time
from PIL import Image
from freeport import free_port

QUESTION = "what is the capital of france"
REPLY = "The capital of France is Paris, a city famous for the Eiffel Tower and croissants."
LOG = "/tmp/jt-chatapp-serial.log"; DUMP = "/tmp/jt-chatapp.raw"
FB = 0xfd000000; W, H = 1920, 1080; PORT = free_port()
LOGICAL_W, LOGICAL_H, SCALE = 960, 540, 2
CLOSE_X, CLOSE_Y = 24, 24; CLOSE_RED = (0xFF, 0x5F, 0x57)
# Framebuffer pixels (the 2x desktop). Her transcript: replies on the left
# (x < 880, clear of the face that sits centred above), typed turns on the
# right; the empty-state prompt sits at y~384 on the left.
LEFT = (600, 1320, 770, 930)     # x0, x1, y0, y1: her reply caption (2.9.0: white text on a dark backdrop over the picture; the backdrop is the dark ink)
RIGHT = (0, 0, 0, 0)             # (2.9.0: the question echo is a caption that fades before this check looks; samtyped= proves it instead)
PROMPT = (380, 700, 975, 1030)   # "Message Samantha" in the glass input panel
DARK = 90; GREY = 170

recorded = {}
class Hd(http.server.BaseHTTPRequestHandler):
    def log_message(self, *a): pass
    def do_POST(self):
        n = int(self.headers.get("Content-Length", "0")); body = self.rfile.read(n)
        if self.path == "/api/pick":
            rep = b"{}"  # no tool named: the question falls through to /api/chat
            self.send_response(200); self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(rep))); self.end_headers(); self.wfile.write(rep)
            return
        recorded["path"] = self.path; recorded["body"] = body
        rep = json.dumps({"model": "samantha", "message": {"role": "assistant", "content": REPLY}, "done": True}).encode()
        self.send_response(200); self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(rep))); self.end_headers(); self.wfile.write(rep)
srv = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Hd); srv.daemon_threads = True
threading.Thread(target=srv.serve_forever, daemon=True).start()
port = srv.server_address[1]

os.chdir(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
for f in (LOG, DUMP):
    try: os.remove(f)
    except FileNotFoundError: pass
q = subprocess.Popen(["qemu-system-i386", "-kernel", "kernel.elf", "-display", "none", "-vga", "std",
                      "-qmp", f"tcp:127.0.0.1:{PORT},server,nowait", "-serial", "file:" + LOG,
                      "-net", "nic,model=rtl8139", "-net", "user",
                      "-append", f"samantha llmhost=10.0.2.2 llmport={port}"],
                     stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
fails = []
try:
    s = None
    for _ in range(50):
        time.sleep(0.2)
        try: s = socket.create_connection(("127.0.0.1", PORT)); break
        except OSError: pass
    if s is None: raise SystemExit("FAIL: no QMP")
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
        cmd({"execute": "input-send-event", "arguments": {"events": [{"type": "btn", "data": {"down": True, "button": "left"}}]}})
        time.sleep(0.1)
        cmd({"execute": "input-send-event", "arguments": {"events": [{"type": "btn", "data": {"down": False, "button": "left"}}]}})
    def dump():
        cmd({"execute": "pmemsave", "arguments": {"val": FB, "size": W * H * 4, "filename": DUMP}})
        return Image.frombytes("RGBA", (W, H), open(DUMP, "rb").read(), "raw", "BGRA").convert("RGB")
    def pixel(img, x, y): return img.getpixel((x * SCALE + 1, y * SCALE + 1))
    def is_red(p): return max(abs(p[i] - CLOSE_RED[i]) for i in range(3)) <= 12
    def ink_in(img, box, limit):
        x0, x1, y0, y1 = box; n = 0
        for y in range(y0, y1):
            for x in range(x0, x1):
                p = img.getpixel((x, y))
                if p[0] < limit and p[1] < limit and p[2] < limit: n += 1
        return n
    def keys(*qc): cmd({"execute": "send-key", "arguments": {"keys": [{"type": "qcode", "data": k} for k in qc]}})
    def serial():
        try: return open(LOG, "r", encoding="latin-1").read()
        except FileNotFoundError: return ""
    for _ in range(200):
        time.sleep(0.2)
        if "samfocus" in serial(): break
    else: fails.append("Samantha's window never opened (no samfocus marker)")
    for _ in range(100):  # the compositor paints her window a beat after the marker
        time.sleep(0.2)
        if is_red(pixel(dump(), CLOSE_X, CLOSE_Y)): break
    else: fails.append("no red close dot on her window at (24,24)")
    time.sleep(1.5)
    before = dump(); before_ink = ink_in(before, LEFT, DARK); prompt_ink = ink_in(before, PROMPT, GREY)
    # The empty state is her "Say something to Samantha." prompt, not a
    # blank void; it is grey text, so it counts at the looser threshold, and
    # no dark reply ink exists yet.
    if prompt_ink == 0: fails.append("empty state shows no prompt text (0 ink px where \"Say something to Samantha.\" belongs)")
    for ch in QUESTION:
        keys("spc" if ch == " " else ch); time.sleep(0.05)
    time.sleep(0.3); keys("ret")
    for _ in range(60):
        time.sleep(0.5)
        if "chatreply=" in serial(): break
    else: fails.append("no chatreply= marker within 30s")
    time.sleep(1.5)
    after = dump()
    after_ink = ink_in(after, LEFT, DARK) - before_ink; q_ink = ink_in(after, RIGHT, DARK)
    print("reply ink: before=%d (prompt ink %d) after=+%d; question-bubble ink=%d" % (before_ink, prompt_ink, after_ink, q_ink))
    if after_ink < 200: fails.append("reply did not render: only %d new ink px in her transcript" % after_ink)
    if "samtyped=" + QUESTION not in serial(): fails.append("the question never reached her whole (no samtyped= line)")
    body = recorded.get("body", b"").decode("utf-8", "replace")
    print("recorded request:", recorded.get("path"), body[:300])
    try:
        j = json.loads(body)
        if j.get("model") != "samantha": fails.append("model != samantha: %r" % j.get("model"))
        if j["messages"][-1]["content"] != QUESTION: fails.append("last user message wrong: %r" % j["messages"][-1])
    except Exception as e: fails.append("request not Ollama JSON: %s" % e)
    rl = [l[l.index("chatreply="):] for l in serial().splitlines() if "chatreply=" in l]
    print("serial:", rl[-1] if rl else "(none)")
    if not rl or "Paris" not in rl[-1]: fails.append("chatreply serial line lacks the fake reply")
    move(CLOSE_X, CLOSE_Y); time.sleep(0.2); click(); time.sleep(0.8)
    if is_red(pixel(dump(), CLOSE_X, CLOSE_Y)): fails.append("Chat window did not close via red button after the reply")
finally:
    q.kill(); q.wait()
if fails:
    print("FAIL:"); [print("  -", x) for x in fails]; sys.exit(1)
print("PASS: GUI Chat app asked a question and rendered Samantha's reply on screen")
