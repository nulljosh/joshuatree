#!/usr/bin/env python3
"""Asking Samantha for the weather fetches it. She never says "open Weather first".

Boots with `samantha` (her full-screen avatar, the phone demo's path), so the
desktop never runs and nothing has fetched the weather yet. Types "whats the
weather"; a fake /api/pick at 10.0.2.2 answers the weather tool. Asserts a
`wxfetch` line lands between the question and her `chattool=weather:` reply,
i.e. the tool fetched on its own. Discriminating: drop the weather_fetch()
call in kernel/chat.h's weather tool and no wxfetch ever prints on this boot.

Usage: tools/checks/samweather-check.py   (from the repo root)
"""
import http.server, json, os, socket, subprocess, sys, threading, time
from freeport import free_port

LOG = "/tmp/jt-samweather-serial.log"
PORT = free_port()


class Hd(http.server.BaseHTTPRequestHandler):
    def log_message(self, *a): pass
    def do_POST(self):
        self.rfile.read(int(self.headers.get("Content-Length", "0")))
        rep = json.dumps({"tool": "weather", "arg": ""} if self.path == "/api/pick" else
                         {"model": "samantha", "message": {"role": "assistant", "content": "ok"}, "done": True}).encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(rep)))
        self.end_headers()
        self.wfile.write(rep)


os.chdir(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
subprocess.run(["make", "-s", "kernel.elf"], check=True)
srv = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Hd)
threading.Thread(target=srv.serve_forever, daemon=True).start()
try: os.remove(LOG)
except FileNotFoundError: pass
q = subprocess.Popen(["qemu-system-i386", "-kernel", "kernel.elf", "-display", "none", "-vga", "std",
                      "-nic", "user,model=rtl8139",
                      "-qmp", f"tcp:127.0.0.1:{PORT},server,nowait", "-serial", "file:" + LOG,
                      "-append", f"samantha llmhost=10.0.2.2 llmport={srv.server_address[1]}"],
                     stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
def serial():
    try: return open(LOG, encoding="latin-1").read()
    except FileNotFoundError: return ""
def wait_for(m, secs):
    for _ in range(int(secs * 10)):
        if m in serial(): return True
        time.sleep(0.1)
    return False
fail = None
try:
    s = None
    for _ in range(50):
        time.sleep(0.2)
        try: s = socket.create_connection(("127.0.0.1", PORT)); break
        except OSError: pass
    if s is None: raise SystemExit("FAIL: QMP never came up")
    f = s.makefile("rw")
    def cmd(o):
        f.write(json.dumps(o) + "\n"); f.flush()
        while True:
            r = json.loads(f.readline())
            if "return" in r or "error" in r: return r
    f.readline(); cmd({"execute": "qmp_capabilities"})
    def key(k): cmd({"execute": "send-key", "arguments": {"keys": [{"type": "qcode", "data": k}]}})
    if not wait_for("samfocus", 40): raise SystemExit("FAIL: her avatar never focused its input box")
    time.sleep(0.5)
    before = serial().count("wxfetch")
    for ch in "whats the weather":
        key("spc" if ch == " " else ch); time.sleep(0.05)
    time.sleep(0.3); key("ret")
    if not wait_for("chattool=weather:", 40):
        fail = "no chattool=weather line: the picker or tool never ran"
    elif serial().count("wxfetch") <= before:
        fail = "she answered the weather without fetching it (the old \"open Weather first\" reply)"
finally:
    q.kill(); q.wait(); srv.shutdown()
if fail:
    print("FAIL:", fail); sys.exit(1)
print("PASS: asking Samantha for the weather fetched it, no Weather app needed")
