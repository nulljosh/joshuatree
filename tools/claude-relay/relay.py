#!/usr/bin/env python3
"""claude-relay: the small HTTP bridge between Joshua Tree's Claude app and Claude Code.

    !!! THIS IS A COMMAND-CAPABLE AGENT BEHIND AN HTTP PORT. !!!

    Every request that carries the right token runs `claude -p` (headless Claude Code) on
    THIS machine, as THIS user, logged in to THIS user's Claude plan. Treat the token like
    a password for your account. Anyone holding it can make Claude read the files under
    --cwd (the repo by default) and spend your plan's usage.

Joshua Tree has no TLS and no Node, so it cannot run Claude Code itself. It can send one
plain HTTP POST (SYS_HTTP_POST). This relay takes that POST and does the Claude part:

    POST /api/claude
    Authorization: Bearer <token>
    Content-Type: application/json      {"prompt": "...", "session": "<uuid or empty>"}
      (or text/plain: the whole body is the prompt, session from the X-Claude-Session header)

    200 text/plain:   S <session uuid>\\n<reply text>
    401 no or wrong token          404 any other path        405 not POST
    411 no Content-Length          413 body over the cap     400 bad JSON, empty prompt, bad session
    429 a request is already running (one at a time)        504 claude ran past --timeout
    502 claude failed or answered something unreadable

What it locks down, and why:
  - Binds 127.0.0.1 unless --lan is given. QEMU's user network reaches the host's loopback
    at 10.0.2.2, so the emulator needs no LAN exposure at all. --lan (a Pi or a second
    machine) binds 0.0.0.0 and prints a warning, because the token then crosses the LAN in
    plain HTTP and anyone who can sniff it can replay it.
  - Refuses to start without a token of 16 to 63 characters (Joshua Tree keeps it in a
    64-byte kernel buffer; the app never sees it, the kernel adds the header itself).
    The token comes from --token-file or the CLAUDE_RELAY_TOKEN environment variable,
    never from the command line, so it does not show up in `ps`.
  - Compares the token in constant time. Never logs it, never logs a prompt or a reply:
    the log line is method, path, status, byte counts and seconds.
  - Read-only tools by default: `--tools Read Grep Glob`, path-scoped allow rules for
    the working directory only, `--permission-mode dontAsk` (anything else is denied, not
    asked), `--strict-mcp-config` (none of your MCP servers load) and
    `--setting-sources project` (your user hooks and plugins do not load).
    Read can still see every file under --cwd, including anything secret you keep there.
  - The prompt goes to claude on stdin, never in argv: no `ps` leak, and a prompt that
    starts with "-" cannot become a flag. The session id must be a UUID before it gets
    anywhere near --resume.
  - Bodies over --max-body bytes are refused before they are read. One request at a
    time; a second gets 429 instead of queueing. Each run has a hard --timeout, after which
    claude's whole process group is killed.
  - The reply is cut to fit Joshua Tree's 8 KB reply buffer and turned into plain ASCII
    (its fonts have no curly quotes or dashes).

Run it (from the repo root):
    printf '%s' "$(openssl rand -hex 24)" > ~/.claude-relay-token && chmod 600 ~/.claude-relay-token
    python3 tools/claude-relay/relay.py --token-file ~/.claude-relay-token
Then in Joshua Tree: Settings > Assistant > Claude relay = 10.0.2.2:8765, Claude token = that file's text.

Python standard library only. tools/checks/claude-relay-check.py proves the rules above
against a stub `claude`.
"""
import argparse, hmac, http.server, json, os, re, signal, subprocess, sys, threading, time, urllib.request, urllib.error

DEFAULT_PORT = 8765
DEFAULT_TIMEOUT = 150          # seconds; the kernel waits up to 240 s (JT_HTTP_POST_TICKS_CLAUDE), so the relay's 504 lands first
DEFAULT_MAX_BODY = 4096        # the app's prompt is at most a few hundred bytes
REPLY_MAX = 8191               # JT_HTTP_POST_REPLY_MAX (8192) minus the app's NUL
TOKEN_MIN, TOKEN_MAX = 16, 63  # 63: the kernel keeps it in a 64-byte buffer
SESSIONS_MAX = 64
UUID_RE = re.compile(r"^[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12}$")
READ_ONLY_TOOLS = ["Read", "Grep", "Glob"]
ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

# Joshua Tree's fonts are ASCII (plus the degree sign); map the punctuation Claude likes.
ASCII_MAP = {
    "‘": "'", "’": "'", "‚": "'", "‛": "'", "“": '"', "”": '"', "„": '"',
    "–": "-", "—": "-", "―": "-", "−": "-", "…": "...", "•": "*", "·": "*",
    "→": "->", "←": "<-", "⇒": "=>", "≤": "<=", "≥": ">=", "≠": "!=", " ": " ",
    "✓": "v", "✔": "v", "✗": "x", "✘": "x", "×": "x", "\t": "    ",
}


def to_ascii(text):
    out = []
    for ch in text:
        if ch in ASCII_MAP: out.append(ASCII_MAP[ch])
        elif ch == "\n" or 32 <= ord(ch) < 127: out.append(ch)
        elif ch == "°": out.append(ch)       # the one non-ASCII glyph the fonts have
        elif ch == "\r": continue
        else: out.append("?")
    return "".join(out)


def claude_argv(cfg, session):
    """The exact command line one request runs. The prompt is NOT here: it goes on stdin."""
    argv = [cfg.claude, "-p", "--output-format", "json"]
    argv += ["--tools", *cfg.tools]
    argv += ["--allowedTools", *[t + "(./**)" for t in cfg.tools]]
    argv += ["--permission-mode", "dontAsk", "--strict-mcp-config", "--setting-sources", "project"]
    if cfg.model: argv += ["--model", cfg.model]
    if session: argv += ["--resume", session]
    return argv


SAMANTHA = ("You are Samantha, the assistant inside Joshua Tree, a small operating system built from scratch "
            "that runs on a Raspberry Pi. You answer at its console. Be warm, plain and brief: a few short "
            "sentences, no markdown, no lists unless asked. You can also act on the Pi: end your answer with an "
            "action, alone on its own line, under 80 characters. [[note TEXT]] prints TEXT on the Pi's console. "
            "[[led blink]] blinks the Pi's green light once. These two are the only actions; use one only when it "
            "helps, never invent others.")


TOP_WORDS = ("architecture", "design a", "prove", "security", "tradeoff", "step by step plan")
HARD_WORDS = ("fix", "bug", "why", "explain", "write", "design", "debug", "compare", "plan", "code")


def pick_model(cfg, prompt):
    """A short, easy question goes to the cheap model; a long one or one that asks for real work goes to the strong one.
    Both names are flags, so there is nothing to watch: --api-model (cheap) and --api-model-hard."""
    low = prompt.lower()
    if any(w in low for w in TOP_WORDS): return cfg.api_model_top
    hard = len(prompt) > 280 or any(w in low for w in HARD_WORDS)
    return cfg.api_model_hard if hard else cfg.api_model


FILES_DIR = os.path.expanduser("~/pi-files")   # the only folder the model can look in
TOOLS = [
    {"name": "list_files", "description": "List the files in the shared folder.",
     "input_schema": {"type": "object", "properties": {}}},
    {"name": "read_file", "description": "Read one text file from the shared folder by its plain name.",
     "input_schema": {"type": "object", "properties": {"name": {"type": "string"}}, "required": ["name"]}},
]


def run_tool(name, args):
    """Read-only, one folder, plain file names only: no paths, no dot files, no links, 20000 characters at most."""
    try:
        files = sorted(f for f in os.listdir(FILES_DIR) if not f.startswith(".")
                       and os.path.isfile(os.path.join(FILES_DIR, f)) and not os.path.islink(os.path.join(FILES_DIR, f)))
    except OSError:
        return "The shared folder does not exist."
    if name == "list_files": return "\n".join(files) or "(empty)"
    if name == "read_file":
        n = args.get("name", "") if isinstance(args, dict) else ""
        if n not in files: return "No such file. Use list_files first."
        with open(os.path.join(FILES_DIR, n), errors="replace") as fh: return fh.read(20000)
    return "Unknown tool."


def api_call(cfg, key, model, messages, system=SAMANTHA):
    body = json.dumps({"model": model, "max_tokens": 600, "system": system, "tools": TOOLS, "messages": messages}).encode()
    req = urllib.request.Request("https://api.anthropic.com/v1/messages", data=body, method="POST", headers={
        "x-api-key": key, "anthropic-version": "2023-06-01", "content-type": "application/json"})
    with urllib.request.urlopen(req, timeout=cfg.timeout) as r:
        return json.loads(r.read().decode("utf-8", "replace"))


def run_api(cfg, prompt, pi=""):
    """--api-key-file mode: the Messages API with two read-only file tools, paid from the Claude Platform credit, not
    the Claude Code plan. Stateless: each question stands alone, at most 4 model turns. Returns (status, text)."""
    try:
        key = open(os.path.expanduser(cfg.api_key_file)).read().strip()
    except OSError:
        return 502, "The relay has no API key file."
    model = pick_model(cfg, prompt)
    system = SAMANTHA + (" Live status of the Pi you run on (ip, clock in UTC seconds since 1970, Wi-Fi bars): " + pi if pi else "")
    messages = [{"role": "user", "content": prompt}]
    try:
        for _ in range(4):
            d = api_call(cfg, key, model, messages, system)
            blocks = d.get("content", [])
            if d.get("stop_reason") != "tool_use": break
            messages.append({"role": "assistant", "content": blocks})
            results = [{"type": "tool_result", "tool_use_id": b["id"], "content": run_tool(b.get("name"), b.get("input"))}
                       for b in blocks if b.get("type") == "tool_use"]
            messages.append({"role": "user", "content": results})
    except urllib.error.HTTPError as e:
        return 502, "The API said %d." % e.code
    except (urllib.error.URLError, OSError, ValueError, KeyError):
        return 502, "The API did not answer."
    text = "".join(b.get("text", "") for b in d.get("content", []) if isinstance(b, dict))
    return (200, "S -\n" + text) if text else (502, "The API returned no text.")


class Relay:
    def __init__(self, cfg, token):
        self.cfg = cfg
        self.token = token.encode()
        self.busy = threading.Lock()
        self.sessions = set()   # session ids this relay has handed out; --resume only accepts these

    def run_claude(self, prompt, session, pi=""):
        """Returns (status, text). Kills claude's whole process group past the timeout."""
        if self.cfg.api_key_file: return run_api(self.cfg, prompt, pi)
        p = subprocess.Popen(claude_argv(self.cfg, session), cwd=self.cfg.cwd, stdin=subprocess.PIPE,
                             stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, start_new_session=True)
        try:
            out, _ = p.communicate(prompt.encode("utf-8"), timeout=self.cfg.timeout)
        except subprocess.TimeoutExpired:
            try: os.killpg(p.pid, signal.SIGKILL)
            except ProcessLookupError: pass
            p.communicate()
            return 504, "Claude took longer than %d seconds. Try a smaller question." % self.cfg.timeout
        try:
            d = json.loads(out.decode("utf-8", "replace"))
        except ValueError:
            return 502, "Claude Code did not answer (exit %d). Is it installed and logged in on the relay machine?" % p.returncode
        text = d.get("result") if isinstance(d, dict) else None
        sid = d.get("session_id", "") if isinstance(d, dict) else ""
        if not isinstance(text, str) or (d.get("is_error") and not text):
            return 502, "Claude Code returned an error."
        if not (isinstance(sid, str) and UUID_RE.match(sid)): sid = ""
        if sid:
            if len(self.sessions) >= SESSIONS_MAX: self.sessions.clear()
            self.sessions.add(sid)
        return 200, "S %s\n%s" % (sid or "-", text)


class Handler(http.server.BaseHTTPRequestHandler):
    server_version = "claude-relay"
    sys_version = ""
    relay = None  # set by serve()

    def log_message(self, fmt, *args):  # the default would log the request line; we log our own, below
        pass

    def log_line(self, status, nin, nout, t0):
        sys.stderr.write("%s %s %s %d in=%d out=%d %.1fs\n" % (time.strftime("%H:%M:%S"), self.command,
                         self.path.split("?")[0][:40], status, nin, nout, time.time() - t0))

    def reply(self, status, text, nin=0, t0=None):
        body = to_ascii(text).encode("latin-1", "replace")
        if len(body) > REPLY_MAX: body = body[:REPLY_MAX - 4] + b"\n..."
        self.send_response(status)
        self.send_header("Content-Type", "text/plain; charset=iso-8859-1")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Connection", "close")
        self.end_headers()
        self.wfile.write(body)
        self.close_connection = True
        self.log_line(status, nin, len(body), t0 or time.time())

    def do_GET(self): self.reply(405, "POST only.")
    do_PUT = do_DELETE = do_PATCH = do_HEAD = do_GET

    def do_POST(self):
        t0 = time.time()
        r = self.relay
        if self.path != "/api/claude": return self.reply(404, "Not here. POST /api/claude.", 0, t0)
        auth = self.headers.get("Authorization", "")
        given = auth[7:].strip().encode() if auth.startswith("Bearer ") else b""
        if not hmac.compare_digest(given, r.token): return self.reply(401, "Wrong or missing relay token.", 0, t0)  # TOKEN-CHECK
        if self.headers.get("Transfer-Encoding"): return self.reply(411, "Send a Content-Length.", 0, t0)
        try: n = int(self.headers.get("Content-Length", ""))
        except ValueError: return self.reply(411, "Send a Content-Length.", 0, t0)
        if n < 0: return self.reply(400, "Bad length.", 0, t0)
        if n > r.cfg.max_body: return self.reply(413, "That prompt is too long for the relay.", n, t0)
        raw = self.rfile.read(n)
        ctype = self.headers.get("Content-Type", "").split(";")[0].strip().lower()
        session = ""
        if ctype == "application/json":
            try: d = json.loads(raw.decode("utf-8"))
            except ValueError: return self.reply(400, "Bad JSON.", n, t0)
            if not isinstance(d, dict): return self.reply(400, "Bad JSON.", n, t0)
            prompt, session = d.get("prompt", ""), d.get("session", "") or ""
            pi = d.get("pi", "") if isinstance(d.get("pi", ""), str) else ""
            self.pi_status = pi[:120]
            if not isinstance(prompt, str) or not isinstance(session, str): return self.reply(400, "Bad JSON.", n, t0)
        else:
            prompt = raw.decode("utf-8", "replace")
            session = self.headers.get("X-Claude-Session", "").strip()
        if not prompt.strip(): return self.reply(400, "Empty prompt.", n, t0)
        if session in ("-",): session = ""
        if session and (not UUID_RE.match(session) or session not in r.sessions):
            session = ""  # unknown or malformed: start fresh rather than pass it to --resume
        if not r.busy.acquire(blocking=False): return self.reply(429, "Claude is busy with another question.", n, t0)
        try: status, text = r.run_claude(prompt, session, getattr(self, "pi_status", ""))
        finally: r.busy.release()
        return self.reply(status, text, n, t0)


def read_token(args):
    if args.token_file:
        with open(os.path.expanduser(args.token_file), encoding="utf-8") as f: tok = f.read().strip()
    else:
        tok = os.environ.get("CLAUDE_RELAY_TOKEN", "").strip()
    if not (TOKEN_MIN <= len(tok) <= TOKEN_MAX) or not all(33 <= ord(c) < 127 for c in tok):
        sys.exit("claude-relay: refusing to start. Give a token of %d to %d printable characters with "
                 "--token-file FILE or CLAUDE_RELAY_TOKEN. Make one: openssl rand -hex 24" % (TOKEN_MIN, TOKEN_MAX))
    return tok


def main(argv=None):
    ap = argparse.ArgumentParser(description="HTTP relay from Joshua Tree's Claude app to headless Claude Code. "
                                 "Command-capable: guard the token.")
    ap.add_argument("--port", type=int, default=DEFAULT_PORT)
    ap.add_argument("--lan", action="store_true", help="listen on every interface (Pi/LAN). Plain HTTP: the token can be sniffed.")
    ap.add_argument("--token-file", help="file holding the shared token (else CLAUDE_RELAY_TOKEN)")
    ap.add_argument("--cwd", default=ROOT, help="where Claude Code runs and the only place it may read (default: this repo)")
    ap.add_argument("--timeout", type=int, default=DEFAULT_TIMEOUT, help="hard limit per request, seconds")
    ap.add_argument("--max-body", type=int, default=DEFAULT_MAX_BODY, help="largest request body, bytes")
    ap.add_argument("--claude", default="claude", help="the claude executable")
    ap.add_argument("--model", default="", help="optional --model for claude")
    ap.add_argument("--api-key-file", default="", help="answer with the Messages API as Samantha (Claude Platform credit) instead of claude -p")
    ap.add_argument("--api-model", default="claude-haiku-5-5", help="cheap model for short questions in --api-key-file mode")
    ap.add_argument("--api-model-top", default="claude-opus-5-5", help="strongest model, for design, proofs and security questions")
    ap.add_argument("--api-model-hard", default="claude-sonnet-5-5", help="stronger model for long or hard questions")
    ap.add_argument("--tools", default=",".join(READ_ONLY_TOOLS),
                    help="comma-separated built-in tools (default Read,Grep,Glob: read-only)")
    args = ap.parse_args(argv)
    args.tools = [t for t in args.tools.split(",") if t]
    args.cwd = os.path.abspath(os.path.expanduser(args.cwd))
    token = read_token(args)
    if any(t not in READ_ONLY_TOOLS for t in args.tools):
        sys.stderr.write("claude-relay: WARNING tools %s are not read-only. Claude can change files or run "
                         "commands for anyone with the token.\n" % ",".join(args.tools))
    host = "0.0.0.0" if args.lan else "127.0.0.1"
    if args.lan:
        sys.stderr.write("claude-relay: WARNING --lan: listening on every interface over plain HTTP. Anyone on this "
                         "network who sees one request can replay the token.\n")
    Handler.relay = Relay(args, token)
    srv = http.server.ThreadingHTTPServer((host, args.port), Handler)
    srv.daemon_threads = True
    sys.stderr.write("claude-relay: listening on %s:%d, cwd %s, tools %s, timeout %ds\n"
                     % (host, srv.server_address[1], args.cwd, ",".join(args.tools), args.timeout))
    sys.stderr.flush()
    try: srv.serve_forever()
    except KeyboardInterrupt: pass
    return 0


if __name__ == "__main__":
    sys.exit(main())
