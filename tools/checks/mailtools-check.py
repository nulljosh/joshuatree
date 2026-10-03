#!/usr/bin/env python3
"""1.6.21 ("Samantha reads and sends mail from Chat"): headless proof that
her picker's "os":"jt" flag really unlocks read_mail/send_mail against the
real Mail store -- not just that the picker names the tool.

2.0.0: she is the ring-3 window user/samantha.c now. This boots with the
`samantha` flag (her window opens first), types straight into her input bar
(no `n` key), and reads her markers behind the kernel's
"syscall: write(1) from ring 3: " serial prefix. Same fake HTTP server shape
as tools/checks/chattools-check.py, answering both POST /api/pick and POST
/api/chat, reached from the guest at 10.0.2.2 via the kernel's
llmhost=/llmport= override -- turing.heyitsmejosh.com never touched.

Scenarios (one continuous boot, in order):
  (a) "read my email" -> pick answers {"tool":"read_mail","arg":""}.
      Asserts chattool=read_mail: fires with a real subject (the compiled-in
      seed message is the newest at boot), the reply renders as ink in her
      transcript, and /api/chat is never called.
  (b) "email joshua tree saying the tools shipped" -> pick answers
      {"tool":"send_mail","arg":"joshua tree"}. Asserts chattool=send_mail:
      fires and /api/chat is never called. Then "read my email" again: the
      newest message she reads back off MAIL.TXT is now the one she just
      sent ("From Samantha"), not the seed subject from scenario (a) -- a
      real MAIL.TXT append, not just a rendered reply.

Discriminating: with the "os":"jt" flag reverted or run_tool's
read_mail/send_mail cases removed, the picker (scripted here to answer by
substring match regardless of the flag) still names the tool, but a kernel
without this branch falls through to /api/chat instead --
chattool=read_mail:/chattool=send_mail: never fire, /api/chat IS called, and
the second read still returns the seed subject. This script fails by name on
each of those.

Usage: python3 tools/checks/mailtools-check.py
"""
from freeport import free_port
import http.server, json, os, socket, subprocess, sys, threading, time
from PIL import Image

LOG = "/tmp/jt-mailtools-serial.log"; DUMP = "/tmp/jt-mailtools.raw"
FB = 0xfd000000; W, H = 1920, 1080; PORT = free_port()

# Her transcript, in framebuffer pixels (the 2x desktop): the left half of the
# body holds only her own reply bubbles (typed turns sit on the right, the
# face above y=340), so dark pixels there are her rendered replies.
TX0, TX1, TY0, TY1 = 200, 880, 360, 750
DARK = 90

PICK_ANSWERS = [
    ("read my email", {"tool": "read_mail", "arg": ""}),
    ("joshua tree", {"tool": "send_mail", "arg": "joshua tree"}),
]

state = {"chat_calls": [], "pick_calls": []}


class Hd(http.server.BaseHTTPRequestHandler):
    def log_message(self, *a): pass

    def do_POST(self):
        n = int(self.headers.get("Content-Length", "0")); body = self.rfile.read(n)
        if self.path == "/api/pick":
            try:
                q = json.loads(body.decode("utf-8")).get("q", "")
            except Exception:
                q = ""
            state["pick_calls"].append(q)
            answer = {"tool": None, "arg": ""}
            for needle, a in PICK_ANSWERS:
                if needle in q.lower():
                    answer = a
                    break
            rep = json.dumps(answer).encode()
        else:
            state["chat_calls"].append(body)
            rep = json.dumps({"model": "samantha", "message": {"role": "assistant", "content": "I can't help with that here."}, "done": True}).encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(rep)))
        self.end_headers()
        self.wfile.write(rep)


def marker_lines(text, marker):
    """Serial lines carrying `marker`, trimmed to start at it (the ring-3
    prefix "syscall: write(1) from ring 3: " sits in front)."""
    return [l[l.index(marker):] for l in text.splitlines() if marker in l]


def main():
    os.chdir(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
    subprocess.run(["make", "-s", "kernel.elf"], check=True)

    srv = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Hd)
    srv.daemon_threads = True
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    port = srv.server_address[1]

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

        def dump():
            cmd({"execute": "pmemsave", "arguments": {"val": FB, "size": W * H * 4, "filename": DUMP}})
            return Image.frombytes("RGBA", (W, H), open(DUMP, "rb").read(), "raw", "BGRA").convert("RGB")

        def reply_ink(img):
            n = 0
            for y in range(TY0, TY1):
                for x in range(TX0, TX1):
                    p = img.getpixel((x, y))
                    if p[0] < DARK and p[1] < DARK and p[2] < DARK: n += 1
            return n

        def keys(*qc): cmd({"execute": "send-key", "arguments": {"keys": [{"type": "qcode", "data": k} for k in qc]}})

        def type_msg(msg):
            for ch in msg:
                keys("spc" if ch == " " else ch); time.sleep(0.05)
            time.sleep(0.3); keys("ret")

        def serial():
            try: return open(LOG, "r", encoding="latin-1").read()
            except FileNotFoundError: return ""

        def wait_for(marker, timeout_s, label, after=0):
            for _ in range(int(timeout_s / 0.5)):
                time.sleep(0.5)
                if marker in serial()[after:]: return True
            fails.append("%s within %ss" % (label, timeout_s))
            return False

        # her window drew and focused its input bar; give the first keys a clear runway
        wait_for("samfocus", 40, "no samfocus marker: Samantha's window never opened")
        # The wallpaper swaps from the baked satellite to the live map once the geo fetch lands (CI has
        # a network, so it always does, seconds after the window opens). That repaint moves the dark-pixel
        # count by hundreds of thousands, so take the baseline only after it. No network: the wait just times out.
        for _ in range(60):
            if "wallsrc=map" in serial(): break
            time.sleep(0.5)
        time.sleep(3.0)

        # --- scenario (a): "read my email" -> read_mail -------------------
        before_ink = reply_ink(dump())
        mark = len(serial())
        type_msg("read my email")
        wait_for("chattool=read_mail:", 20, "scenario a: no chattool=read_mail: marker", after=mark)
        time.sleep(1.0)
        reply_a = reply_ink(dump()) - before_ink
        print("scenario a: new reply-band ink = %d" % reply_a)
        if reply_a < 100: fails.append("scenario a: read_mail reply did not render in her transcript (%d new ink px)" % reply_a)
        if state["chat_calls"]: fails.append("scenario a: /api/chat was called even though the picker named read_mail")
        reads = marker_lines(serial()[mark:], "chattool=read_mail:")
        seed_subject = None
        if not reads:
            fails.append("scenario a: chattool=read_mail: marker missing: %r" % marker_lines(serial(), "chattool="))
        elif reads[-1] == "chattool=read_mail:none":
            fails.append("scenario a: read_mail reported an empty inbox, but the seed messages should still be present")
        else:
            seed_subject = reads[-1]
            print("scenario a: read_mail listed a real message: %r" % seed_subject)
        time.sleep(1.0)

        # --- scenario (b): "email joshua tree saying ..." -> send_mail ----
        state["chat_calls"] = []
        mark = len(serial())
        type_msg("email joshua tree saying the tools shipped")
        wait_for("chattool=send_mail:", 20, "scenario b: no chattool=send_mail: marker", after=mark)
        time.sleep(0.6)
        if state["chat_calls"]: fails.append("scenario b: /api/chat was called even though the picker named send_mail")
        sends = marker_lines(serial()[mark:], "chattool=send_mail:")
        if not sends or sends[-1] != "chattool=send_mail:joshua tree":
            fails.append("scenario b: chattool=send_mail: marker missing or wrong arg: %r" % marker_lines(serial()[mark:], "chattool="))
        time.sleep(1.0)

        # The message is a real MAIL.TXT row: read_mail loads the file again
        # and now lands on the newest message, the one she just wrote.
        mark = len(serial())
        type_msg("read my email")
        wait_for("chattool=read_mail:", 20, "scenario b: no chattool=read_mail: marker after sending", after=mark)
        again = marker_lines(serial()[mark:], "chattool=read_mail:")
        print("scenario b: newest message after sending: %r (before: %r)" % (again[-1] if again else None, seed_subject))
        if not again or again[-1] != "chattool=read_mail:From Samantha":
            fails.append("scenario b: the newest message read back is %r, not the one she sent -- the message was never really sent" % (again[-1] if again else None))

    finally:
        q.kill(); q.wait()
        try: srv.shutdown()
        except Exception: pass

    if fails:
        print("FAIL:")
        for x in fails: print("  -", x)
        sys.exit(1)
    print("PASS: \"read my email\" lists a real message via chattool=read_mail and "
          "\"email <someone> <text>\" lands a persisted message in Mail via chattool=send_mail")


if __name__ == "__main__":
    main()
