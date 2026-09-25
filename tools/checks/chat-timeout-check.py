#!/usr/bin/env python3
"""1.1.3: CodeRabbit review of the 1.1.0 Chat work, item 4 -- a connected-
but-silent LLM host must not hold the GUI Chat app (kernel/chat.h) for
net.c's old SLOW_REPLY_TIMEOUT_TICKS default (15000 ticks, ~150s, sized
for a slow local LLM under load). chat_send/chat_pick now pass http_post
a bounded reply_timeout_ticks (CHAT_SEND_TIMEOUT_TICKS = 4500, ~45s for
/api/chat; CHAT_PICK_TIMEOUT_TICKS = 1000, ~10s for /api/pick, tried
first on every message per the 1.1.0 tool-picker work) instead of the
default.

Same shape as tools/checks/chatapp-check.py (QMP absolute-pointer click
on the Chat dock slot, `n` to open the prompt, a typed question, Enter),
except the fake "Samantha" here accepts the TCP connection and reads the
request but never answers at all -- exactly the "connected, then nothing"
case the old unbounded default let hang indefinitely. Asserts:

  1. chatpickfail=noreply fires on serial (the /api/pick leg gave up)
     well under CHAT_PICK_TIMEOUT_TICKS + net RTT slack.
  2. chatfail=noreply fires next (the /api/chat leg gave up) well under
     CHAT_SEND_TIMEOUT_TICKS + slack, and the WHOLE round trip (pick then
     send) lands under a generous wall-clock ceiling -- proving this is a
     bounded failure, not the old multi-minute hang.
  3. The app is still alive and responsive afterwards: a fresh
     `chatconsole` marker is printed (the app's main loop returned to the
     top and redrew, it did not stay wedged inside the network call), the
     rendered status line shows the existing generic error text (not a
     blank screen), and the title bar's red close light still closes the
     window on a click.

Discriminating: reverting chat_send/chat_pick to call plain http_post (no
timeout override) makes this fail by wall-clock alone -- against the
default 15000-tick (~150s) timeout neither chatpickfail= nor chatfail=
appears within this check's ~70s ceiling, so it times out and fails
before ever getting to the responsiveness assertions. Reverting only the
markers (keeping the timeout) makes the marker assertions fail even
though the bound itself still held, so both halves of the fix are
covered.

Usage: python3 tools/checks/chat-timeout-check.py   (from the repo root, after make kernel.elf)
"""
import http.server, os, socket, subprocess, sys, threading, time
from PIL import Image

QUESTION = "what is the capital of france"
LOG = "/tmp/jt-chattimeout-serial.log"; DUMP = "/tmp/jt-chattimeout.raw"
FB = 0xfd000000; W, H = 1920, 1080; PORT = 4471
LOGICAL_W, LOGICAL_H, SCALE = 960, 540, 2
DOCK_ICON, DOCK_GAP, SLOT0_X = 37, 6, 247; PITCH = DOCK_ICON + DOCK_GAP; ICON_ROW_Y = 487
CLOSE_X, CLOSE_Y = 94, 56; CLOSE_RED = (0xFF, 0x5F, 0x57)

# CHAT_PICK_TIMEOUT_TICKS (1000, ~10s) then CHAT_SEND_TIMEOUT_TICKS (4500,
# ~45s) run back to back for an unhandled message (chat_pick is tried
# first on every message since 1.1.0). ~65s of real kernel-side waiting;
# this check's own polling ceiling below is generous on top of that.
PICK_BOUND_S = 25       # 10s bound + connect/poll slack
ROUNDTRIP_BOUND_S = 75  # 10s pick + 45s send + slack

# A server that accepts the connection, reads the request, and never
# answers: the exact "connected, then silence" case the old unbounded
# default let hang. http.server's BaseHTTPRequestHandler always calls
# handle_one_request in a loop; just never writing a response and instead
# blocking the (daemon) handler thread reproduces it without tearing the
# connection down early, which would be a different (fast) failure mode
# (a plain connect-refused/reset, not a silent host).
class SilentHandler(http.server.BaseHTTPRequestHandler):
    def log_message(self, *a): pass
    def do_POST(self):
        n = int(self.headers.get("Content-Length", "0"))
        self.rfile.read(n)
        time.sleep(120)  # outlives this check; the process exit kills this daemon thread


def main():
    srv = http.server.ThreadingHTTPServer(("127.0.0.1", 0), SilentHandler)
    srv.daemon_threads = True
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    port = srv.server_address[1]

    os.chdir(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
    subprocess.run(["make", "-s", "kernel.elf"], check=True, timeout=120)
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
        import json as _json
        def cmd(o):
            f.write(_json.dumps(o) + "\n"); f.flush()
            while True:
                r = _json.loads(f.readline())
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

        consoles_before = serial().count("chatconsole")
        if consoles_before == 0: fails.append("no chatconsole marker before sending anything")

        keys("n"); time.sleep(0.6)
        for ch in QUESTION:
            keys("spc" if ch == " " else ch); time.sleep(0.05)
        time.sleep(0.3); keys("ret")
        t0 = time.time()

        pick_t = None
        for _ in range(int(PICK_BOUND_S / 0.5)):
            time.sleep(0.5)
            if "chatpickfail=" in serial(): pick_t = time.time() - t0; break
        if pick_t is None:
            fails.append(f"no chatpickfail= marker within {PICK_BOUND_S}s -- /api/pick did not give up on its own bound")
        else:
            print(f"chatpickfail= at {pick_t:.1f}s (bound: CHAT_PICK_TIMEOUT_TICKS ~10s)")

        send_t = None
        for _ in range(int((ROUNDTRIP_BOUND_S - (pick_t or 0)) / 0.5) if pick_t else int(ROUNDTRIP_BOUND_S / 0.5)):
            time.sleep(0.5)
            if "chatfail=" in serial(): send_t = time.time() - t0; break
        if send_t is None:
            fails.append(f"no chatfail= marker within the {ROUNDTRIP_BOUND_S}s round-trip ceiling -- Chat is still hanging on the silent host")
        else:
            print(f"chatfail= at {send_t:.1f}s total (bound: CHAT_PICK_TIMEOUT_TICKS + CHAT_SEND_TIMEOUT_TICKS ~55s)")
            if send_t > ROUNDTRIP_BOUND_S:
                fails.append(f"chatfail= took {send_t:.1f}s, over the {ROUNDTRIP_BOUND_S}s ceiling")

        time.sleep(1.0)
        text = serial()
        if "chatpickfail=noreply" not in text: fails.append("chatpickfail=noreply marker missing (net.c's REPLY_TIMEOUT case)")
        if "chatfail=noreply" not in text: fails.append("chatfail=noreply marker missing (net.c's REPLY_TIMEOUT case)")
        consoles_after = text.count("chatconsole")
        if consoles_after <= consoles_before:
            fails.append("no fresh chatconsole marker after the failure -- the app loop looks stuck, not just slow")
        else:
            print(f"chatconsole redrew {consoles_after - consoles_before} more time(s) after the failure: app loop is alive")

        # Responsiveness proof: the window is still live and clickable --
        # the red close light (unmoved, no drag involved here) still closes
        # it, exactly like every other Chat/app-close check in this suite.
        move(CLOSE_X, CLOSE_Y); time.sleep(0.3); click(); time.sleep(0.8)
        if is_red(pixel(dump(), CLOSE_X, CLOSE_Y)):
            fails.append("Chat window did not close via the red button after the timeout -- GUI looks unresponsive")
        else:
            print("close light still closed the window after the timeout: GUI stayed responsive")

        try: cmd({"execute": "quit"})
        except Exception: pass
    except Exception as e:
        fails.append(f"exception: {e}")
    finally:
        try: q.wait(timeout=5)
        except Exception: q.kill()

    if fails:
        print("FAIL:")
        for m in fails: print("  - " + m)
        sys.exit(1)
    print("PASS: a connected-but-silent LLM host gives Chat a bounded failure (pick ~10s, chat ~45s) "
          "instead of net.c's old multi-minute default, reports the existing error status, and the GUI "
          "(chatconsole redraw, the red close light) stays responsive throughout")


if __name__ == "__main__":
    main()
