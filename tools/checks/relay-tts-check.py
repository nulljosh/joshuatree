#!/usr/bin/env python3
"""Relay text-to-speech check, no network, no real ElevenLabs, no money.

Starts tools/claude-relay/relay.py on loopback with --tts-base pointed at a stub ElevenLabs server
in this process. The stub records every call and answers with known PCM. Proves:

  normal: POST /api/tts with the token returns the stub's PCM bytes (16-bit mono, X-Sample-Rate 22050);
    the stub got the key in xi-api-key, the default Samantha voice id in the path, pcm_22050, and the text.
  per-request cap: text over the cap is 413 and the stub sees no call.
  per-day cap: requests are served until the day's total would pass the cap, then 429 with no stub call;
    the count survives a relay restart; a usage file dated yesterday starts a fresh day.
  failures: an upstream error is 502 and gives the characters back; a redirect is not followed.
  key hygiene: a unique key never appears in any response body or header, on any path (ok, 413, 429, 401,
    upstream 401 that echoes the key, missing file, loose file mode), nor in the relay's log.
  token: no token and a wrong token are 401 and the stub sees no call.
  setup: no key file is 503; a key file readable by others is 503 and never read.

Usage: python3 tools/checks/relay-tts-check.py
"""
import http.client, http.server, json, os, re, shutil, subprocess, sys, tempfile, threading, time

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
RELAY = os.path.join(ROOT, "tools", "claude-relay", "relay.py")
TOKEN = "relay-tts-token-0123456789abcdef"
KEY = "sk_unit_ZQ7vK3mP9xL2wR8tB5nC4jH6"
PCM = bytes((i * 7) & 0xFF for i in range(2000))   # 1000 samples of known bytes
fails = []


def check(name, ok, detail=""):
    print(("ok   " if ok else "FAIL ") + name + (("  (" + detail + ")") if detail and not ok else ""))
    if not ok: fails.append(name)


calls = []
mode = {"v": "ok"}


class Stub(http.server.BaseHTTPRequestHandler):
    def log_message(self, *a): pass

    def do_POST(self):
        n = int(self.headers.get("Content-Length", "0"))
        body = json.loads(self.rfile.read(n) or b"{}")
        calls.append({"path": self.path, "key": self.headers.get("xi-api-key"), "body": body})
        if mode["v"] == "401":   # a hostile or careless upstream that repeats the key it was given
            data = ("bad key " + (self.headers.get("xi-api-key") or "")).encode()
            self.send_response(401); self.send_header("X-Echo", self.headers.get("xi-api-key") or "")
        elif mode["v"] == "redirect":
            data = b""
            self.send_response(302); self.send_header("Location", "http://127.0.0.1:1/steal")
        else:
            data = PCM
            self.send_response(200)
        self.send_header("Content-Length", str(len(data))); self.end_headers(); self.wfile.write(data)


def start_relay(tmp, port_stub, daily=100, per=40, keyfile=None, usage=None):
    env = dict(os.environ, CLAUDE_RELAY_TOKEN=TOKEN)
    errlog = open(os.path.join(tmp, "relay-%d.log" % time.time_ns()), "w+")
    args = [sys.executable, RELAY, "--port", "0", "--cwd", tmp,
            "--tts-base", "http://127.0.0.1:%d" % port_stub, "--tts-key-file", keyfile or os.path.join(tmp, "eleven.key"),
            "--tts-usage-file", usage or os.path.join(tmp, "usage.json"),
            "--tts-max-chars", str(per), "--tts-daily-chars", str(daily), "--timeout", "5"]
    p = subprocess.Popen(args, env=env, stdout=subprocess.DEVNULL, stderr=errlog)
    for _ in range(100):
        time.sleep(0.05)
        errlog.seek(0); m = re.search(r"listening on ([\d.]+):(\d+)", errlog.read())
        if m: return p, int(m.group(2)), errlog
        if p.poll() is not None: break
    p.kill(); raise SystemExit("FAIL: relay did not start: " + open(errlog.name).read())


def post(port, body, token=TOKEN, ctype="application/json", path="/api/tts"):
    c = http.client.HTTPConnection("127.0.0.1", port, timeout=20)
    h = {"Content-Type": ctype}
    if token is not None: h["Authorization"] = "Bearer " + token
    if isinstance(body, dict): body = json.dumps(body).encode()
    c.request("POST", path, body=body, headers=h)
    r = c.getresponse(); data = r.read(); heads = str(r.getheaders()); c.close()
    seen.append(data + heads.encode())
    return r.status, data, r


seen = []   # every response (body + headers), scanned for the key at the end
tmp = tempfile.mkdtemp(prefix="jt-relay-tts-")
srv = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Stub)
threading.Thread(target=srv.serve_forever, daemon=True).start()
sp = srv.server_address[1]
keyfile = os.path.join(tmp, "eleven.key")
usage = os.path.join(tmp, "usage.json")
procs, logs = [], []
try:
    # no key file yet
    p, port, el = start_relay(tmp, sp); procs.append(p); logs.append(el)
    st, data, _ = post(port, {"text": "hello"})
    check("no key file -> 503 and no upstream call", st == 503 and not calls, str(st))
    # a key file others can read
    with open(keyfile, "w") as f: f.write(KEY)
    os.chmod(keyfile, 0o644)
    st, data, _ = post(port, {"text": "hello"})
    check("key file readable by others -> 503, never used", st == 503 and not calls, str(st))
    os.chmod(keyfile, 0o600)

    # 5: token
    st, _, _ = post(port, {"text": "hello"}, token=None)
    check("no token -> 401", st == 401, str(st))
    st, _, _ = post(port, {"text": "hello"}, token="wrong-token-wrong-token-wrong")
    check("wrong token -> 401", st == 401, str(st))
    check("refused tokens never reached ElevenLabs", not calls)

    # 1: normal
    st, data, r = post(port, {"text": "Hello, I am Samantha."})
    check("normal request -> 200 and the stub's PCM", st == 200 and data == PCM, "%d %d bytes" % (st, len(data)))
    check("reply says 22050 Hz raw audio", r.getheader("X-Sample-Rate") == "22050" and r.getheader("Content-Type") == "application/octet-stream")
    c0 = calls[0] if calls else {}
    check("stub got the key, Samantha's voice and pcm_22050",
          c0.get("key") == KEY and c0.get("path") == "/v1/text-to-speech/EXAVITQu4vr4xnSDxMaL?output_format=pcm_22050", repr(c0.get("path")))
    check("stub got the text and a model id", c0.get("body", {}).get("text") == "Hello, I am Samantha." and c0.get("body", {}).get("model_id"))
    st, data, _ = post(port, b"plain text works", ctype="text/plain")
    check("text/plain body works", st == 200 and data == PCM)
    st, _, _ = post(port, {"text": "  "})
    check("empty text -> 400", st == 400, str(st))
    st, _, _ = post(port, {"text": 5})
    check("non-string text -> 400", st == 400, str(st))
    used = json.load(open(usage))["chars"]
    check("usage file counts the two good requests (21 + 16)", used == 37, str(used))

    # 2: per-request cap (40 here)
    n0 = len(calls)
    st, data, _ = post(port, {"text": "x" * 41})
    check("over the per-request cap -> 413", st == 413, str(st))
    check("413 made no upstream call", len(calls) == n0)
    check("413 did not spend the day's count", json.load(open(usage))["chars"] == 37)
    st, _, _ = post(port, {"text": "y" * 40})   # exactly the cap passes; day now 77
    check("exactly the cap is allowed", st == 200, str(st))

    # 3: per-day cap (100 here): 77 used, 23 left
    n0 = len(calls)
    st, data, _ = post(port, {"text": "z" * 24})
    check("a request that would cross the day cap -> 429", st == 429, str(st))
    check("429 made no upstream call", len(calls) == n0)
    st, _, _ = post(port, {"text": "z" * 23})
    check("a request that fits the rest of the day is served", st == 200, str(st))
    st, _, _ = post(port, {"text": "a"})
    check("then the day is full -> 429", st == 429, str(st))

    # restart: the count survives
    p.terminate(); p.wait(); procs.remove(p)
    n0 = len(calls)
    p, port, el = start_relay(tmp, sp); procs.append(p); logs.append(el)
    st, _, _ = post(port, {"text": "a"})
    check("after a relay restart the day is still full -> 429", st == 429, str(st))
    check("the refused restart request made no upstream call", len(calls) == n0)
    # a new day resets it
    json.dump({"date": "2000-01-01", "chars": 100}, open(usage, "w"))
    st, _, _ = post(port, {"text": "new day"})
    check("a usage file from another date starts a fresh day", st == 200, str(st))
    check("the fresh day counts only today", json.load(open(usage))["chars"] == 7)

    # failures: upstream 401 echoing the key, and a redirect
    json.dump({"date": time.strftime("%Y-%m-%d"), "chars": 0}, open(usage, "w"))
    mode["v"] = "401"
    st, data, _ = post(port, {"text": "upstream says no"})
    check("upstream error -> 502", st == 502, str(st))
    check("a failed call gives the characters back", json.load(open(usage))["chars"] == 0)
    mode["v"] = "redirect"
    n0 = len(calls)
    st, _, _ = post(port, {"text": "redirect me"})
    check("an upstream redirect is a 502 and is not followed", st == 502 and len(calls) == n0 + 1, str(st))
    mode["v"] = "ok"

    # claude path still 401s without a token (shared check)
    st, _, _ = post(port, {"prompt": "hi"}, token=None, path="/api/claude")
    check("/api/claude still needs the token", st == 401)

    # 4: key hygiene
    p.terminate(); p.wait(); procs.remove(p)
    for el in logs: el.seek(0)
    log = "".join(el.read() for el in logs)
    check("the key is in no response body or header", all(KEY.encode() not in blob for blob in seen), "%d responses" % len(seen))
    check("the key is in no log line", KEY not in log)
    check("no text spoken is in the log", "Samantha" not in log and "plain text works" not in log)
    check("the log has a line per tts request", log.count("/api/tts") >= 10, str(log.count("/api/tts")))
    check("the stub saw the key only in xi-api-key", all(c["key"] == KEY for c in calls) and KEY not in json.dumps([c["body"] for c in calls]))

    # startup refusals
    for bad in (("--tts-voice", "../x"), ("--tts-base", "http://example.com"), ("--tts-base", "ftp://127.0.0.1"), ("--tts-max-chars", "0")):
        r = subprocess.run([sys.executable, RELAY, *bad], env=dict(os.environ, CLAUDE_RELAY_TOKEN=TOKEN), capture_output=True, text=True, timeout=10)
        check("refuses to start with %s %s" % bad, r.returncode != 0 and "--tts" in r.stderr, r.stderr[-80:])
finally:
    for q in procs:
        q.kill(); q.wait()
    srv.shutdown()
    shutil.rmtree(tmp, ignore_errors=True)

print("FAILED: " + ", ".join(fails) if fails else "relay-tts-check: all ok")
sys.exit(1 if fails else 0)
