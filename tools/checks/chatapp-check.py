#!/usr/bin/env python3
"""1.0.12: headless proof that the real GUI Chat app (kernel/chat.h,
gui_launch_chat_app, dock slot 7) can ask Samantha a question and put her
answer on the emulated screen -- not just on serial. chat-samantha-check.py
covers the shell's `chat` command and the wire shape; this one covers the
app a visitor actually clicks: QMP absolute-pointer click on the Chat dock
slot (same shape as appclose-check.py), `n` to open the prompt, the question
typed with send-key, Enter, then pmemsave of the framebuffer.

The fake Samantha is a loopback HTTP server reached through QEMU's user-mode
NAT at 10.0.2.2 (the kernel's `llmhost=`/`llmport=` multiboot override);
turing.heyitsmejosh.com is never touched.

Asserts, in order: the Chat window opened (red close light at (94,56));
zero CHAT_INK pixels below the reply line before anything is sent; after
`chatreply=` fires on serial, the question row carries ink (the >>> echo)
AND the band below it carries ink (the rendered answer); the recorded
request is Ollama-shaped with model samantha and the typed question as its
newest user message; the serial reply line carries the fake answer; and
the red close light still closes the window afterwards.

Discriminating: with chat.h's render of the assistant turn stubbed out (or
`chat_send` failing), the band below the question row stays at zero ink
and the run fails by name.

Usage: python3 tools/checks/chatapp-check.py   (from the repo root, after make kernel.elf)
"""
import http.server, json, os, socket, subprocess, sys, threading, time
from PIL import Image

QUESTION = "what is the capital of france"
REPLY = "The capital of France is Paris, a city famous for the Eiffel Tower and croissants."
LOG = "/tmp/jt-chatapp-serial.log"; DUMP = "/tmp/jt-chatapp.raw"
FB = 0xfd000000; W, H = 1920, 1080; PORT = 4470
LOGICAL_W, LOGICAL_H, SCALE = 960, 540, 2
DOCK_ICON, DOCK_GAP, SLOT0_X = 37, 6, 247; PITCH = DOCK_ICON + DOCK_GAP; ICON_ROW_Y = 487
CLOSE_X, CLOSE_Y = 94, 56; CLOSE_RED = (0xFF, 0x5F, 0x57)
VX, VY, VW, VH = 78, 72, 804, 345   # app viewport (x+8, y+32, w-16, h-40) for x=70,y=40,w=820,h=385
INK = (0x1C, 0x1C, 0x1E)
QROW_TOP = VY + (-32 + 76)           # viewport y of the question row (T=-32 windowed)
REPLY_TOP = QROW_TOP + 20

recorded = {}
class Hd(http.server.BaseHTTPRequestHandler):
    def log_message(self, *a): pass
    def do_POST(self):
        n = int(self.headers.get("Content-Length", "0")); body = self.rfile.read(n)
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
                      "-append", f"llmhost=10.0.2.2 llmport={port}"],
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
    def ink_below(img, ytop):
        n = 0
        for y in range(ytop, VY + VH - 20):
            for x in range(VX + 4, VX + VW - 4):
                if pixel(img, x, y) == INK: n += 1
        return n
    def keys(*qc): cmd({"execute": "send-key", "arguments": {"keys": [{"type": "qcode", "data": k} for k in qc]}})
    def serial(): 
        try: return open(LOG, "r", encoding="latin-1").read()
        except FileNotFoundError: return ""
    for _ in range(120):
        if pixel(dump(), 480, 511) == (0xEF, 0xEB, 0xE4): break
        time.sleep(0.25)
    else: raise SystemExit("FAIL: desktop never appeared")
    time.sleep(0.5)
    move(SLOT0_X + 7 * PITCH + DOCK_ICON // 2, ICON_ROW_Y); time.sleep(0.3); click()
    for _ in range(40):
        time.sleep(0.1)
        if is_red(pixel(dump(), CLOSE_X, CLOSE_Y)): break
    else: fails.append("Chat window never opened from dock slot 7")
    time.sleep(0.5)
    if "chatconsole" not in serial(): fails.append("no chatconsole marker")
    before = dump(); before_ink = ink_below(before, REPLY_TOP)
    keys("n"); time.sleep(0.6)
    for ch in QUESTION:
        keys("spc" if ch == " " else ch); time.sleep(0.05)
    time.sleep(0.3); keys("ret")
    for _ in range(60):
        time.sleep(0.5)
        if "chatreply=" in serial(): break
    else: fails.append("no chatreply= marker within 30s")
    time.sleep(1.5)
    after = dump()
    after_ink = ink_below(after, REPLY_TOP); q_ink = ink_below(after, QROW_TOP) - after_ink
    print("ink below reply line: before=%d after=%d; question-row ink=%d" % (before_ink, after_ink, q_ink))
    if before_ink != 0: fails.append("ink present below reply line before any message (%d)" % before_ink)
    if after_ink < 200: fails.append("reply did not render: only %d ink px below the question row" % after_ink)
    if q_ink < 50: fails.append("question echo did not render (%d ink px)" % q_ink)
    body = recorded.get("body", b"").decode("utf-8", "replace")
    print("recorded request:", recorded.get("path"), body[:300])
    try:
        j = json.loads(body)
        if j.get("model") != "samantha": fails.append("model != samantha: %r" % j.get("model"))
        if j["messages"][-1]["content"] != QUESTION: fails.append("last user message wrong: %r" % j["messages"][-1])
    except Exception as e: fails.append("request not Ollama JSON: %s" % e)
    rl = [l for l in serial().splitlines() if l.startswith("chatreply=")]
    print("serial:", rl[-1] if rl else "(none)")
    if not rl or "Paris" not in rl[-1]: fails.append("chatreply serial line lacks the fake reply")
    move(CLOSE_X, CLOSE_Y); time.sleep(0.2); click(); time.sleep(0.8)
    if is_red(pixel(dump(), CLOSE_X, CLOSE_Y)): fails.append("Chat window did not close via red button after the reply")
finally:
    q.kill(); q.wait()
if fails:
    print("FAIL:"); [print("  -", x) for x in fails]; sys.exit(1)
print("PASS: GUI Chat app asked a question and rendered Samantha's reply on screen")
