#!/usr/bin/env python3
"""Speech to text for Joshua Tree, on the Mac, next to relay.py (which this never touches).

    POST /api/stt        Authorization: Bearer <token>   (the relay's token file, same rules)
    Content-Type: application/octet-stream
    body: raw PCM, signed 16-bit little-endian, mono, 16000 Hz, no header, at most --max-seconds of it
    200 text/plain: the words, one line (empty when nothing was said); X-Took says the seconds whisper needed
    400 bad body (empty, odd length, too long)   401 no or wrong token   404 other path   405 not POST   500 whisper failed

The words come from whisper.cpp (`brew install whisper-cpp`) with its base English model; nothing leaves the Mac.
The Pi's voice mode (double-tap and hold Space) records from the USB microphone (arch/arm64/xhci.c, 16 kHz mono) and posts it here.

    python3 tools/claude-relay/stt.py --token-file ~/.claude-relay-token      (port 8766, loopback: QEMU)
    python3 tools/claude-relay/stt.py --lan --token-file ~/.claude-relay-token --tls-cert ... --tls-key ...   (the Pi)
On the LAN it takes the relay's own certificate and key: the Pi checks the same name (jt-relay) on this port as on the
relay's, and --lan without TLS is refused, like relay.py, so the bearer never crosses the room in the clear.
"""
import argparse, hmac, http.server, os, ssl, shutil, struct, subprocess, sys, tempfile, time

DEFAULT_PORT = 8766
RATE = 16000
MODEL = os.path.expanduser("~/.cache/whisper-cpp/ggml-base.en.bin")


def transcribe(pcm, model, threads):
    with tempfile.TemporaryDirectory() as d:
        wav = os.path.join(d, "in.wav")
        with open(wav, "wb") as f:
            f.write(b"RIFF" + struct.pack("<I", 36 + len(pcm)) + b"WAVEfmt " + struct.pack("<IHHIIHH", 16, 1, 1, RATE, RATE * 2, 2, 16)
                    + b"data" + struct.pack("<I", len(pcm)) + pcm)
        r = subprocess.run(["whisper-cli", "-m", model, "-f", wav, "-nt", "-np", "-t", str(threads), "-l", "en"],
                           capture_output=True, text=True, timeout=120)
        if r.returncode: raise RuntimeError(r.stderr.strip()[-300:])
        return " ".join(r.stdout.split())


class Handler(http.server.BaseHTTPRequestHandler):
    token = ""; model = MODEL; max_bytes = 0; threads = 4
    def log_message(self, fmt, *a): sys.stderr.write("stt %s %s\n" % (self.address_string(), fmt % a))
    def reply(self, status, text, took=None):
        body = text.encode("utf-8", "replace")
        self.send_response(status); self.send_header("Content-Type", "text/plain; charset=utf-8"); self.send_header("Content-Length", str(len(body)))
        if took is not None: self.send_header("X-Took", "%.2f" % took)
        self.send_header("Connection", "close"); self.end_headers(); self.wfile.write(body)
    def do_GET(self): self.reply(405, "POST /api/stt")
    def do_POST(self):
        if self.path != "/api/stt": return self.reply(404, "not here, POST /api/stt")
        auth = self.headers.get("Authorization", "")
        if not (auth.startswith("Bearer ") and hmac.compare_digest(auth[7:].strip(), self.token)): return self.reply(401, "no or wrong token")
        n = int(self.headers.get("Content-Length") or 0)
        if n <= 0 or n % 2 or n > self.max_bytes: return self.reply(400, "body must be 16-bit mono PCM at 16 kHz, up to %d bytes" % self.max_bytes)
        pcm = self.rfile.read(n)
        t0 = time.time()
        try: text = transcribe(pcm, self.model, self.threads)
        except Exception as e: return self.reply(500, "whisper: %s" % e)
        self.reply(200, text, time.time() - t0)


def main():
    ap = argparse.ArgumentParser(description="Speech to text for the Pi: raw 16 kHz PCM in, words out, whisper.cpp on the Mac.")
    ap.add_argument("--port", type=int, default=DEFAULT_PORT)
    ap.add_argument("--bind", default="127.0.0.1")
    ap.add_argument("--lan", action="store_true", help="listen on every interface (the Pi); requires TLS")
    ap.add_argument("--tls-cert", help="PEM server certificate, the relay's")
    ap.add_argument("--tls-key", help="PEM private key for --tls-cert")
    ap.add_argument("--token-file", help="file holding the shared token (else CLAUDE_RELAY_TOKEN)")
    ap.add_argument("--model", default=MODEL)
    ap.add_argument("--max-seconds", type=int, default=30)
    ap.add_argument("--threads", type=int, default=4)
    a = ap.parse_args()
    if bool(a.tls_cert) != bool(a.tls_key): ap.error("--tls-cert and --tls-key must be supplied together")
    if a.lan and not a.tls_cert: ap.error("--lan requires --tls-cert and --tls-key; refusing a cleartext bearer")
    if a.lan: a.bind = "0.0.0.0"
    tok = open(os.path.expanduser(a.token_file)).read().strip() if a.token_file else os.environ.get("CLAUDE_RELAY_TOKEN", "")
    if not 16 <= len(tok) <= 63: sys.exit("stt: need a token of 16 to 63 characters: --token-file FILE or CLAUDE_RELAY_TOKEN")
    if not shutil.which("whisper-cli"): sys.exit("stt: whisper-cli not found (brew install whisper-cpp)")
    if not os.path.exists(a.model): sys.exit("stt: no model at %s" % a.model)
    Handler.token, Handler.model, Handler.threads, Handler.max_bytes = tok, a.model, a.threads, a.max_seconds * RATE * 2
    srv = http.server.ThreadingHTTPServer((a.bind, a.port), Handler)
    if a.tls_cert:
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        context.minimum_version = ssl.TLSVersion.TLSv1_2
        context.load_cert_chain(os.path.expanduser(a.tls_cert), os.path.expanduser(a.tls_key))
        srv.socket = context.wrap_socket(srv.socket, server_side=True)
    print("stt: listening on %s:%d, model %s" % (a.bind, a.port, os.path.basename(a.model)), flush=True)
    try: srv.serve_forever()
    except KeyboardInterrupt: pass


if __name__ == "__main__": main()
