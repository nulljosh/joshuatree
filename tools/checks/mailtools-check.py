#!/usr/bin/env python3
"""1.6.21 ("Samantha reads and sends mail from Chat"): headless proof that
chat_pick's new "os":"jt" flag really unlocks read_mail/send_mail inside
kernel/chat.h's chat_run_tool, against the real Mail store -- not just
that the picker names the tool.

Same shape as tools/checks/chattools-check.py (QMP absolute-pointer click
on a dock slot, send-key typing, framebuffer pmemsave/ink-counting) and
its fake HTTP server answering both POST /api/pick and POST /api/chat,
reached from the guest at 10.0.2.2 via the kernel's llmhost=/llmport=
multiboot override -- turing.heyitsmejosh.com never touched.

Scenarios (one continuous boot, in order):
  (a) "read my email" -> pick answers {"tool":"read_mail","arg":""}.
      Asserts chattool=read_mail: fires on serial (the compiled-in seed
      message "Welcome to Mail" is the newest at boot) and its subject
      renders as ink in the Chat reply band, with /api/chat never called.
  (b) "email joshua tree saying the tools shipped" -> pick answers
      {"tool":"send_mail","arg":"joshua tree"}. Asserts chattool=send_mail:
      fires, then closes Chat, opens Mail from its dock slot and confirms
      the window's ink no longer matches the baseline captured before this
      scenario ran (a real MAIL.TXT append, not just a rendered reply) --
      the same "different from the known baseline" check scenario (a) in
      chattools-check.py already trusts for a persisted write.

Discriminating: with the "os":"jt" flag reverted or chat_run_tool's
read_mail/send_mail cases removed, the picker (scripted here to answer by
substring match regardless of the flag) still names the tool, but a real
kernel without this branch falls through to chat_send instead --
chattool=read_mail:/chattool=send_mail: never fire, /api/chat IS called,
and Mail's ink after scenario (b) is bit-identical to the baseline. This
script fails by name on each of those.

Usage: python3 tools/checks/mailtools-check.py
"""
import http.server, json, os, socket, subprocess, sys, threading, time
from PIL import Image
from freeport import free_port

LOG = "/tmp/jt-mailtools-serial.log"; DUMP = "/tmp/jt-mailtools.raw"
FB = 0xfd000000; W, H = 1920, 1080; PORT = free_port()0
LOGICAL_W, LOGICAL_H, SCALE = 960, 540, 2
DOCK_ICON, DOCK_GAP, SLOT0_X = 37, 6, 247; PITCH = DOCK_ICON + DOCK_GAP; ICON_ROW_Y = 487
CLOSE_X, CLOSE_Y = 94, 56; CLOSE_RED = (0xFF, 0x5F, 0x57)
VX, VY, VW, VH = 78, 72, 804, 345
INK = (0x1C, 0x1C, 0x1E)
QROW_TOP = VY + (-32 + 76)
REPLY_TOP = QROW_TOP + 20

DOCK_MAIL, DOCK_CHAT = 2, 7  # GUI_DOCK_DEFAULT slots: Apps,Files,Mail,Calendar,Notes,Reminders,Terminal,Chat,...

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

        def open_app(slot, label):
            click_dock(slot)
            for _ in range(200):
                time.sleep(0.1)
                if is_red(pixel(dump(), CLOSE_X, CLOSE_Y)): return True
            fails.append("%s window never opened from dock slot %d" % (label, slot))
            return False

        def close_app():
            move(CLOSE_X, CLOSE_Y); time.sleep(0.2); click(); time.sleep(0.6)

        for _ in range(120):
            if pixel(dump(), 480, 511) == (0xEF, 0xEB, 0xE4): break
            time.sleep(0.25)
        else: raise SystemExit("FAIL: desktop never appeared")
        time.sleep(0.5)

        # --- baseline: Mail's ink before scenario (b) ever sends anything
        if open_app(DOCK_MAIL, "Mail"):
            time.sleep(0.4)
            mail_baseline_ink = ink_count(dump())
            print("Mail ink before scenario (b): %d" % mail_baseline_ink)
            close_app()

        # --- scenario (a): "read my email" -> read_mail -------------------
        if open_app(DOCK_CHAT, "Chat"):
            time.sleep(0.4)
            keys("n"); time.sleep(0.5)
            type_msg("read my email")
            time.sleep(0.3); keys("ret")
            wait_for("chattool=read_mail:", 20, "scenario a: no chattool=read_mail: marker")
            time.sleep(1.0)
            reply_ink = ink_below(dump(), REPLY_TOP)
            print("scenario a: chat reply-band ink = %d" % reply_ink)
            if reply_ink < 100: fails.append("scenario a: read_mail reply did not render in the Chat body (%d ink px)" % reply_ink)
            if state["chat_calls"]: fails.append("scenario a: /api/chat was called even though the picker named read_mail")
            sl = serial()
            if "chattool=read_mail:" not in sl:
                fails.append("scenario a: chattool=read_mail: marker missing: %r" % [l for l in sl.splitlines() if l.startswith("chattool=")])
            elif "chattool=read_mail:none" in sl:
                fails.append("scenario a: read_mail reported an empty inbox, but the seed messages should still be present")
            else:
                print("scenario a: read_mail listed a real message: %r" % [l for l in sl.splitlines() if l.startswith("chattool=read_mail:")][-1])
            close_app()

        # --- scenario (b): "email joshua tree saying ..." -> send_mail ----
        state["chat_calls"] = []
        if open_app(DOCK_CHAT, "Chat"):
            time.sleep(0.4)
            keys("n"); time.sleep(0.5)
            type_msg("email joshua tree saying the tools shipped")
            time.sleep(0.3); keys("ret")
            wait_for("chattool=send_mail:", 20, "scenario b: no chattool=send_mail: marker")
            time.sleep(0.6)
            if state["chat_calls"]: fails.append("scenario b: /api/chat was called even though the picker named send_mail")
            sl = serial()
            if "chattool=send_mail:joshua tree" not in sl:
                fails.append("scenario b: chattool=send_mail: marker missing or wrong arg: %r" % [l for l in sl.splitlines() if l.startswith("chattool=")])
            close_app()

        # Mail should now really hold the new message (persisted MAIL.TXT
        # write, not just a rendered chat bubble).
        if open_app(DOCK_MAIL, "Mail"):
            time.sleep(0.4)
            mail_after_ink = ink_count(dump())
            print("Mail ink after scenario (b): %d (baseline was %d)" % (mail_after_ink, mail_baseline_ink))
            if abs(mail_after_ink - mail_baseline_ink) < 5:
                fails.append("scenario b: Mail window looks unchanged from the baseline (baseline=%d, after=%d) -- the message was never really sent" % (mail_baseline_ink, mail_after_ink))
            close_app()

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
