#!/usr/bin/env python3
"""claude-relay check, no QEMU, no network, no real model.

Starts tools/claude-relay/relay.py on loopback with a stub `claude` first on PATH (a tiny
Python script that records its argv, stdin and working directory, then answers in Claude
Code's real `--output-format json` shape: result, session_id, is_error). Proves:

  token: no token and a wrong token are 401 and never start claude; the right one is 200.
  reply: "S <uuid>" then the text, curly punctuation turned to ASCII, capped to 8191 bytes.
  session: the uuid handed back resumes (--resume <uuid>); an unknown or malformed one does not.
  read-only: the exact argv (--tools Read Grep Glob, cwd-scoped allow rules, dontAsk,
    --strict-mcp-config, --setting-sources project), the prompt on stdin and never in argv,
    claude run in --cwd.
  limits: an over-cap body is 413 unread; a run past --timeout is 504 and the stub and its
    child are killed; a second request while one runs is 429; GET 405, other paths 404.
  hygiene: the log never holds the token or the prompt; it binds 127.0.0.1; it will not
    start without a 16-63 character token.
  discrimination: a copy of relay.py with the token-check line removed answers a request
    with no token, so the token assertion above is a real one.

Usage: python3 tools/checks/claude-relay-check.py
"""
import http.client, json, os, re, shutil, socket, subprocess, sys, tempfile, threading, time

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
RELAY = os.path.join(ROOT, "tools", "claude-relay", "relay.py")
TOKEN = "relay-check-token-0123456789abcdef"
fails = []


def check(name, ok, detail=""):
    print(("ok   " if ok else "FAIL ") + name + (("  (" + detail + ")") if detail and not ok else ""))
    if not ok: fails.append(name)


STUB = r'''#!/usr/bin/env python3
import json, os, subprocess, sys, time, uuid
prompt = sys.stdin.read()
log = os.environ["STUB_LOG"]
rec = {"argv": sys.argv[1:], "stdin": prompt, "cwd": os.getcwd(), "pid": os.getpid()}
if "SLEEP" in prompt:
    child = subprocess.Popen(["sleep", "60"])
    rec["child"] = child.pid
with open(log, "a") as f: f.write(json.dumps(rec) + "\n")
if "SLEEP" in prompt:
    time.sleep(60)
sid = sys.argv[sys.argv.index("--resume") + 1] if "--resume" in sys.argv else str(uuid.uuid4())
text = "x" * 20000 if "BIG" in prompt else "echo: " + prompt + " — “ok”"
print(json.dumps({"type": "result", "subtype": "success", "is_error": False, "result": text, "session_id": sid, "num_turns": 1}))
'''


def start_relay(script, tmp, env_extra=None, extra_args=()):
    env = dict(os.environ)
    env["PATH"] = os.path.join(tmp, "bin") + os.pathsep + env["PATH"]
    env["STUB_LOG"] = os.path.join(tmp, "stub.log")
    env["CLAUDE_RELAY_TOKEN"] = TOKEN
    if env_extra: env.update(env_extra)
    errlog = open(os.path.join(tmp, "relay-%d.log" % time.time_ns()), "w+")
    p = subprocess.Popen([sys.executable, script, "--port", "0", "--cwd", tmp, *extra_args],
                         env=env, stdout=subprocess.DEVNULL, stderr=errlog)
    for _ in range(100):
        time.sleep(0.05)
        errlog.seek(0); m = re.search(r"listening on ([\d.]+):(\d+)", errlog.read())
        if m: return p, m.group(1), int(m.group(2)), errlog
        if p.poll() is not None: break
    p.kill(); raise SystemExit("FAIL: relay did not start: " + open(errlog.name).read())


def req(port, body=b"", token=TOKEN, method="POST", path="/api/claude", ctype="application/json", headers=None, timeout=20):
    c = http.client.HTTPConnection("127.0.0.1", port, timeout=timeout)
    h = {"Content-Type": ctype}
    if token is not None: h["Authorization"] = "Bearer " + token
    if headers: h.update(headers)
    if isinstance(body, (dict, list)): body = json.dumps(body).encode()
    c.request(method, path, body=body if method == "POST" else None, headers=h)
    r = c.getresponse(); data = r.read(); c.close()
    return r.status, data


def stub_calls(tmp):
    try: return [json.loads(l) for l in open(os.path.join(tmp, "stub.log"))]
    except FileNotFoundError: return []


def alive(pid):
    try:  # Linux: a killed process nobody has reaped yet is a zombie, which is dead for this purpose
        with open("/proc/%d/stat" % pid) as f:
            if f.read().rsplit(")", 1)[1].split()[0] == "Z": return False
    except (OSError, IndexError): pass
    try: os.kill(pid, 0); return True
    except ProcessLookupError: return False
    except PermissionError: return True


tmp = tempfile.mkdtemp(prefix="jt-claude-relay-")
os.makedirs(os.path.join(tmp, "bin"))
stub = os.path.join(tmp, "bin", "claude")
with open(stub, "w") as f: f.write(STUB)
os.chmod(stub, 0o755)
procs = []
try:
    p, host, port, errlog = start_relay(RELAY, tmp, extra_args=("--timeout", "3")); procs.append(p)
    check("binds 127.0.0.1 by default", host == "127.0.0.1", host)

    # token
    st, _ = req(port, {"prompt": "hello"}, token=None)
    check("no token -> 401", st == 401, str(st))
    st, _ = req(port, {"prompt": "hello"}, token="wrong-token-wrong-token-wrong")
    check("wrong token -> 401", st == 401, str(st))
    st, _ = req(port, {"prompt": "hello"}, token=TOKEN + "x")
    check("token with a suffix -> 401", st == 401, str(st))
    check("refused requests never started claude", stub_calls(tmp) == [])

    # a good request
    st, data = req(port, {"prompt": "what is in VERSION", "session": ""})
    text = data.decode("latin-1")
    m = re.match(r"S ([0-9a-f-]{36})\n", text)
    check("right token -> 200", st == 200, str(st))
    check("reply starts with S <uuid>", bool(m), text[:60])
    check("reply carries the answer", "echo: what is in VERSION" in text, text[:80])
    check("reply punctuation is ASCII", '- "ok"' in text and all(ord(ch) < 128 for ch in text), repr(text[-20:]))
    calls = stub_calls(tmp)
    check("claude ran once", len(calls) == 1, str(len(calls)))
    want = ["-p", "--output-format", "json", "--tools", "Read", "Grep", "Glob",
            "--allowedTools", "Read(./**)", "Grep(./**)", "Glob(./**)",
            "--permission-mode", "dontAsk", "--strict-mcp-config", "--setting-sources", "project"]
    if calls:
        c0 = calls[0]
        check("argv is exactly the read-only set", c0["argv"] == want, " ".join(c0["argv"]))
        check("prompt went on stdin", c0["stdin"] == "what is in VERSION", repr(c0["stdin"]))
        check("prompt is not in argv", not any("VERSION" in a for a in c0["argv"]))
        check("claude ran in --cwd", os.path.realpath(c0["cwd"]) == os.path.realpath(tmp), c0["cwd"])
        check("no Bash/Edit/Write tool offered", not any(t in c0["argv"] for t in ("Bash", "Edit", "Write", "WebFetch")))

    # session resume
    sid = m.group(1) if m else ""
    st, data = req(port, {"prompt": "and again", "session": sid})
    calls = stub_calls(tmp)
    check("known session resumes", st == 200 and calls and calls[-1]["argv"][-2:] == ["--resume", sid], str(calls[-1]["argv"][-2:] if calls else None))
    check("resumed reply keeps the session", data.decode("latin-1").startswith("S " + sid + "\n"))
    st, data = req(port, {"prompt": "stranger", "session": "11111111-2222-3333-4444-555555555555"})
    calls = stub_calls(tmp)
    check("unknown session starts fresh", st == 200 and "--resume" not in calls[-1]["argv"])
    st, data = req(port, {"prompt": "inject", "session": "--dangerously-skip-permissions"})
    calls = stub_calls(tmp)
    check("malformed session never reaches argv", st == 200 and not any("dangerously" in a for a in calls[-1]["argv"]))
    st, data = req(port, b"plain text prompt", ctype="text/plain", headers={"X-Claude-Session": sid})
    calls = stub_calls(tmp)
    check("text/plain body works and resumes", st == 200 and calls[-1]["stdin"] == "plain text prompt" and calls[-1]["argv"][-1] == sid)

    # reply cap
    st, data = req(port, {"prompt": "BIG"})
    check("reply capped to 8191 bytes", st == 200 and len(data) <= 8191, str(len(data)))

    # refusals before claude
    n0 = len(stub_calls(tmp))
    st, _ = req(port, {"prompt": "y" * 5000})
    check("over-cap body -> 413", st == 413, str(st))
    st, _ = req(port, {"prompt": "   "})
    check("empty prompt -> 400", st == 400, str(st))
    st, _ = req(port, b"{nope")
    check("bad JSON -> 400", st == 400, str(st))
    st, _ = req(port, method="GET")
    check("GET -> 405", st == 405, str(st))
    st, _ = req(port, {"prompt": "x"}, path="/api/chat")
    check("other path -> 404", st == 404, str(st))
    check("none of those started claude", len(stub_calls(tmp)) == n0)

    # timeout and one-at-a-time
    res = {}
    def slow(): res["slow"] = req(port, {"prompt": "SLEEP please"}, timeout=30)
    t0 = time.time(); th = threading.Thread(target=slow); th.start()
    for _ in range(100):
        time.sleep(0.05)
        if any("SLEEP" in c["stdin"] for c in stub_calls(tmp)): break
    st2, _ = req(port, {"prompt": "me too"})
    check("second request while one runs -> 429", st2 == 429, str(st2))
    th.join(30); took = time.time() - t0
    check("run past --timeout -> 504", res.get("slow", (0,))[0] == 504, str(res.get("slow", (0,))[0]))
    check("504 came back near the 3s limit", took < 8, "%.1fs" % took)
    slept = [c for c in stub_calls(tmp) if "SLEEP" in c["stdin"]]
    time.sleep(0.3)
    if slept:
        check("timed-out claude was killed", not alive(slept[0]["pid"]))
        check("its child process was killed too", not alive(slept[0]["child"]))
    st, _ = req(port, {"prompt": "after the timeout"})
    check("relay answers again after a timeout", st == 200, str(st))

    # log hygiene
    errlog.seek(0); log = errlog.read()
    check("log never holds the token", TOKEN not in log)
    check("log never holds a prompt", "what is in VERSION" not in log and "plain text prompt" not in log)
    check("log has one line per request", log.count("/api/claude") >= 10, str(log.count("/api/claude")))

    # startup refusals
    for bad in ("", "short"):
        r = subprocess.run([sys.executable, RELAY, "--port", "0"], env={**os.environ, "CLAUDE_RELAY_TOKEN": bad},
                           capture_output=True, text=True, timeout=10)
        check("refuses to start with token %r" % bad, r.returncode != 0 and "refusing" in r.stderr)

    # discrimination: the same request with the token check removed must get through
    mutant = os.path.join(tmp, "relay_mutant.py")
    src = open(RELAY).read()
    lines = [l for l in src.splitlines() if "# TOKEN-CHECK" not in l]
    check("token check line is marked once", len(src.splitlines()) - len(lines) == 1)
    open(mutant, "w").write("\n".join(lines) + "\n")
    mp, _, mport, _ = start_relay(mutant, tmp); procs.append(mp)
    st, _ = req(mport, {"prompt": "no token at all"}, token=None)
    check("mutant without the token check answers a tokenless request (so the 401 assertion is real)", st == 200, str(st))
finally:
    for p in procs:
        p.kill(); p.wait()
    shutil.rmtree(tmp, ignore_errors=True)

print("FAILED: " + ", ".join(fails) if fails else "claude-relay-check: all ok")
sys.exit(1 if fails else 0)
