#!/usr/bin/env python3
"""1.1.0 ("Samantha's tools work from the Chat app"): headless proof that
kernel/chat.h's new chat_pick/chat_run_tool really let Chat handle a
message locally -- new reminder, a note, opening another app -- instead of
always asking Samantha, and that the ordinary question path (chat_pick
answers null) still falls straight through to chat_send exactly as before
this pass.

Same shape as tools/checks/chatapp-check.py (QMP absolute-pointer click on
a dock slot, send-key typing, framebuffer pmemsave/ink-counting) and
tools/checks/chat-samantha-check.py (a fake HTTP server reached from the
guest at 10.0.2.2 via the kernel's llmhost=/llmport= multiboot override,
turing.heyitsmejosh.com never touched). One fake server answers both
POST /api/pick (scripted per question) and POST /api/chat, all inside one
continuous boot -- three scenarios run back to back against the one
running kernel, in the order below, since Reminders/NOTES.TXT state left
by an earlier scenario is exactly what the later ones check.

Scenarios:
  (a) "remind me to buy milk" -> pick answers new_reminder/"buy milk".
      Asserts `chattool=new_reminder:` fires on serial, the confirmation
      ("Reminder added: buy milk") renders as ink in the Chat body, and
      the fake server's /api/chat handler is never hit at all. Then closes
      Chat, opens Reminders from its dock slot (5) and confirms its
      window's ink no longer matches the same window's own empty-list
      baseline (captured before this scenario ran) -- a real, persisted
      REMINDERS.TXT row, not just a rendered chat reply. Deliberately not
      "more ink": a short new row can render with fewer exact-color ink
      pixels than the longer "No reminders yet." message it replaced, so
      "different from the known-empty baseline" is what's discriminating.
  (b) "what is the capital of france" -> pick answers {"tool":null}.
      Asserts /api/chat WAS called this time and its reply renders (same
      ink check chatapp-check.py already uses for the plain question
      path), proving the tool path never swallows an ordinary question.
  (c) "open notes" -> pick answers open_app/"notes". Asserts Chat closes
      and Notes opens in its place with no extra click (chat_run_tool's
      chat_launch_after -> gui_launch_from_dock's own again: reopen path):
      the `editorchrome` serial marker fires and the red close light
      shows at (94,56), the same coordinate/marker chatapp-check.py and
      appclose-check.py already trust for "a real app window opened".

Discriminating: with chat_run_tool stubbed to `return 0` unconditionally
(the tool-handling feature reverted), scenario (a) never emits
`chattool=new_reminder:`, the Chat reply comes from the fake /api/chat
reply instead of "Reminder added: buy milk", the fake server DOES see a
POST /api/chat for it, and Reminders' ink never changes -- this script
fails by name on each of those. Scenario (c) similarly never sees
`editorchrome`/the close light, because Chat would ask Samantha "open
notes" as a plain question instead of opening the Notes app itself.

Usage: python3 tools/checks/chattools-check.py         (from the repo root, after make kernel.elf)
       python3 tools/checks/chattools-check.py --live   (real Turing host, no host overrides; prints chatpick=... if the real picker answers)
"""
import http.server, json, os, socket, subprocess, sys, tempfile, threading, time
from PIL import Image

LOG = "/tmp/jt-chattools-serial.log"; DUMP = "/tmp/jt-chattools.raw"
FB = 0xfd000000; W, H = 1920, 1080; PORT = 4471
LOGICAL_W, LOGICAL_H, SCALE = 960, 540, 2
DOCK_ICON, DOCK_GAP, SLOT0_X = 37, 6, 247; PITCH = DOCK_ICON + DOCK_GAP; ICON_ROW_Y = 487
CLOSE_X, CLOSE_Y = 94, 56; CLOSE_RED = (0xFF, 0x5F, 0x57)
VX, VY, VW, VH = 78, 72, 804, 345   # app viewport (x+8, y+32, w-16, h-40) for x=70,y=40,w=820,h=385 -- the one window rect every dock app (blocking or multi-window slot 0) shares
INK = (0x1C, 0x1C, 0x1E)
QROW_TOP = VY + (-32 + 76)           # viewport y of the question row (T=-32 windowed)
REPLY_TOP = QROW_TOP + 20

DOCK_CHAT, DOCK_NOTES, DOCK_REMINDERS = 7, 4, 5  # GUI_DOCK_DEFAULT slots (see kernel.c: Apps,Files,Mail,Calendar,Notes,Reminders,Terminal,Chat,Weather,Stocks,Trash)

REPLY_CHAT = "The capital of France is Paris, a city famous for the Eiffel Tower and croissants."

# Scripted /api/pick answers, matched by a substring of the (lowercased)
# "q" field the kernel actually sends -- real chat_pick behaviour, a small
# model plus strict validation server-side, is out of scope for a
# network-free regression test; this stands in for "the picker said X".
PICK_ANSWERS = [
    ("buy milk", {"tool": "new_reminder", "arg": "buy milk"}),
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

        def ink_count(img, color=INK):
            n = 0
            for y in range(VY, VY + VH - 4):
                for x in range(VX + 4, VX + VW - 4):
                    if pixel(img, x, y) == color: n += 1
            return n

        def ink_below(img, ytop):
            n = 0
            for y in range(ytop, VY + VH - 20):
                for x in range(VX + 4, VX + VW - 4):
                    if pixel(img, x, y) == INK: n += 1
            return n

        def keys(*qc): cmd({"execute": "send-key", "arguments": {"keys": [{"type": "qcode", "data": k} for k in qc]}})

        def type_msg(msg):
            for ch in msg:
                keys("spc" if ch == " " else ch); time.sleep(0.05)

        def serial():
            try: return open(LOG, "r", encoding="latin-1").read()
            except FileNotFoundError: return ""

        def wait_for(marker, timeout_s, label):
            for _ in range(int(timeout_s / 0.5)):
                time.sleep(0.5)
                if marker in serial(): return True
            fails.append("%s within %ss" % (label, timeout_s))
            return False

        def click_dock(slot):
            move(SLOT0_X + slot * PITCH + DOCK_ICON // 2, ICON_ROW_Y); time.sleep(0.3); click()

        for _ in range(120):
            if pixel(dump(), 480, 511) == (0xEF, 0xEB, 0xE4): break
            time.sleep(0.25)
        else: raise SystemExit("FAIL: desktop never appeared")
        time.sleep(0.5)

        # --- baseline: Reminders, empty, before scenario (a) ever runs ---
        click_dock(DOCK_REMINDERS)
        for _ in range(40):
            time.sleep(0.1)
            if is_red(pixel(dump(), CLOSE_X, CLOSE_Y)): break
        else: fails.append("Reminders window never opened from dock slot %d" % DOCK_REMINDERS)
        time.sleep(0.4)
        baseline_ink = ink_count(dump())
        print("Reminders ink before scenario (a): %d" % baseline_ink)
        move(CLOSE_X, CLOSE_Y); time.sleep(0.2); click(); time.sleep(0.6)

        # --- scenario (a): "remind me to buy milk" -> new_reminder ------
        click_dock(DOCK_CHAT)
        for _ in range(40):
            time.sleep(0.1)
            if is_red(pixel(dump(), CLOSE_X, CLOSE_Y)): break
        else: fails.append("Chat window never opened from dock slot %d" % DOCK_CHAT)
        time.sleep(0.4)
        keys("n"); time.sleep(0.5)
        type_msg("remind me to buy milk")
        time.sleep(0.3); keys("ret")
        wait_for("chattool=new_reminder:", 20, "no chattool=new_reminder: marker")
        time.sleep(1.0)
        after_reply = dump()
        reply_ink = ink_below(after_reply, REPLY_TOP)
        print("scenario a: chat reply-band ink = %d" % reply_ink)
        if reply_ink < 100: fails.append("scenario a: confirmation reply did not render in the Chat body (%d ink px)" % reply_ink)
        if state["chat_calls"]: fails.append("scenario a: /api/chat was called (%d time(s)) even though the picker named a locally-handled tool" % len(state["chat_calls"]))
        sl = serial()
        if "chattool=new_reminder:buy milk" not in sl:
            fails.append("scenario a: chattool=new_reminder: marker missing or wrong text: %r" %
                         [l for l in sl.splitlines() if l.startswith("chattool=")])
        move(CLOSE_X, CLOSE_Y); time.sleep(0.2); click(); time.sleep(0.6)

        # Reminders should now really hold the new row (persisted VFS
        # write, not just a rendered chat bubble).
        click_dock(DOCK_REMINDERS)
        for _ in range(40):
            time.sleep(0.1)
            if is_red(pixel(dump(), CLOSE_X, CLOSE_Y)): break
        else: fails.append("Reminders window never (re)opened from dock slot %d after scenario a" % DOCK_REMINDERS)
        time.sleep(0.4)
        after_ink = ink_count(dump())
        print("Reminders ink after scenario (a): %d (baseline was %d)" % (after_ink, baseline_ink))
        # Not "more ink": "No reminders yet." is a longer string than
        # "buy milk" plus a checkbox glyph, so a real new row can render
        # with FEWER exact-color ink pixels than the empty-list message it
        # replaced. What's discriminating is that the window is no longer
        # bit-identical to the empty state at all -- with chat_run_tool's
        # new_reminder case reverted (returns 0), REMINDERS.TXT is never
        # touched and this same window renders "No reminders yet." again,
        # pixel-for-pixel, giving after_ink == baseline_ink exactly (same
        # deterministic AA text rendering, same boot session).
        if abs(after_ink - baseline_ink) < 20:
            fails.append("scenario a: Reminders window looks unchanged from the empty-list baseline (baseline=%d, after=%d) -- the reminder was never really added" % (baseline_ink, after_ink))
        move(CLOSE_X, CLOSE_Y); time.sleep(0.2); click(); time.sleep(0.6)

        # --- scenario (b): an ordinary question -> pick says null --------
        click_dock(DOCK_CHAT)
        for _ in range(40):
            time.sleep(0.1)
            if is_red(pixel(dump(), CLOSE_X, CLOSE_Y)): break
        else: fails.append("Chat window never (re)opened from dock slot %d for scenario b" % DOCK_CHAT)
        time.sleep(0.4)
        before_b = ink_below(dump(), REPLY_TOP)
        keys("n"); time.sleep(0.5)
        type_msg("what is the capital of france")
        time.sleep(0.3); keys("ret")
        wait_for("chatreply=", 30, "scenario b: no chatreply= marker")
        time.sleep(1.2)
        after_b = dump()
        after_b_ink = ink_below(after_b, REPLY_TOP)
        print("scenario b: reply-band ink before=%d after=%d" % (before_b, after_b_ink))
        if after_b_ink < 200: fails.append("scenario b: Samantha's reply did not render (%d ink px)" % after_b_ink)
        if not state["chat_calls"]: fails.append("scenario b: /api/chat was never called for an ordinary question")
        rl = [l for l in serial().splitlines() if l.startswith("chatreply=")]
        if not rl or "Paris" not in rl[-1]: fails.append("scenario b: chatreply= serial line lacks the fake reply")
        move(CLOSE_X, CLOSE_Y); time.sleep(0.2); click(); time.sleep(0.6)

        # --- scenario (c): "open notes" -> open_app -----------------------
        click_dock(DOCK_CHAT)
        for _ in range(40):
            time.sleep(0.1)
            if is_red(pixel(dump(), CLOSE_X, CLOSE_Y)): break
        else: fails.append("Chat window never (re)opened from dock slot %d for scenario c" % DOCK_CHAT)
        time.sleep(0.4)
        keys("n"); time.sleep(0.5)
        type_msg("open notes")
        time.sleep(0.3); keys("ret")
        wait_for("chattool=open_app:Notes", 20, "scenario c: no chattool=open_app:Notes marker")
        wait_for("editorchrome", 15, "scenario c: Notes never opened (no editorchrome marker)")
        time.sleep(0.6)
        final = dump()
        if not is_red(pixel(final, CLOSE_X, CLOSE_Y)):
            fails.append("scenario c: no window open (red close light absent) after Chat should have handed off to Notes")
        move(CLOSE_X, CLOSE_Y); time.sleep(0.2); click(); time.sleep(0.6)

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
            "-net", "nic,model=rtl8139", "-net", "user"]
    proc = subprocess.Popen(args, stdin=subprocess.PIPE, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        time.sleep(5)
        proc.stdin.write(b"sendkey esc\n"); proc.stdin.flush()
        time.sleep(2)
        proc.stdin.write(send("chat remind me to test the live picker").encode()); proc.stdin.flush()
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
        if l.startswith("chat"):
            print(l)
    print("----------------------------------------------------")
    pick_lines = [l for l in serial_text.splitlines() if l.startswith("chatpick=")]
    if not pick_lines:
        print("NOTE: no chatpick= line at all -- /api/pick was unreachable (network/DNS) or the request failed; "
              "this is informational only, not a failure (the pick is an optimisation, chat_send still ran)")
    elif pick_lines[-1] == "chatpick=new_reminder":
        print("PASS (live): the real Turing picker answered chatpick=new_reminder for \"remind me to test the live picker\"")
    else:
        print("NOTE: the real picker answered %r, not new_reminder -- the live model's own judgement call, not this kernel's bug" % pick_lines[-1])


if __name__ == "__main__":
    main()
