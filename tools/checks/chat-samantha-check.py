#!/usr/bin/env python3
# 1.0.12 (direct owner request, "hook Chat up to our Samantha LLM, the
# Turing project"): headless proof that Chat's compiled-in defaults
# (kernel.c's llm_model/llm_host/llm_port) really point at the Turing
# project's Ollama-compatible /api/chat, that chat_send (kernel/chat.h)
# builds the exact request shape Turing expects, and that a host forcing
# HTTPS -- which this kernel cannot speak, no TLS anywhere in this stack
# -- surfaces a clear, specific status instead of a silent/generic
# failure. Same shape as tools/checks/chat-live-check.sh (real network,
# not runnable in CI or this sandbox) and tools/checks/weather-app-check.sh
# (a local fake server standing in for the real host, driven the same
# "-net nic,model=rtl8139 -net user -append wxhost=..." way): never
# touches turing.heyitsmejosh.com for real, and never needs to -- the
# real endpoint is proven live by .github/workflows/check.yml's `network`
# job instead (a real GitHub runner, real internet, this sandbox's
# outbound proxy blocks that host).
#
# There is no wxhost=-style override for Chat's own llm_host/llm_port
# before this pass (only Settings persists them, and Settings has no
# boot-time/headless input path this check could drive without a real
# mouse), so kernel.c's kmain gained a matching llmhost=/llmport=
# multiboot command-line override, applied AFTER settings_load() runs so
# a stale/persisted SETTINGS.TXT can never silently win over it -- see
# that override's own comments in kernel.c for why the ordering matters.
#
# Two scenarios, one fake local HTTP server per boot:
#   reply    the fake server answers exactly Turing's real reply shape
#            ({"model":"samantha","message":{"role":"assistant","content":
#            "..."},"done":true}); asserts the RECORDED request the kernel
#            actually sent is Ollama-shaped with "model":"samantha" and
#            the typed question as the newest (and only) user message,
#            and that the fake reply's content really came back over
#            serial's `chatreply=` line (chat.h's own real-fetch-proof
#            marker, same convention weather_fetch's wx=/geo= lines use).
#   redirect the fake server answers 301 (the exact shape a host forcing
#            HTTPS gives a plain-HTTP client), no JSON body at all;
#            asserts the kernel's own `chathttps=1` serial marker fires
#            (chat_send's new http_last_status() check in kernel/chat.h)
#            instead of a bare, generic failure or a hang.
#
# Discriminating both ways (see the PR this shipped in for the actual
# revert-and-rerun proof): against a kernel still defaulting to
# qwen3:8b/10.0.2.2:11434, the recorded request's "model" field would
# read "qwen3:8b", not "samantha" -- the `reply` scenario's own model
# assertion catches that even though this check forces host/port through
# the command line either way. Against a kernel with the 301/302/307/308
# check removed from chat_send, the `redirect` scenario never emits
# `chathttps=1` and instead just times out waiting for it (json_extract_string
# finds no "content" field in a redirect's body, the same silent "no
# reply" every other network failure gave before this pass).
import http.server, json, os, subprocess, sys, tempfile, threading, time

QUESTION = "tell me a fact about bananas"  # letters and spaces only -- keeps the QEMU sendkey mapping below trivial (no punctuation table needed)
REPLY_CONTENT = "potassium42 is the real reason bananas are famous"  # a unique marker, easy to grep off the chatreply= serial line


def make_server(mode_holder, recorded):
    class H(http.server.BaseHTTPRequestHandler):
        def log_message(self, *a):
            pass

        def do_POST(self):
            length = int(self.headers.get("Content-Length", "0"))
            body = self.rfile.read(length)
            recorded["path"] = self.path
            recorded["body"] = body
            recorded["content_type"] = self.headers.get("Content-Type", "")
            if mode_holder["mode"] == "redirect":
                self.send_response(301)
                self.send_header("Location", "https://turing.heyitsmejosh.com/api/chat")
                self.send_header("Content-Length", "0")
                self.end_headers()
                return
            reply = json.dumps({
                "model": "samantha",
                "message": {"role": "assistant", "content": REPLY_CONTENT},
                "done": True,
            }).encode()
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(reply)))
            self.end_headers()
            self.wfile.write(reply)

    srv = http.server.ThreadingHTTPServer(("127.0.0.1", 0), H)
    srv.daemon_threads = True
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    return srv


def send(s):
    # Same mapping tools/checks/chat-live-check.sh's own send() uses: a
    # bare `sendkey ret` doesn't reliably deliver Enter to real interactive
    # text entry per CLAUDE.md, but this exact monitor-stdio pattern is the
    # established, working precedent for the shell's own `chat <message>`
    # command (chat-live-check.sh already proves it against a real Ollama
    # host the same way).
    lines = []
    for c in s:
        lines.append("sendkey spc" if c == " " else "sendkey %s" % c)
    lines.append("sendkey ret")
    return "\n".join(lines) + "\n"


def boot_and_ask(name, append, wait_secs):
    """Boots kernel.elf headless, drops to the shell, types `chat QUESTION`,
    polls serial for a chatreply=/chathttps= marker up to wait_secs, quits,
    and returns the full serial log. Shared by the fake-server scenarios
    below and --live's real-network run (append=None there: no override,
    so the kernel's own compiled-in defaults -- turing.heyitsmejosh.com,
    port 80, model samantha -- are what's actually exercised)."""
    workdir = tempfile.mkdtemp(prefix="jt-chatsam-" + name + "-")
    log = os.path.join(workdir, "serial.log")

    args = ["qemu-system-i386", "-kernel", "kernel.elf", "-display", "none",
            "-monitor", "stdio", "-serial", "file:" + log,
            "-net", "nic,model=rtl8139", "-net", "user"]
    if append:
        args += ["-append", append]

    proc = subprocess.Popen(args, stdin=subprocess.PIPE, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        time.sleep(5)
        proc.stdin.write(b"sendkey esc\n")
        proc.stdin.flush()
        time.sleep(2)
        proc.stdin.write(send("chat " + QUESTION).encode())
        proc.stdin.flush()
        for _ in range(wait_secs):
            time.sleep(1)
            try:
                text = open(log, "r", encoding="latin-1").read()
            except FileNotFoundError:
                text = ""
            if "chatreply=" in text or "chathttps=" in text:
                break
        proc.stdin.write(b"quit\n")
        proc.stdin.flush()
    finally:
        try:
            proc.stdin.close()
        except Exception:
            pass
        try:
            proc.wait(timeout=10)
        except Exception:
            proc.kill()
            proc.wait(timeout=5)

    try:
        return open(log, "r", encoding="latin-1").read()
    except FileNotFoundError:
        return ""


def run_scenario(name, mode):
    recorded = {}
    mode_holder = {"mode": mode}
    srv = make_server(mode_holder, recorded)
    port = srv.server_address[1]
    try:
        serial = boot_and_ask(name, "llmhost=10.0.2.2 llmport=%d" % port, wait_secs=30)
    finally:
        try:
            srv.shutdown()
        except Exception:
            pass
    return serial, recorded


def run_live():
    # No -append override at all: the kernel's own compiled-in defaults
    # (turing.heyitsmejosh.com, port 80, model samantha) are what's really
    # exercised here, over the runner's real internet, real DNS, real TLS-
    # or-not answer. Longer timeout than the fake-server scenarios: real
    # DNS + a real cross-internet round trip (plus whatever Turing's own
    # model takes to answer) is legitimately slower than a loopback fetch.
    serial = boot_and_ask("live", None, wait_secs=90)
    print("---- live serial (chat* lines only) ----")
    for l in serial.splitlines():
        if l.startswith("chat") or l.startswith("nettest") or l.startswith("wxfetch"):
            print(l)
    print("-----------------------------------------")
    if "chathttps=1" in serial:
        print("FAIL: turing.heyitsmejosh.com forces HTTPS -- this kernel speaks HTTP only "
              "(chat_send's own chathttps=1 marker fired, the exact status Chat now shows in "
              "Settings/the shell instead of a blank or garbage reply)")
        sys.exit(1)
    reply_lines = [l for l in serial.splitlines() if l.startswith("chatreply=")]
    if not reply_lines:
        print("FAIL: no chatreply= and no chathttps=1 on serial -- turing.heyitsmejosh.com:80 "
              "never answered within the timeout (network unreachable, DNS failure, or a real hang)")
        sys.exit(1)
    print("PASS: turing.heyitsmejosh.com:80 answered a real POST /api/chat over plain HTTP:")
    print("  " + reply_lines[-1])


def main():
    os.chdir(os.path.join(os.path.dirname(__file__), "..", ".."))
    subprocess.run(["make", "-s", "kernel.elf"], check=True)

    if "--live" in sys.argv:
        run_live()
        return

    failures = []

    # --- scenario 1: a real Ollama/Turing-shaped reply -------------------
    serial, recorded = run_scenario("reply", "ok")
    if os.environ.get("CHATSAM_DEBUG"):
        print("---- reply scenario serial ----")
        print(serial)
        print("--------------------------------")

    if recorded.get("path") != "/api/chat":
        failures.append("fake server never saw a POST to /api/chat (path was %r)" % recorded.get("path"))

    ct = (recorded.get("content_type") or "").lower()
    if not ct.startswith("application/json"):
        failures.append("request content-type was %r, not application/json" % recorded.get("content_type"))

    body = recorded.get("body")
    if not body:
        failures.append("fake server recorded no request body at all -- chat_send never sent one")
    else:
        try:
            parsed = json.loads(body.decode("utf-8"))
        except Exception as e:
            parsed = None
            failures.append("request body is not valid JSON: %r (%s)" % (body[:200], e))
        if parsed is not None:
            if parsed.get("model") != "samantha":
                failures.append("request \"model\" is %r, not \"samantha\" -- the compiled-in default did not take effect" % parsed.get("model"))
            if parsed.get("stream") is not False:
                failures.append("request \"stream\" is %r, not false" % parsed.get("stream"))
            messages = parsed.get("messages") or []
            if not messages:
                failures.append("request has no \"messages\" array")
            else:
                newest = messages[-1]
                if newest.get("role") != "user":
                    failures.append("newest message role is %r, not \"user\"" % newest.get("role"))
                if newest.get("content") != QUESTION:
                    failures.append("newest message content is %r, not the typed question %r" % (newest.get("content"), QUESTION))

    reply_lines = [l for l in serial.splitlines() if l.startswith("chatreply=")]
    if not reply_lines:
        failures.append("no chatreply= line on serial -- the fake reply's content never made it back through chat_send")
    elif REPLY_CONTENT not in reply_lines[-1]:
        failures.append("chatreply= line does not contain the fake server's own reply content: %r" % reply_lines[-1])

    # --- scenario 2: the host forces HTTPS (a real 301) ------------------
    serial2, recorded2 = run_scenario("redirect", "redirect")
    if os.environ.get("CHATSAM_DEBUG"):
        print("---- redirect scenario serial ----")
        print(serial2)
        print("-----------------------------------")

    if recorded2.get("path") != "/api/chat":
        failures.append("redirect scenario: fake server never saw a POST to /api/chat (path was %r)" % recorded2.get("path"))
    if "chathttps=1" not in serial2:
        failures.append("redirect scenario: no chathttps=1 marker on serial -- the HTTPS-redirect status did not surface")
    if "chatreply=" in serial2:
        failures.append("redirect scenario: a chatreply= line appeared even though the server only ever answered 301 -- content must not be invented")

    if failures:
        print("FAIL:")
        for f in failures:
            print("  - " + f)
        sys.exit(1)

    print("PASS: Chat's compiled-in defaults really reach an Ollama/Turing-shaped /api/chat with model \"samantha\" "
          "and the typed question as the newest user message (chatreply= proves the fake reply's real content came back), "
          "and a host that forces HTTPS surfaces a clear chathttps=1 status instead of a silent failure")


if __name__ == "__main__":
    main()
