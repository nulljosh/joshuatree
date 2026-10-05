#!/usr/bin/env python3
"""1.1.0 ("Samantha's tools work from the Chat app"): headless proof that
Samantha's tool picker really lets her handle a message locally -- new
reminder, a note, opening another app -- instead of always asking the model,
and that the ordinary question path (/api/pick answers null) still falls
straight through to /api/chat exactly as before.

2.0.0: she is the ring-3 compositor window user/samantha.c now, so this boots
with the `samantha` flag (her window opens first, her input bar focused), types
straight into the bar (there is no `n` key and the empty-state rows are not
clickable), and reads her markers behind the kernel's
"syscall: write(1) from ring 3: " serial prefix. Apps are opened through her
open_app tool, never by dock coordinates. Same fake-HTTP-server shape as
tools/checks/chat-samantha-check.py (reached from the guest at 10.0.2.2 via the
kernel's llmhost=/llmport= override, turing.heyitsmejosh.com never touched).
One fake server answers both POST /api/pick (scripted per question) and POST
/api/chat inside one continuous boot, scenarios back to back, since the
REMINDERS.TXT state an earlier scenario leaves is what the later ones read.

Scenarios:
  (0) The empty-state example "note pick up dry cleaning" typed into the bar:
      pick answers new_note; asserts `chattool=new_note:pick up dry cleaning`.
  (a) "what are my reminders" first: pick answers list_reminders and the
      list is empty (`chattool=list_reminders:none`). Then "remind me to buy
      milk": pick answers new_reminder/"buy milk". Asserts
      `chattool=new_reminder:buy milk` fires, the confirmation renders as ink
      in her transcript, and /api/chat is never hit. Then "what are my
      reminders" again: `chattool=list_reminders:` now carries "buy milk",
      which she reads back off REMINDERS.TXT (rem_load opens the file every
      time), so the row is a real persisted one and not just a rendered
      bubble.
  (b) "what is the capital of france" -> pick answers {"tool":null}.
      Asserts /api/chat WAS called this time and its reply renders as ink
      in the transcript: the tool path never swallows an ordinary question.
  (c) "open notes" -> pick answers open_app/"notes". Asserts
      `chattool=open_app:Notes` and that the Notes ring-3 window really came
      up (`notes: ring-3 window` on serial) with no extra click.

Discriminating: with run_tool stubbed to `return 0` unconditionally, scenario
(a) never emits `chattool=new_reminder:`, the reply comes from the fake
/api/chat instead, the fake server DOES see a POST /api/chat for it, and the
read-back list stays empty; scenario (c) never sees the Notes window, because
she would ask the model "open notes" as a plain question.

Usage: python3 tools/checks/chattools-check.py         (from the repo root, after make kernel.elf)
       python3 tools/checks/chattools-check.py --live   (real Turing host, no host overrides; prints chatpick=... if the real picker answers)
"""
import re, http.server, json, os, socket, subprocess, sys, tempfile, threading, time
from PIL import Image
from freeport import free_port

LOG = "/tmp/jt-chattools-serial.log"; DUMP = "/tmp/jt-chattools.raw"
FB = 0xfd000000; W, H = 1920, 1080; PORT = free_port()

# Her transcript, in framebuffer pixels (the 2x desktop): the left half of the
# body holds only her own reply bubbles (the typed turns sit on the right, the
# face above y=340), so dark pixels there are her rendered replies.
# 2.9.0 put her reply caption at the bottom (y 770 to 930). In a wide window (2.12.1: her mouth is low and her chin travels down toward the
# glass panel) the captions are one compact row (26 logical px, its feather 4) with its bottom 4 px above the panel: logical y 440 to 478, so
# y 870 to 960 here. Its dark backdrop is the ink.
TX0, TX1, TY0, TY1 = 600, 1320, 870, 960
DARK = 90

REPLY_CHAT = "The capital of France is Paris, a city famous for the Eiffel Tower and croissants."

# Scripted /api/pick answers, matched by a substring of the (lowercased)
# "q" field she actually sends -- the real picker, a small model plus strict
# validation server-side, is out of scope for a network-free regression
# test; this stands in for "the picker said X".
PICK_ANSWERS = [
    ("pick up dry cleaning", {"tool": "new_note", "arg": "pick up dry cleaning"}),
    ("buy milk", {"tool": "new_reminder", "arg": "buy milk"}),
    ("my reminders", {"tool": "list_reminders", "arg": ""}),
    ("capital of france", {"tool": None, "arg": ""}),
    ("open notes", {"tool": "open_app", "arg": "notes"}),
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
            rep = json.dumps({"model": "samantha", "message": {"role": "assistant", "content": REPLY_CHAT}, "done": True}).encode()
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

    if "--live" in sys.argv:
        run_live()
        return

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

        def settle():
            """Wait until every caption has faded (her own marker), so the ink count starts from a bare picture."""
            for _ in range(200):
                v = re.findall(r"samface: captions=(\d)", serial())
                if not v or v[-1] == "0": return
                time.sleep(0.25)

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

        # her window drew and focused its input bar; the face-frame fetches
        # settle a moment later, so give the first keys a clear runway
        wait_for("samfocus", 40, "no samfocus marker: Samantha's window never opened")
        time.sleep(2.0)

        # --- scenario (0): an empty-state example, typed straight in ------
        type_msg("note pick up dry cleaning")
        wait_for("chattool=new_note:", 20, "no chattool=new_note: marker for the typed empty-state example")
        sl0 = serial()
        if not any(l.startswith("chattool=new_note:pick up dry cleaning") for l in marker_lines(sl0, "chattool=new_note:")):
            fails.append("scenario 0: typing the example ran the wrong tool/arg: %r" % marker_lines(sl0, "chattool="))
        else:
            print("scenario 0: typing an empty-state example fired chattool=new_note: with no `n` press")
        time.sleep(1.0)

        # --- baseline: no reminders yet, read back through the picker ------
        mark = len(serial())
        type_msg("what are my reminders")
        wait_for("chattool=list_reminders:", 20, "scenario a: no chattool=list_reminders: marker for the empty baseline", after=mark)
        base = marker_lines(serial()[mark:], "chattool=list_reminders:")
        print("reminders before scenario (a): %r" % (base[-1] if base else None))
        if not base or base[-1] != "chattool=list_reminders:none":
            fails.append("scenario a: the reminders list was not empty before anything was added: %r" % base)
        time.sleep(1.0)

        # --- scenario (a): "remind me to buy milk" -> new_reminder ------
        settle(); before_ink = reply_ink(dump())
        mark = len(serial())
        type_msg("remind me to buy milk")
        wait_for("chattool=new_reminder:", 20, "no chattool=new_reminder: marker", after=mark)
        time.sleep(1.2)
        reply_a = reply_ink(dump()) - before_ink
        print("scenario a: new reply-band ink = %d" % reply_a)
        if reply_a < 100: fails.append("scenario a: confirmation reply did not render in her transcript (%d new ink px)" % reply_a)
        if state["chat_calls"]: fails.append("scenario a: /api/chat was called (%d time(s)) even though the picker named a locally-handled tool" % len(state["chat_calls"]))
        sl = serial()[mark:]
        if not any(l == "chattool=new_reminder:buy milk" for l in marker_lines(sl, "chattool=new_reminder:")):
            fails.append("scenario a: chattool=new_reminder: marker missing or wrong text: %r" % marker_lines(sl, "chattool="))

        # the reminder is a real persisted row: she reads it back off REMINDERS.TXT
        mark = len(serial())
        type_msg("what are my reminders")
        wait_for("chattool=list_reminders:", 20, "scenario a: no chattool=list_reminders: marker after adding", after=mark)
        after = marker_lines(serial()[mark:], "chattool=list_reminders:")
        print("reminders after scenario (a): %r" % (after[-1] if after else None))
        if not after or "buy milk" not in after[-1]:
            fails.append("scenario a: the reminder was never really added, REMINDERS.TXT reads back %r" % after)
        time.sleep(1.0)

        # --- scenario (b): an ordinary question -> pick says null --------
        settle(); before_b = reply_ink(dump())
        mark = len(serial())
        type_msg("what is the capital of france")
        wait_for("chatreply=", 30, "scenario b: no chatreply= marker", after=mark)
        time.sleep(1.5)
        after_b = reply_ink(dump()) - before_b
        print("scenario b: new reply-band ink = %d" % after_b)
        if after_b < 200: fails.append("scenario b: her reply did not render (%d new ink px)" % after_b)
        if not state["chat_calls"]: fails.append("scenario b: /api/chat was never called for an ordinary question")
        rl = marker_lines(serial()[mark:], "chatreply=")
        if not rl or "Paris" not in rl[-1]: fails.append("scenario b: chatreply= serial line lacks the fake reply")

        # --- scenario (c): "open notes" -> open_app -----------------------
        mark = len(serial())
        type_msg("open notes")
        wait_for("chattool=open_app:Notes", 20, "scenario c: no chattool=open_app:Notes marker", after=mark)
        wait_for("notes: ring-3 window", 20, "scenario c: Notes never opened (no ring-3 window marker)", after=mark)

    finally:
        q.kill(); q.wait()
        try: srv.shutdown()
        except Exception: pass

    if fails:
        print("FAIL:")
        for x in fails: print("  -", x)
        sys.exit(1)
    print("PASS: Chat's tool picker adds a real reminder and skips /api/chat for it, "
          "still asks Samantha a plain question, and hands off to Notes with no extra click")


def send(s):
    lines = []
    for c in s:
        lines.append("sendkey spc" if c == " " else "sendkey %s" % c)
    lines.append("sendkey ret")
    return "\n".join(lines) + "\n"


def run_live():
    """--live: no host overrides at all, the kernel's own compiled-in
    defaults (turing.heyitsmejosh.com:80, model samantha) are what's
    actually exercised, same shape as chat-samantha-check.py's own --live.
    Sends "remind me to test the live picker" through the shell `chat`
    command from a plain boot (text output only, no QMP/mouse needed
    here -- the shell command exercises the exact same chat_pick this
    script's headless scenarios already prove against a fake server) and
    prints whether the real picker answered chatpick=new_reminder."""
    workdir = tempfile.mkdtemp(prefix="jt-chattools-live-")
    log = os.path.join(workdir, "serial.log")
    args = ["qemu-system-i386", "-kernel", "kernel.elf", "-display", "none",
            "-monitor", "stdio", "-serial", "file:" + log,
            "-net", "nic,model=rtl8139", "-net", "user", "-append", "samantha"]
    proc = subprocess.Popen(args, stdin=subprocess.PIPE, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        for _ in range(60):  # her window drew and focused its input bar
            time.sleep(0.5)
            try:
                if "samfocus" in open(log, "r", encoding="latin-1").read(): break
            except FileNotFoundError: pass
        time.sleep(2)
        proc.stdin.write(send("remind me to test the live picker").encode()); proc.stdin.flush()
        serial_text = ""
        for _ in range(90):
            time.sleep(1)
            try:
                serial_text = open(log, "r", encoding="latin-1").read()
            except FileNotFoundError:
                serial_text = ""
            if "chatpick=" in serial_text or "chatreply=" in serial_text or "chathttps=" in serial_text:
                break
        proc.stdin.write(b"quit\n"); proc.stdin.flush()
    finally:
        try: proc.stdin.close()
        except Exception: pass
        try: proc.wait(timeout=10)
        except Exception: proc.kill(); proc.wait(timeout=5)

    print("---- live serial (chat*/chattool* lines only) ----")
    for l in serial_text.splitlines():
        if "chat" in l:
            print(l)
    print("----------------------------------------------------")
    pick_lines = marker_lines(serial_text, "chatpick=")
    if not pick_lines:
        print("NOTE: no chatpick= line at all -- /api/pick was unreachable (network/DNS) or the request failed; "
              "this is informational only, not a failure (the pick is an optimisation, chat_send still ran)")
    elif pick_lines[-1] == "chatpick=new_reminder":
        print("PASS (live): the real Turing picker answered chatpick=new_reminder for \"remind me to test the live picker\"")
    else:
        print("NOTE: the real picker answered %r, not new_reminder -- the live model's own judgement call, not this kernel's bug" % pick_lines[-1])


if __name__ == "__main__":
    main()
