#!/usr/bin/env python3
"""Headless proof for the notes/reminders tools this pass adds to
kernel/chat.h's chat_run_tool: list_reminders and read_notes.

Same shape as tools/checks/mailtools-check.py: QMP absolute-pointer click
on a dock slot, send-key typing, serial-log assertions, reached against a
fake HTTP server standing in for Turing's /api/pick and /api/chat.

Scenarios (one continuous boot, in order):
  (a) "remind me to buy milk" -> pick answers {"tool":"new_reminder","arg":"buy milk"}.
      Seeds a real reminder via the already-shipped new_reminder path.
  (b) "what are my reminders" -> pick answers {"tool":null,"arg":""} (the
      picker doesn't know list_reminders yet), so this exercises the new
      kernel-local keyword fallback (chat_keyword_fallback) rather than
      the picker. Asserts chattool=list_reminders: fires and its text
      includes "buy milk", with /api/chat never called.
  (c) "what's in my notes" -> pick also answers {"tool":null,"arg":""}.
      Asserts chattool=read_notes: fires with the compiled-in seed note
      (kernel.c's demo_notes, written to NOTES.TXT at first boot), again
      via the keyword fallback, with /api/chat never called.

Discriminating: with chat_run_tool's list_reminders/read_notes cases
removed, or chat_keyword_fallback deleted/not wired into
chat_process_message, scenarios (b)/(c) fall through to chat_send instead
-- chattool=list_reminders:/chattool=read_notes: never fire and
/api/chat IS called. This script fails by name on each of those.

Usage: python3 tools/checks/notestools-check.py
"""
from freeport import free_port
import http.server, json, os, socket, subprocess, sys, threading, time

LOG = "/tmp/jt-notestools-serial.log"
PORT = free_port()

DOCK_CHAT = 7  # GUI_DOCK_DEFAULT slots: Apps,Files,Mail,Calendar,Notes,Reminders,Terminal,Chat,...
DOCK_ICON, DOCK_GAP, SLOT0_X = 37, 6, 247
PITCH = DOCK_ICON + DOCK_GAP
ICON_ROW_Y = 487
LOGICAL_W, LOGICAL_H = 960, 540

PICK_ANSWERS = [
    ("buy milk", {"tool": "new_reminder", "arg": "buy milk"}),
    ("my reminders", {"tool": None, "arg": ""}),
    ("my notes", {"tool": None, "arg": ""}),
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

    try: os.remove(LOG)
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
            cmd({"execute": "pmemsave", "arguments": {"val": 0xfd000000, "size": 1920 * 1080 * 4, "filename": "/tmp/jt-notestools.raw"}})

        def pixel(x, y):
            from PIL import Image
            dump()
            img = Image.frombytes("RGBA", (1920, 1080), open("/tmp/jt-notestools.raw", "rb").read(), "raw", "BGRA").convert("RGB")
            return img.getpixel((x * 2 + 1, y * 2 + 1))

        def is_red(p): return max(abs(p[i] - v) for i, v in enumerate((0xFF, 0x5F, 0x57))) <= 12

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

        def open_chat():
            click_dock(DOCK_CHAT)
            for _ in range(200):
                time.sleep(0.1)
                if is_red(pixel(94, 56)): return True
            fails.append("Chat window never opened from dock slot %d" % DOCK_CHAT)
            return False

        def close_app():
            move(94, 56); time.sleep(0.2); click(); time.sleep(0.6)

        for _ in range(120):
            if pixel(480, 511) == (0xEF, 0xEB, 0xE4): break
            time.sleep(0.25)
        else: raise SystemExit("FAIL: desktop never appeared")
        time.sleep(0.5)

        # --- scenario (a): seed a reminder ---------------------------------
        if open_chat():
            time.sleep(0.4)
            keys("n"); time.sleep(0.5)
            type_msg("remind me to buy milk")
            time.sleep(0.3); keys("ret")
            wait_for("chattool=new_reminder:", 20, "scenario a: no chattool=new_reminder: marker")
            close_app()

        # --- scenario (b): "what are my reminders" -> list_reminders,
        #     via the local keyword fallback (pick answers tool:null) -----
        state["chat_calls"] = []
        if open_chat():
            time.sleep(0.4)
            keys("n"); time.sleep(0.5)
            type_msg("what are my reminders")
            time.sleep(0.3); keys("ret")
            wait_for("chattool=list_reminders:", 20, "scenario b: no chattool=list_reminders: marker")
            time.sleep(0.3)
            sl = serial()
            if state["chat_calls"]: fails.append("scenario b: /api/chat was called even though the fallback named list_reminders")
            lines = [l for l in sl.splitlines() if l.startswith("chattool=list_reminders:")]
            if not lines or "buy milk" not in lines[-1]:
                fails.append("scenario b: chattool=list_reminders: marker missing or missing 'buy milk': %r" % lines)
            close_app()

        # --- scenario (c): "what's in my notes" -> read_notes, via the
        #     local keyword fallback ------------------------------------
        state["chat_calls"] = []
        if open_chat():
            time.sleep(0.4)
            keys("n"); time.sleep(0.5)
            type_msg("what's in my notes")
            time.sleep(0.3); keys("ret")
            wait_for("chattool=read_notes:", 20, "scenario c: no chattool=read_notes: marker")
            time.sleep(0.3)
            sl = serial()
            if state["chat_calls"]: fails.append("scenario c: /api/chat was called even though the fallback named read_notes")
            if "chattool=read_notes:none" in sl:
                fails.append("scenario c: read_notes reported no notes, but the compiled-in seed note should still be present")
            close_app()

    finally:
        q.kill(); q.wait()
        try: srv.shutdown()
        except Exception: pass

    if fails:
        print("FAIL:")
        for x in fails: print("  -", x)
        sys.exit(1)
    print("PASS: \"what are my reminders\" and \"what's in my notes\" both work via chat_keyword_fallback "
          "(list_reminders/read_notes), with /api/chat never called")


if __name__ == "__main__":
    main()
