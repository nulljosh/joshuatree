#!/usr/bin/env python3
"""Spotify for Blackcomb, on the Mac, next to yt.py (same token, same rules). A remote, not a player: the sound comes out of
the Spotify app on the Mac (or any device on the account), because Spotify's own streams are DRM and closed. Needs Premium.

    POST /api/spotify/page   Authorization: Bearer <token>   body: one command
        now | play | pause | next | prev | search WORDS | play URI (spotify:track:..., spotify:album:...)
    200 text/html: a plain page with what is playing, Play/Pause/Next/Prev links and, after search, the tracks as Play links
    400 no such command   401 no or wrong token   404 other path   405 not POST   502 Spotify refused (the text says why)

One-time login (opens the browser once, keeps a refresh token in ~/.claude-relay-spotify.json, mode 600):
    python3 tools/claude-relay/spotify.py --login CLIENT_ID      (the app's redirect URI must be http://127.0.0.1:8768/callback)
Then, as with yt.py:
    python3 tools/claude-relay/spotify.py --token-file ~/.claude-relay-token      (port 8769, loopback: QEMU)
    python3 tools/claude-relay/spotify.py --lan --token-file ... --tls-cert ... --tls-key ...            (the Pi)
--fixture FILE answers every command from a saved /v1/me/player reply (and a search reply under "search"), for the checks.
"""
import argparse, hmac, html, http.server, json, os, re, shutil, ssl, sys, time, urllib.parse, urllib.request, urllib.error
import base64, hashlib, secrets, webbrowser

DEFAULT_PORT = 8769   # the relay's port plus three (stt.py +1, yt.py +2); the login callback uses 8768
CALLBACK = "http://127.0.0.1:8768/callback"
STORE = os.path.expanduser("~/.claude-relay-spotify.json")
SCOPE = "user-read-playback-state user-modify-playback-state"
URI = re.compile(r"spotify:(track|album|playlist|artist):[A-Za-z0-9]{22}")
SIMPLE = {"play": ("PUT", "/me/player/play"), "pause": ("PUT", "/me/player/pause"),
          "next": ("POST", "/me/player/next"), "prev": ("POST", "/me/player/previous")}


def token_call(**form):
    req = urllib.request.Request("https://accounts.spotify.com/api/token", urllib.parse.urlencode(form).encode(), method="POST")
    with urllib.request.urlopen(req, timeout=20) as r: return json.load(r)


def login(client_id):
    """PKCE, so no client secret ever lives on disk: the browser sends a code to a one-shot loopback server."""
    verifier = secrets.token_urlsafe(64)
    challenge = base64.urlsafe_b64encode(hashlib.sha256(verifier.encode()).digest()).rstrip(b"=").decode()
    got = {}
    class H(http.server.BaseHTTPRequestHandler):
        def log_message(self, *a): pass
        def do_GET(self):
            got.update(urllib.parse.parse_qs(urllib.parse.urlparse(self.path).query))
            self.send_response(200); self.send_header("Content-Type", "text/plain"); self.end_headers(); self.wfile.write(b"Spotify linked. Close this tab.")
    srv = http.server.HTTPServer(("127.0.0.1", 8768), H)
    url = "https://accounts.spotify.com/authorize?" + urllib.parse.urlencode(dict(
        client_id=client_id, response_type="code", redirect_uri=CALLBACK, scope=SCOPE, code_challenge_method="S256", code_challenge=challenge))
    print("open this if the browser does not:", url); webbrowser.open(url)
    while "code" not in got and "error" not in got: srv.handle_request()
    if "code" not in got: sys.exit("spotify: login refused: %s" % got["error"][0])
    t = token_call(grant_type="authorization_code", code=got["code"][0], redirect_uri=CALLBACK, client_id=client_id, code_verifier=verifier)
    fd = os.open(STORE, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
    with os.fdopen(fd, "w") as f: json.dump({"client_id": client_id, "refresh_token": t["refresh_token"]}, f)
    print("spotify: linked, refresh token in", STORE)


class Api:
    """The Web API with one cached access token; refreshes a minute early."""
    def __init__(self): self.access, self.until = "", 0
    def bearer(self):
        if time.time() > self.until - 60:
            s = json.load(open(STORE))
            t = token_call(grant_type="refresh_token", refresh_token=s["refresh_token"], client_id=s["client_id"])
            self.access, self.until = t["access_token"], time.time() + t["expires_in"]
            if t.get("refresh_token"):   # Spotify may rotate it
                s["refresh_token"] = t["refresh_token"]; json.dump(s, open(STORE, "w"))
        return self.access
    def call(self, method, path, body=None):
        req = urllib.request.Request("https://api.spotify.com/v1" + path, json.dumps(body).encode() if body else None, method=method,
                                     headers={"Authorization": "Bearer " + self.bearer(), "Content-Type": "application/json"})
        try:
            with urllib.request.urlopen(req, timeout=20) as r: raw = r.read(); return json.loads(raw) if raw else {}
        except urllib.error.HTTPError as e:
            msg = e.read().decode("utf-8", "replace")[:200]
            raise RuntimeError("Spotify %d: %s" % (e.code, "no active device: open Spotify on the Mac once" if e.code == 404 else msg))


def parse(body):
    """(command, argument) or None for anything that is not one of the commands."""
    w = body.strip().split(None, 1) + [""]
    c, arg = w[0].lower(), w[1].strip()
    if c in ("now", "play", "pause", "next", "prev") and (not arg or c == "play"):
        if c == "play" and arg and not URI.fullmatch(arg): return None
        return c, arg
    if c == "search" and 0 < len(arg) <= 100: return c, arg
    return None


def run(cmd, arg, api, fixture):
    """Does the command, returns (now playing reply, search reply or None)."""
    if fixture: return fixture, (fixture.get("search") if cmd == "search" else None)
    found = None
    if cmd == "search":
        found = api.call("GET", "/search?" + urllib.parse.urlencode({"q": arg, "type": "track", "limit": 10}))
    elif cmd == "play" and arg:
        api.call("PUT", "/me/player/play", {"uris": [arg]} if arg.startswith("spotify:track:") else {"context_uri": arg})
        time.sleep(0.6)
    elif cmd in SIMPLE:
        api.call(*SIMPLE[cmd]); time.sleep(0.6)   # Spotify lags a moment before /player shows the change
    return api.call("GET", "/me/player"), found


def page(now, found):
    """The remote as a page Blackcomb reads: what is playing, the four controls, and any search hits."""
    item = (now or {}).get("item") or {}
    title = html.escape(item.get("name") or "Nothing playing")
    who = ", ".join(a["name"] for a in item.get("artists", [])) + (" - " + item["album"]["name"] if item.get("album") else "")
    state = "Playing" if (now or {}).get("is_playing") else "Paused"
    dev = ((now or {}).get("device") or {}).get("name")
    bar = " | ".join('<a href="spotify:%s">%s</a>' % (c, c.capitalize()) for c in ("prev", "pause" if state == "Playing" else "play", "next"))
    rows = "".join('<li><a href="spotify:play %s">%s</a> - %s</li>' % (t["uri"], html.escape(t["name"]), html.escape(", ".join(a["name"] for a in t["artists"])))
                   for t in ((found or {}).get("tracks") or {}).get("items", []))
    return ("<html><head><title>Spotify</title></head><body><h1>%s</h1><p>%s</p><p>%s%s</p><p>%s</p>%s</body></html>"
            % (title, html.escape(who), state, " on " + html.escape(dev) if dev else "", bar, "<h2>Results</h2><ul>%s</ul>" % rows if rows else ""))


class Handler(http.server.BaseHTTPRequestHandler):
    token = ""; fixture = None; api = None
    def log_message(self, fmt, *a): sys.stderr.write("spotify %s %s\n" % (self.address_string(), fmt % a))
    def reply(self, status, text, kind="text/plain"):
        body = text.encode("utf-8", "replace")
        self.send_response(status); self.send_header("Content-Type", kind + "; charset=utf-8"); self.send_header("Content-Length", str(len(body)))
        self.send_header("Connection", "close"); self.end_headers(); self.wfile.write(body)
    def do_GET(self): self.reply(405, "POST /api/spotify/page")
    def do_POST(self):
        if self.path != "/api/spotify/page": return self.reply(404, "not here, POST /api/spotify/page")
        auth = self.headers.get("Authorization", "")
        if not (auth.startswith("Bearer ") and hmac.compare_digest(auth[7:].strip(), self.token)): return self.reply(401, "no or wrong token")
        n = int(self.headers.get("Content-Length") or 0)
        cmd = parse(self.rfile.read(n).decode("utf-8", "replace")) if 0 < n <= 200 else None
        if not cmd: return self.reply(400, "now, play [URI], pause, next, prev, or search WORDS")
        try: now, found = run(*cmd, self.api, self.fixture)
        except Exception as e: return self.reply(502, "%s" % e)
        self.reply(200, page(now, found), "text/html")


def main():
    ap = argparse.ArgumentParser(description="Spotify remote pages for Blackcomb on the Pi, from the Web API on the Mac.")
    ap.add_argument("--login", metavar="CLIENT_ID", help="link the Spotify account once, then exit")
    ap.add_argument("--port", type=int, default=DEFAULT_PORT)
    ap.add_argument("--bind", default="127.0.0.1")
    ap.add_argument("--lan", action="store_true", help="listen on every interface (the Pi); requires TLS")
    ap.add_argument("--tls-cert"); ap.add_argument("--tls-key")
    ap.add_argument("--token-file", help="file holding the shared token (else CLAUDE_RELAY_TOKEN)")
    ap.add_argument("--fixture", help="answer every command from this saved /v1/me/player JSON (the checks)")
    a = ap.parse_args()
    if a.login: return login(a.login)
    if bool(a.tls_cert) != bool(a.tls_key): ap.error("--tls-cert and --tls-key must be supplied together")
    if a.lan and not a.tls_cert: ap.error("--lan requires --tls-cert and --tls-key; refusing a cleartext bearer")
    if a.lan: a.bind = "0.0.0.0"
    tok = open(os.path.expanduser(a.token_file)).read().strip() if a.token_file else os.environ.get("CLAUDE_RELAY_TOKEN", "")
    if not 16 <= len(tok) <= 63: sys.exit("spotify: need a token of 16 to 63 characters: --token-file FILE or CLAUDE_RELAY_TOKEN")
    if not a.fixture and not os.path.exists(STORE): sys.exit("spotify: not linked yet: --login CLIENT_ID first")
    Handler.token, Handler.api = tok, Api()
    if a.fixture: Handler.fixture = json.load(open(a.fixture))
    srv = http.server.ThreadingHTTPServer((a.bind, a.port), Handler)
    if a.tls_cert:
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        context.minimum_version = ssl.TLSVersion.TLSv1_2
        context.load_cert_chain(os.path.expanduser(a.tls_cert), os.path.expanduser(a.tls_key))
        srv.socket = context.wrap_socket(srv.socket, server_side=True)
    print("spotify: listening on %s:%d%s" % (a.bind, a.port, ", fixture " + a.fixture if a.fixture else ""), flush=True)
    try: srv.serve_forever()
    except KeyboardInterrupt: pass


if __name__ == "__main__": main()
