#!/usr/bin/env python3
"""Headless proof for the notes/reminders tools in Samantha's run_tool
(user/samantha.c, the ring-3 window; kernel/chat.h's chat_run_tool before
2.0): list_reminders and read_notes.

2.0.0: boots with the `samantha` flag (her window opens first), types
straight into her input bar (no `n` key) and reads her markers behind the
kernel's "syscall: write(1) from ring 3: " serial prefix. Same shape as
tools/checks/mailtools-check.py: send-key typing, serial-log assertions,
against a fake HTTP server standing in for Turing's /api/pick and /api/chat.

Scenarios (one continuous boot, in order):
  (a) "remind me to buy milk" -> pick answers {"tool":"new_reminder","arg":"buy milk"}.
      Seeds a real reminder via the already-shipped new_reminder path.
  (b) "what are my reminders" -> pick answers {"tool":null,"arg":""} (the
      picker doesn't know list_reminders yet), so this exercises the local
      keyword fallback (keyword_fallback) rather than the picker. Asserts
      chattool=list_reminders: fires and its text includes "buy milk", with
      /api/chat never called.
  (c) "what's in my notes" -> pick also answers {"tool":null,"arg":""}.
      Asserts chattool=read_notes: fires with the compiled-in seed note
      (the demo notes written to NOTES.TXT at first boot), again via the
      keyword fallback, with /api/chat never called.

Discriminating: with run_tool's list_reminders/read_notes cases removed, or
keyword_fallback deleted/not wired into send(), scenarios (b)/(c) fall
through to /api/chat instead -- chattool=list_reminders:/chattool=read_notes:
never fire and /api/chat IS called. This script fails by name on each of those.

Usage: python3 tools/checks/notestools-check.py
"""
from freeport import free_port
import http.server, json, os, socket, subprocess, sys, threading, time

LOG = "/tmp/jt-notestools-serial.log"
PORT = free_port()

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

        def keys(*qc): cmd({"execute": "send-key", "arguments": {"keys": [{"type": "qcode", "data": k} for k in qc]}})

        def type_msg(msg):
            names = {" ": "spc", "'": "apostrophe"}
            for ch in msg:
                keys(names.get(ch, ch)); time.sleep(0.05)
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

        def marker_lines(text, marker):
            return [l[l.index(marker):] for l in text.splitlines() if marker in l]

        # her window drew and focused its input bar; give the first keys a clear runway
        wait_for("samfocus", 40, "no samfocus marker: Samantha's window never opened")
        time.sleep(2.0)

        # --- scenario (a): seed a reminder ---------------------------------
        mark = len(serial())
        type_msg("remind me to buy milk")
        wait_for("chattool=new_reminder:", 20, "scenario a: no chattool=new_reminder: marker", after=mark)
        time.sleep(1.0)

        # --- scenario (b): "what are my reminders" -> list_reminders,
        #     via the local keyword fallback (pick answers tool:null) -----
        state["chat_calls"] = []
        mark = len(serial())
        type_msg("what are my reminders")
        wait_for("chattool=list_reminders:", 20, "scenario b: no chattool=list_reminders: marker", after=mark)
        time.sleep(0.3)
        if state["chat_calls"]: fails.append("scenario b: /api/chat was called even though the fallback named list_reminders")
        lines = marker_lines(serial()[mark:], "chattool=list_reminders:")
        if not lines or "buy milk" not in lines[-1]:
            fails.append("scenario b: chattool=list_reminders: marker missing or missing 'buy milk': %r" % lines)
        time.sleep(1.0)

        # --- scenario (c): "what's in my notes" -> read_notes, via the
        #     local keyword fallback ------------------------------------
        state["chat_calls"] = []
        mark = len(serial())
        type_msg("what's in my notes")
        wait_for("chattool=read_notes:", 20, "scenario c: no chattool=read_notes: marker", after=mark)
        time.sleep(0.3)
        if state["chat_calls"]: fails.append("scenario c: /api/chat was called even though the fallback named read_notes")
        if "chattool=read_notes:none" in serial()[mark:]:
            fails.append("scenario c: read_notes reported no notes, but the compiled-in seed note should still be present")

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
