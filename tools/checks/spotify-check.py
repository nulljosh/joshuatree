#!/usr/bin/env python3
"""The Mac Spotify remote (tools/claude-relay/spotify.py), from a saved /v1/me/player reply (no network, no account).
It runs on a free port with a throwaway token. `now` must show the track, the artist, the device and a Pause link; a
paused reply must offer Play; `search` must list the saved hits as Play links; HTML in a title must come back escaped.
A wrong token gets 401, a bad command or a bad URI 400, a GET 405, another path 404.
Usage: tools/checks/spotify-check.py   (from the repo root)
"""
import json, os, shutil, socket, subprocess, sys, tempfile, time, urllib.request, urllib.error

root = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..")
tmp = tempfile.mkdtemp(); fails = []
URI = "spotify:track:" + "a" * 22
fix = {"is_playing": True, "device": {"name": "Mac mini"},
       "item": {"name": "Joshua <b>Tree</b>", "artists": [{"name": "U2"}], "album": {"name": "The Album"}},
       "search": {"tracks": {"items": [{"name": "Where the Streets", "uri": URI, "artists": [{"name": "U2"}]}]}}}
paused = dict(fix, is_playing=False)
for name, d in (("playing", fix), ("paused", paused)): json.dump(d, open("%s/%s.json" % (tmp, name), "w"))
tok = "check-token-0123456789abcdef"

def serve(fixture):
    with socket.socket() as s: s.bind(("127.0.0.1", 0)); port = s.getsockname()[1]
    p = subprocess.Popen([sys.executable, os.path.join(root, "tools/claude-relay/spotify.py"), "--port", str(port), "--fixture", fixture],
                         env=dict(os.environ, CLAUDE_RELAY_TOKEN=tok), stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True)
    for _ in range(50):
        if p.stdout.readline().startswith("spotify: listening"): break
        time.sleep(0.1)
    return p, port

def post(port, body, token=tok, path="/api/spotify/page", method="POST"):
    req = urllib.request.Request("http://127.0.0.1:%d%s" % (port, path), data=body.encode() if body is not None else None, method=method,
                                 headers={"Authorization": "Bearer " + token})
    try:
        with urllib.request.urlopen(req, timeout=20) as r: return r.status, r.read().decode()
    except urllib.error.HTTPError as e: return e.code, e.read().decode()

def need(ok, what):
    if ok: print("  ok:", what)
    else: fails.append(what)

p, port = serve(tmp + "/playing.json"); q = None
try:
    st, h = post(port, "now")
    need(st == 200 and "U2" in h and "Mac mini" in h and 'href="spotify:pause"' in h, "now shows the track, artist, device and a Pause link")
    need("<b>Tree" not in h and "&lt;b&gt;Tree" in h, "a title with HTML comes back escaped")
    st, h = post(port, "search streets")
    need(st == 200 and 'href="spotify:play %s"' % URI in h, "search lists the hit as a Play link")
    st, h = post(port, "play " + URI); need(st == 200, "play URI is accepted")
    for body, want, what in (("play spotify:track:short", 400, "a bad URI"), ("dance", 400, "an unknown command"), ("pause now", 400, "pause with an argument"),
                             ("search " + "x" * 101, 400, "a 101 character search")):
        st, _ = post(port, body); need(st == want, "%s is %d" % (what, want))
    st, _ = post(port, "now", token="wrong-token-0123456789"); need(st == 401, "a wrong token is 401")
    st, _ = post(port, None, method="GET"); need(st == 405, "a GET is 405")
    st, _ = post(port, "now", path="/x"); need(st == 404, "another path is 404")
    q, port2 = serve(tmp + "/paused.json")
    st, h = post(port2, "now"); need(st == 200 and 'href="spotify:play"' in h and "Paused" in h, "a paused reply offers Play")
finally:
    p.terminate()
    if q: q.terminate()
    shutil.rmtree(tmp, ignore_errors=True)
if fails:
    for f in fails: print("FAIL:", f)
    sys.exit(1)
print("OK: spotify.py renders the remote and refuses bad input")
