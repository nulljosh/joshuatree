#!/usr/bin/env python3
"""claude-relay: the small HTTP bridge between Joshua Tree's Claude app and Claude Code.

    !!! THIS IS A COMMAND-CAPABLE AGENT BEHIND AN HTTP PORT. !!!

    Every request that carries the right token runs `claude -p` (headless Claude Code) on
    THIS machine, as THIS user, logged in to THIS user's Claude plan. Treat the token like
    a password for your account. Anyone holding it can make Claude read the files under
    --cwd (the repo by default) and spend your plan's usage.

Joshua Tree cannot run Claude Code itself. The i386 app defaults to a loopback HTTP POST
(SYS_HTTP_POST); the Pi Terminal uses HTTPS. This relay handles the Claude part:

    POST /api/claude
    Authorization: Bearer <token>
    Content-Type: application/json      {"prompt": "...", "session": "<uuid or empty>",
                                         "model": "auto|haiku|sonnet|opus", "effort": "low|medium|high"}
      (model and effort are optional, default auto and medium; anything else is 400. The reply's
       second line is "M <name of the model that answered>".)
      (or text/plain: the whole body is the prompt, session from the X-Claude-Session header)

    200 text/plain:   S <session uuid>\\nM <model that answered, e.g. Claude Haiku 5.5>\\n<reply text>
    401 no or wrong token          404 any other path        405 not POST
    411 no Content-Length          413 body over the cap     400 bad JSON, empty prompt, bad session
    429 a request is already running (one at a time)        504 claude ran past --timeout
    502 claude failed or answered something unreadable

What it locks down, and why:
  - Binds 127.0.0.1 unless --lan is given. QEMU's user network reaches the host's loopback
    at 10.0.2.2, so the emulator needs no LAN exposure at all. --lan (a Pi or a second
    machine) binds 0.0.0.0 and requires --tls-cert and --tls-key. The Pi verifies that
    certificate against its embedded trust anchors and never falls back to HTTP.
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
import argparse, hmac, http.server, json, os, re, signal, ssl, stat, subprocess, sys, threading, time, urllib.request, urllib.error, uuid

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


# The Pi's /model and /effort (docs/AGENT.md). The request may carry "model" and "effort"; each must be one of these
# words. A model word picks a configured model id (--api-model and friends); a user-supplied id never reaches the API.
MODELS = ("auto", "haiku", "sonnet", "opus")
EFFORTS = {                    # name: (thinking budget tokens, max_tokens); medium is what the relay always did
    "low": (0, 300),
    "medium": (0, 600),
    "high": (2048, 4096),      # max_tokens must exceed the thinking budget
}


def claude_argv(cfg, session, model="auto"):
    """The exact command line one request runs. The prompt is NOT here: it goes on stdin."""
    argv = [cfg.claude, "-p", "--output-format", "json"]
    argv += ["--tools", *cfg.tools]
    argv += ["--allowedTools", *[t + "(./**)" for t in cfg.tools]]
    argv += ["--permission-mode", "dontAsk", "--strict-mcp-config", "--setting-sources", "project"]
    if model != "auto": argv += ["--model", model]   # an alias from MODELS, never raw input
    elif cfg.model: argv += ["--model", cfg.model]
    if session: argv += ["--resume", session]
    return argv


SAMANTHA = ("You are Samantha, the assistant inside Joshua Tree, a small operating system built from scratch "
            "that runs on a Raspberry Pi. You answer at its console. Be warm, plain and brief: a few short "
            "sentences, no markdown, no lists unless asked. You can also act on the Pi: end your answer with "
            "actions, each alone on its own line, under 80 characters, at most 4 per answer. "
            "[[note TEXT]] prints TEXT on the Pi's console. [[say TEXT]] shows TEXT as a spoken caption. "
            "[[led blink]] blinks the Pi's green light once. [[open APP]] opens an app by name (Calculator or Clock). "
            "[[browse URL]] fetches an http or https page as text. [[calc EXPR]] works out a sum on the Pi. "
            "[[status]] reports the Pi's address, clock and Wi-Fi. These are the only actions; never invent others. "
            "After the Pi runs them it sends you what they printed as the next message; use that to finish the "
            "task, up to 5 rounds. Use an action only when it helps: browse when you need a live page, calc for "
            "arithmetic, status when asked about the Pi. Nothing you can do changes a file; if a task would need "
            "to, ask first and explain what would change.")


TOP_WORDS = ("architecture", "design a", "prove", "security", "tradeoff", "step by step plan")
HARD_WORDS = ("fix", "bug", "why", "explain", "write", "design", "debug", "compare", "plan", "code")


def pick_model(cfg, prompt):
    """A short, easy question goes to the cheap model; a long one or one that asks for real work goes to the strong one.
    Both names are flags, so there is nothing to watch: --api-model (cheap) and --api-model-hard."""
    low = prompt.lower()
    if any(w in low for w in TOP_WORDS): return cfg.api_model_top
    hard = len(prompt) > 280 or any(w in low for w in HARD_WORDS)
    return cfg.api_model_hard if hard else cfg.api_model


MODEL_RE = re.compile(r"^claude-([a-z]+)-(\d+)-(\d+)$")


def display_name(model):
    """claude-sonnet-5-5 -> "Claude Sonnet 5.5", the name the Pi shows in its prompt. Anything else is just "Claude"."""
    m = MODEL_RE.match(model or "")
    return "Claude %s %s.%s" % (m.group(1).capitalize(), m.group(2), m.group(3)) if m else "Claude"


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
        directory = os.open(FILES_DIR, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW)
    except OSError:
        return "The shared folder does not exist."
    try:
        files = []
        for f in os.listdir(directory):
            try:
                if not f.startswith(".") and stat.S_ISREG(os.stat(f, dir_fd=directory, follow_symlinks=False).st_mode):
                    files.append(f)
            except OSError:
                continue   # a file removed while listing is no longer available
        if name == "list_files": return "\n".join(sorted(files)) or "(empty)"
        if name == "read_file":
            n = args.get("name", "") if isinstance(args, dict) else ""
            if n not in files: return "No such file. Use list_files first."
            try:
                # Pin the folder and refuse links at open time; a replaced FIFO must not block the relay.
                fd = os.open(n, os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK, dir_fd=directory)
                with os.fdopen(fd, errors="replace") as fh:
                    if not stat.S_ISREG(os.fstat(fh.fileno()).st_mode): return "No such file. Use list_files first."
                    return fh.read(20000)
            except OSError:
                return "No such file. Use list_files first."
        return "Unknown tool."
    finally:
        os.close(directory)


def api_call(cfg, key, model, messages, system=SAMANTHA, effort="medium"):
    budget, max_tokens = EFFORTS[effort]
    req_body = {"model": model, "max_tokens": max_tokens, "system": system, "tools": TOOLS, "messages": messages}
    if budget: req_body["thinking"] = {"type": "enabled", "budget_tokens": budget}
    body = json.dumps(req_body).encode()
    req = urllib.request.Request("https://api.anthropic.com/v1/messages", data=body, method="POST", headers={
        "x-api-key": key, "anthropic-version": "2023-06-01", "content-type": "application/json"})
    with urllib.request.urlopen(req, timeout=cfg.timeout) as r:
        return json.loads(r.read().decode("utf-8", "replace"))


API_HISTORY_MAX = 24           # messages kept per session: 5 agent rounds and their results, with room for a follow-up


def run_api(cfg, prompt, pi="", history=None, want="auto", effort="medium"):
    """--api-key-file mode: the Messages API with two read-only file tools, paid from the Claude Platform credit, not
    the Claude Code plan. At most 4 model turns per question. `history` is the session's earlier messages (the agent
    loop sends an action's result as the next user turn); it is extended in place. `want` is a MODELS word, `effort` an
    EFFORTS key. Returns (status, text, name of the model that answered)."""
    try:
        with open(os.path.expanduser(cfg.api_key_file)) as fh: key = fh.read().strip()
    except OSError:
        return 502, "The relay has no API key file.", ""
    model = pick_model(cfg, prompt) if want == "auto" else {"haiku": cfg.api_model, "sonnet": cfg.api_model_hard, "opus": cfg.api_model_top}[want]
    system = SAMANTHA + (" Live status of the Pi you run on (ip, clock in UTC seconds since 1970, Wi-Fi bars): " + pi if pi else "")
    messages = history if history is not None else []
    messages.append({"role": "user", "content": prompt})
    del messages[:-API_HISTORY_MAX]
    try:
        for _ in range(4):
            d = api_call(cfg, key, model, messages, system, effort)
            blocks = d.get("content", [])
            if d.get("stop_reason") != "tool_use": break
            messages.append({"role": "assistant", "content": blocks})
            results = [{"type": "tool_result", "tool_use_id": b["id"], "content": run_tool(b.get("name"), b.get("input"))}
                       for b in blocks if b.get("type") == "tool_use"]
            messages.append({"role": "user", "content": results})
    except urllib.error.HTTPError as e:
        return 502, "The API said %d." % e.code, ""
    except (urllib.error.URLError, OSError, ValueError, KeyError):
        return 502, "The API did not answer.", ""
    text = "".join(b.get("text", "") for b in d.get("content", []) if isinstance(b, dict))
    if text: messages.append({"role": "assistant", "content": text})
    return (200, text, display_name(model)) if text else (502, "The API returned no text.", "")


class Relay:
    def __init__(self, cfg, token):
        self.cfg = cfg
        self.token = token.encode()
        self.busy = threading.Lock()
        self.sessions = set()   # session ids this relay has handed out; --resume only accepts these
        self.api_history = {}   # --api-key-file mode: session id -> its messages, so an agent round can continue

    def run_claude(self, prompt, session, pi="", model="auto", effort="medium"):
        """Returns (status, text). Kills claude's whole process group past the timeout."""
        if self.cfg.api_key_file:
            if not session:
                session = str(uuid.uuid4())
                if len(self.sessions) >= SESSIONS_MAX: self.sessions.clear(); self.api_history.clear()
                self.sessions.add(session)
            status, text, name = run_api(self.cfg, prompt, pi, self.api_history.setdefault(session, []), model, effort)
            return status, ("S %s\nM %s\n%s" % (session, name, text)) if status == 200 else text
        p = subprocess.Popen(claude_argv(self.cfg, session, model), cwd=self.cfg.cwd, stdin=subprocess.PIPE,
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
        used = "Claude " + model.capitalize() if model != "auto" else display_name(self.cfg.model)   # claude -p has no effort knob: ignored here
        return 200, "S %s\nM %s\n%s" % (sid or "-", used, text)


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
        model, effort = "auto", "medium"
        if ctype == "application/json":
            try: d = json.loads(raw.decode("utf-8"))
            except ValueError: return self.reply(400, "Bad JSON.", n, t0)
            if not isinstance(d, dict): return self.reply(400, "Bad JSON.", n, t0)
            prompt, session = d.get("prompt", ""), d.get("session", "") or ""
            model, effort = d.get("model", "auto"), d.get("effort", "medium")
            if not (isinstance(model, str) and isinstance(effort, str) and model in MODELS and effort in EFFORTS):   # a whitelist: anything else, even a real model id, is refused
                return self.reply(400, "Unknown model or effort.", n, t0)
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
        try: status, text = r.run_claude(prompt, session, getattr(self, "pi_status", ""), model, effort)
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
    ap.add_argument("--lan", action="store_true", help="listen on every interface (Pi/LAN); requires TLS")
    ap.add_argument("--tls-cert", help="PEM server certificate, including its chain")
    ap.add_argument("--tls-key", help="PEM private key for --tls-cert")
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
    if bool(args.tls_cert) != bool(args.tls_key): ap.error("--tls-cert and --tls-key must be supplied together")
    if args.lan and not args.tls_cert: ap.error("--lan requires --tls-cert and --tls-key; refusing a cleartext bearer")
    context = None
    if args.tls_cert:
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        context.minimum_version = ssl.TLSVersion.TLSv1_2
        context.load_cert_chain(os.path.expanduser(args.tls_cert), os.path.expanduser(args.tls_key))
    args.tools = [t for t in args.tools.split(",") if t]
    args.cwd = os.path.abspath(os.path.expanduser(args.cwd))
    token = read_token(args)
    if any(t not in READ_ONLY_TOOLS for t in args.tools):
        sys.stderr.write("claude-relay: WARNING tools %s are not read-only. Claude can change files or run "
                         "commands for anyone with the token.\n" % ",".join(args.tools))
    host = "0.0.0.0" if args.lan else "127.0.0.1"
    Handler.relay = Relay(args, token)
    class Server(http.server.ThreadingHTTPServer):
        def get_request(self):
            conn, address = super().get_request()
            conn.settimeout(15)   # a stalled handshake/body must not hold a handler forever
            return conn, address
    srv = Server((host, args.port), Handler)
    if context:
        # Handshake in the handler thread, so one slow peer cannot block accept().
        srv.socket = context.wrap_socket(srv.socket, server_side=True, do_handshake_on_connect=False)
    srv.daemon_threads = True
    sys.stderr.write("claude-relay: listening on %s:%d, cwd %s, tools %s, timeout %ds\n"
                     % (host, srv.server_address[1], args.cwd, ",".join(args.tools), args.timeout))
    sys.stderr.flush()
    try: srv.serve_forever()
    except KeyboardInterrupt: pass
    return 0


if __name__ == "__main__":
    sys.exit(main())
