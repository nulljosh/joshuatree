#!/usr/bin/env python3
"""YouTube for Blackcomb, on the Mac, next to relay.py (which this never touches) and stt.py.

YouTube's own pages are JavaScript, which Blackcomb does not run, so the Pi asks this service instead:

    POST /api/yt/page    Authorization: Bearer <token>   (the relay's token file, same rules as stt.py)
    body: an 11 character video id ([A-Za-z0-9_-])
    200 text/html: a plain page with the title, the channel, the length, the description and a Play link
    POST /api/yt/avi     same token; body: the id, a space, and the most bytes the Pi can take (6.10.0)
    200 video/x-msvideo: the video as a clip Movies plays (motion JPEG 256 wide at 12 fps, 8-bit 11 kHz mono sound),
        cut short when the whole of it would not fit (X-Seconds says how much is in it)
    400 not a video id   401 no or wrong token   404 other path   405 not POST   502 yt-dlp or ffmpeg failed

The facts come from yt-dlp (`brew install yt-dlp`), run with the id as one argument, never through a shell. --fixture FILE
answers from a saved `yt-dlp -j` reply instead, and --fixture-avi FILE sends that clip, for the checks (no network, no
yt-dlp, no ffmpeg).

    python3 tools/claude-relay/yt.py --token-file ~/.claude-relay-token      (port 8767, loopback: QEMU)
    python3 tools/claude-relay/yt.py --lan --token-file ~/.claude-relay-token --tls-cert ... --tls-key ...   (the Pi)
On the LAN it takes the relay's own certificate and key, as stt.py does, and --lan without TLS is refused.
"""
import argparse, hmac, html, http.server, json, os, re, shutil, ssl, subprocess, sys, tempfile

DEFAULT_PORT = 8767   # the relay's port plus two (stt.py is plus one)
VIDEO_ID = re.compile(r"[A-Za-z0-9_-]{11}")


def info(vid, fixture):
    if fixture:
        with open(fixture) as f: return json.load(f)
    r = subprocess.run(["yt-dlp", "-j", "--no-playlist", "--no-warnings", "--", vid], capture_output=True, text=True, timeout=60)
    if r.returncode: raise RuntimeError(r.stderr.strip()[-300:] or "yt-dlp failed")
    return json.loads(r.stdout)


def page(vid, d):
    """The video as a page Blackcomb reads: title, who, how long, the description in paragraphs, and a Play link."""
    t = int(d.get("duration") or 0)
    length = "%d:%02d:%02d" % (t // 3600, t // 60 % 60, t % 60) if t >= 3600 else "%d:%02d" % (t // 60, t % 60)
    views = d.get("view_count")
    who = " - ".join(x for x in (d.get("channel") or d.get("uploader") or "", length, "{:,} views".format(views) if views else "") if x)
    desc = (d.get("description") or "").strip()[:3000]
    paras = "".join("<p>%s</p>" % html.escape(p) for p in desc.split("\n") if p.strip())
    title = html.escape(d.get("title") or vid)
    return ("<html><head><title>%s</title></head><body><h1>%s</h1><p>%s</p>"
            "<p><a href=\"https://www.youtube.com/watch?v=%s&amp;play=1\">Play in Movies</a></p>%s</body></html>"
            % (title, title, html.escape(who), vid, paras))


def clip(vid, cap):
    """The video as a Movies clip of at most cap bytes: one pass, then a shorter one if it came out too big."""
    with tempfile.TemporaryDirectory() as d:
        src, out = os.path.join(d, "v.mp4"), os.path.join(d, "v.avi")
        r = subprocess.run(["yt-dlp", "-q", "--no-playlist", "--no-warnings", "-f", "18/best[height<=360][ext=mp4]/best[height<=360]", "-o", src, "--", vid],
                           capture_output=True, text=True, timeout=300)
        if r.returncode or not os.path.exists(src): raise RuntimeError((r.stderr or "yt-dlp failed").strip()[-300:])
        def encode(seconds=None):
            cmd = ["ffmpeg", "-loglevel", "error", "-y", "-i", src] + (["-t", "%.1f" % seconds] if seconds else []) + [
                   "-vf", "scale=256:-2,fps=12", "-c:v", "mjpeg", "-q:v", "9", "-pix_fmt", "yuvj420p",
                   "-c:a", "pcm_u8", "-ar", "11025", "-ac", "1", "-f", "avi", out]
            e = subprocess.run(cmd, capture_output=True, text=True, timeout=600)
            if e.returncode: raise RuntimeError("ffmpeg: " + e.stderr.strip()[-300:])
            return os.path.getsize(out)
        p = subprocess.run(["ffprobe", "-v", "error", "-show_entries", "format=duration", "-of", "csv=p=0", src], capture_output=True, text=True)
        dur = float(p.stdout.strip() or 0)
        size, seconds = encode(), dur
        if size > cap and dur:
            seconds = dur * cap / size * 0.92
            size = encode(seconds)
        if size > cap: raise RuntimeError("the clip is %d bytes, over the %d the Pi can take" % (size, cap))
        return open(out, "rb").read(), seconds


class Handler(http.server.BaseHTTPRequestHandler):
    token = ""; fixture = None; fixture_avi = None
    def log_message(self, fmt, *a): sys.stderr.write("yt %s %s\n" % (self.address_string(), fmt % a))
    def reply(self, status, text, kind="text/plain"):
        body = text.encode("utf-8", "replace")
        self.send_response(status); self.send_header("Content-Type", kind + "; charset=utf-8"); self.send_header("Content-Length", str(len(body)))
        self.send_header("Connection", "close"); self.end_headers(); self.wfile.write(body)
    def do_GET(self): self.reply(405, "POST /api/yt/page or /api/yt/avi")
    def do_POST(self):
        if self.path not in ("/api/yt/page", "/api/yt/avi"): return self.reply(404, "not here, POST /api/yt/page or /api/yt/avi")
        auth = self.headers.get("Authorization", "")
        if not (auth.startswith("Bearer ") and hmac.compare_digest(auth[7:].strip(), self.token)): return self.reply(401, "no or wrong token")
        n = int(self.headers.get("Content-Length") or 0)
        words = (self.rfile.read(n).decode("ascii", "replace").split() if 0 < n <= 64 else []) + ["", ""]
        vid = words[0]
        if not VIDEO_ID.fullmatch(vid): return self.reply(400, "the body must start with one 11 character video id")
        if self.path == "/api/yt/avi":
            cap = int(words[1]) if words[1].isdigit() else 0
            if not 1 << 20 <= cap <= 1 << 30: return self.reply(400, "after the id, the most bytes the Pi can take (1 MiB to 1 GiB)")
            try: data, seconds = (open(self.fixture_avi, "rb").read(), 0) if self.fixture_avi else clip(vid, cap)
            except Exception as e: return self.reply(502, "%s" % e)
            if len(data) > cap: return self.reply(502, "the clip is over the Pi's %d bytes" % cap)
            self.send_response(200); self.send_header("Content-Type", "video/x-msvideo"); self.send_header("Content-Length", str(len(data)))
            self.send_header("X-Seconds", "%.0f" % seconds); self.send_header("Connection", "close"); self.end_headers(); self.wfile.write(data)
            return
        try: d = info(vid, self.fixture)
        except Exception as e: return self.reply(502, "yt-dlp: %s" % e)
        self.reply(200, page(vid, d), "text/html")


def main():
    ap = argparse.ArgumentParser(description="YouTube video pages for Blackcomb on the Pi, from yt-dlp on the Mac.")
    ap.add_argument("--port", type=int, default=DEFAULT_PORT)
    ap.add_argument("--bind", default="127.0.0.1")
    ap.add_argument("--lan", action="store_true", help="listen on every interface (the Pi); requires TLS")
    ap.add_argument("--tls-cert", help="PEM server certificate, the relay's")
    ap.add_argument("--tls-key", help="PEM private key for --tls-cert")
    ap.add_argument("--token-file", help="file holding the shared token (else CLAUDE_RELAY_TOKEN)")
    ap.add_argument("--fixture", help="answer every id from this saved `yt-dlp -j` JSON (the checks)")
    ap.add_argument("--fixture-avi", help="send this clip for every /api/yt/avi (the checks)")
    a = ap.parse_args()
    if bool(a.tls_cert) != bool(a.tls_key): ap.error("--tls-cert and --tls-key must be supplied together")
    if a.lan and not a.tls_cert: ap.error("--lan requires --tls-cert and --tls-key; refusing a cleartext bearer")
    if a.lan: a.bind = "0.0.0.0"
    tok = open(os.path.expanduser(a.token_file)).read().strip() if a.token_file else os.environ.get("CLAUDE_RELAY_TOKEN", "")
    if not 16 <= len(tok) <= 63: sys.exit("yt: need a token of 16 to 63 characters: --token-file FILE or CLAUDE_RELAY_TOKEN")
    if not a.fixture and not shutil.which("yt-dlp"): sys.exit("yt: yt-dlp not found (brew install yt-dlp)")
    if not a.fixture_avi and not shutil.which("ffmpeg"): sys.exit("yt: ffmpeg not found (brew install ffmpeg)")
    Handler.token, Handler.fixture, Handler.fixture_avi = tok, a.fixture, a.fixture_avi
    srv = http.server.ThreadingHTTPServer((a.bind, a.port), Handler)
    if a.tls_cert:
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        context.minimum_version = ssl.TLSVersion.TLSv1_2
        context.load_cert_chain(os.path.expanduser(a.tls_cert), os.path.expanduser(a.tls_key))
        srv.socket = context.wrap_socket(srv.socket, server_side=True)
    print("yt: listening on %s:%d%s" % (a.bind, a.port, ", fixture " + a.fixture if a.fixture else ""), flush=True)
    try: srv.serve_forever()
    except KeyboardInterrupt: pass


if __name__ == "__main__": main()
