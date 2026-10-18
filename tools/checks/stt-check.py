#!/usr/bin/env python3
"""The Mac speech-to-text service (tools/claude-relay/stt.py): a spoken sentence goes in as raw 16 kHz PCM and the words
come back. macOS `say` speaks "the quick brown fox jumps over the lazy dog", ffmpeg makes the PCM the Pi would send,
stt.py runs on a free port with a throwaway token, and the reply must hold "quick brown fox". A wrong token gets 401, an
odd-length body 400, and a GET 405. Skips (exit 0) where say, ffmpeg, whisper-cli or the base model is missing.
Usage: tools/checks/stt-check.py   (from the repo root)
"""
import json, os, shutil, socket, subprocess, sys, tempfile, time, urllib.request, urllib.error

root = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..")
model = os.path.expanduser("~/.cache/whisper-cpp/ggml-base.en.bin")
if not all(shutil.which(t) for t in ("say", "ffmpeg", "whisper-cli")) or not os.path.exists(model):
    print("SKIP: say, ffmpeg, whisper-cli or ~/.cache/whisper-cpp/ggml-base.en.bin missing"); sys.exit(0)
tmp = tempfile.mkdtemp(); fails = []
subprocess.run(["say", "-o", tmp + "/s.aiff", "the quick brown fox jumps over the lazy dog"], check=True)
subprocess.run(["ffmpeg", "-loglevel", "error", "-y", "-i", tmp + "/s.aiff", "-ar", "16000", "-ac", "1", "-f", "s16le", tmp + "/s.pcm"], check=True)
pcm = open(tmp + "/s.pcm", "rb").read()
with socket.socket() as s: s.bind(("127.0.0.1", 0)); port = s.getsockname()[1]
tok = "check-token-0123456789abcdef"
p = subprocess.Popen([sys.executable, os.path.join(root, "tools/claude-relay/stt.py"), "--port", str(port), "--bind", "127.0.0.1"],
                     env=dict(os.environ, CLAUDE_RELAY_TOKEN=tok), stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True)
def post(body, token=tok, path="/api/stt", method="POST"):
    req = urllib.request.Request("http://127.0.0.1:%d%s" % (port, path), data=body, method=method,
                                 headers={"Authorization": "Bearer " + token, "Content-Type": "application/octet-stream"})
    try:
        with urllib.request.urlopen(req, timeout=120) as r: return r.status, json.loads(r.read())
    except urllib.error.HTTPError as e: return e.code, json.loads(e.read() or b"{}")
try:
    for _ in range(50):
        if p.stdout.readline().startswith("stt: listening"): break
        time.sleep(0.1)
    st, j = post(pcm)
    if st == 200 and "quick brown fox" in j.get("text", "").lower(): print("  ok: %r in %ss" % (j["text"], j["took"]))
    else: fails.append("spoken sentence came back as %s %r" % (st, j))
    for want, (st, j), what in ((401, post(pcm, token="wrong-token-0123456789"), "a wrong token"), (400, post(pcm[:-1]), "an odd-length body"),
                                 (405, post(None, method="GET"), "a GET")):
        if st == want: print("  ok: %s is %d" % (what, st))
        else: fails.append("%s got %d, want %d" % (what, st, want))
finally:
    p.terminate(); shutil.rmtree(tmp, ignore_errors=True)
if fails:
    for f in fails: print("FAIL:", f)
    sys.exit(1)
print("OK: stt.py turns speech into words on the Mac")
