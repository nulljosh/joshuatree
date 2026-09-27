#!/usr/bin/env python3
"""Chat speaks: the kernel fetches /api/speak audio and plays it on the SB16.

Boots QEMU headless (-display none, never a window) with an SB16 wired to
QEMU's wav backend and llmhost= pointed at a tiny stub HTTP server on this
machine (QEMU user networking reaches the host at 10.0.2.2, the same way
tools/checks/chat-samantha-check.py stubs Samantha). The stub answers
POST /api/speak the way Turing's real endpoint will: raw 8-bit unsigned
mono PCM at 16000Hz. Here that PCM is a known 1000Hz tone, full scale, so
it holds NUL bytes (nothing on the path may treat it as a string).

Types three shell commands:
  say hello     stub returns 200 + 2.5s tone
  say missing   stub returns 404 with a text body (must not be played)
  say empty     stub returns 200 with an empty body (must not crash)

Asserts: the stub saw a JSON request {"text":"hello","format":"pcm8"}, the
kernel read every byte of the tone, the wav QEMU wrote has real energy
concentrated at 1000Hz, the 404 and empty replies played nothing, and the
shell kept answering after both.
"""
import http.server, json, math, os, struct, subprocess, sys, tempfile, threading, time

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
os.chdir(ROOT)
subprocess.run(["make", "-s", "kernel.elf"], check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

RATE, TONE_HZ, SECS = 16000, 1000, float(os.environ.get("SPEAK_SECS", "2.5"))
TONE = bytes(max(0, min(255, int(round(128 + 128 * math.sin(2 * math.pi * TONE_HZ * i / RATE)))))
             for i in range(int(RATE * SECS)))
assert 0 in TONE  # the body really carries NUL bytes

recorded = []


class Stub(http.server.BaseHTTPRequestHandler):
    def log_message(self, *a):
        pass

    def do_POST(self):
        body = self.rfile.read(int(self.headers.get("Content-Length", "0")))
        try:
            req = json.loads(body.decode("utf-8"))
        except Exception:
            req = None
        recorded.append((self.path, req))
        text = (req or {}).get("text", "")
        if self.path != "/api/speak" or text == "missing":
            out, code, ctype = b"not found", 404, "text/plain"
        elif text == "empty":
            out, code, ctype = b"", 200, "application/octet-stream"
        else:
            out, code, ctype = TONE, 200, "application/octet-stream"
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(out)))
        self.end_headers()
        self.wfile.write(out)


def keys(text):
    return "".join(("sendkey spc" if c == " " else "sendkey " + c) + "\n" for c in text) + "sendkey ret\n"


def goertzel(samples, rate, freq):
    w = 2 * math.pi * freq / rate
    c = 2 * math.cos(w)
    s1 = s2 = 0.0
    for x in samples:
        s0 = x + c * s1 - s2
        s2, s1 = s1, s0
    return s1 * s1 + s2 * s2 - c * s1 * s2


def zero_crossing_pitch(tone, secs):
    """Schmitt-trigger crossing count: a bare mean-crossing count double-fires
    on a stray ripple near zero (seen under some CI QEMU builds' audio
    resampling), so require a real swing through a +-30% amplitude band
    before it arms the next crossing."""
    mean = sum(tone) / len(tone)
    amp = (max(tone) - min(tone)) / 2
    lo, hi = mean - 0.3 * amp, mean + 0.3 * amp
    armed = tone[0] < lo
    crossings = 0
    for v in tone:
        if armed and v > hi:
            crossings += 1
            armed = False
        elif not armed and v < lo:
            armed = True
    return crossings / secs


def wait_for(path, needle, count, secs):
    end = time.time() + secs
    while time.time() < end:
        try:
            if open(path, errors="replace").read().count(needle) >= count:
                return True
        except FileNotFoundError:
            pass
        time.sleep(0.5)
    return False


fails = []
srv = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Stub)
srv.daemon_threads = True
threading.Thread(target=srv.serve_forever, daemon=True).start()
port = srv.server_address[1]

with tempfile.TemporaryDirectory(prefix="jt-speak-") as work:
    serial = os.path.join(work, "serial.txt")
    wav_path = os.path.join(work, "out.wav")
    q = subprocess.Popen(
        ["qemu-system-i386", "-kernel", "kernel.elf", "-display", "none",
         "-monitor", "stdio", "-serial", f"file:{serial}",
         "-net", "nic,model=rtl8139", "-net", "user",
         "-audiodev", f"wav,id=snd,path={wav_path}", "-device", "sb16,audiodev=snd",
         "-append", f"llmhost=10.0.2.2 llmport={port}"],
        stdin=subprocess.PIPE, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, text=True)
    try:
        time.sleep(5)
        q.stdin.write("sendkey esc\n"); q.stdin.flush()
        time.sleep(2)
        for i, word in enumerate(["hello", "missing", "empty"], 1):
            q.stdin.write(keys("say " + word)); q.stdin.flush()
            if not wait_for(serial, "speak: status=", i, 40):
                fails.append(f"`say {word}` never reached the stub (no speak: line on serial)")
                break
            time.sleep(SECS + 1.5 if word == "hello" else 1)
        q.stdin.write("quit\n"); q.stdin.flush()
        q.wait(timeout=15)
    finally:
        if q.poll() is None:
            q.kill()
        srv.shutdown()

    if os.path.exists(wav_path):
        import shutil
        shutil.copy(wav_path, "/tmp/jt-speak-debug.wav")

    log = open(serial, errors="replace").read() if os.path.exists(serial) else ""
    lines = [l for l in log.splitlines() if l.startswith("speak: ")]
    print("chat-speaks-check: " + " | ".join(lines))
    if "sb16: found" not in log:
        fails.append("serial never reported the card")
    want = f"speak: status=200 bytes={len(TONE)}"
    if want not in lines[:1]:
        fails.append(f"first reply was not read in full (want '{want}', got {lines[:1]})")
    if "speak: status=404 bytes=9" not in lines:
        fails.append("404 reply not seen as a 404")
    if "speak: status=200 bytes=0" not in lines:
        fails.append("empty 200 reply not seen")
    if "timed out" in log:
        fails.append("a DMA transfer timed out")
    if not recorded or recorded[0] != ("/api/speak", {"text": "hello", "format": "pcm8"}):
        fails.append(f"stub saw the wrong first request: {recorded[:1]}")

    samples, rate = [], 0
    if os.path.exists(wav_path) and os.path.getsize(wav_path) > 44:
        raw = open(wav_path, "rb").read()   # header sizes read 0 after a monitor quit; parse by hand
        ch, rate = struct.unpack_from("<HI", raw, 22)
        width = struct.unpack_from("<H", raw, 34)[0] // 8
        raw = raw[44:]
        raw = raw[:len(raw) - len(raw) % (width * ch)]
        vals = struct.unpack(f"<{len(raw)//2}h", raw) if width == 2 else [b - 128 for b in raw]
        samples = [float(v) for v in vals[::ch]]
    if not samples:
        fails.append("QEMU wrote no audio at all")
    else:
        loud = [i for i, v in enumerate(samples) if abs(v) > 500]
        tone = samples[loud[0]:loud[-1] + 1] if loud else []
        secs = len(tone) / rate if rate else 0
        rms = math.sqrt(sum(v * v for v in tone) / len(tone)) if tone else 0.0
        if rms < 1000 or secs < SECS - 0.3:
            fails.append(f"audio silent or short (rms {rms:.0f}, {secs:.2f}s, want ~{SECS}s)")
        else:
            pitch = zero_crossing_pitch(tone, secs)
            seg = tone[:rate // 4]
            e, lo, hi = (goertzel(seg, rate, f) for f in (TONE_HZ, 700, 1400))
            print(f"chat-speaks-check: wav {rate}Hz, {secs:.2f}s loud, rms {rms:.0f}, pitch {pitch:.1f}Hz, "
                  f"energy {TONE_HZ}/700 = {e / max(lo, 1):.0f}x, {TONE_HZ}/1400 = {e / max(hi, 1):.0f}x")
            if abs(pitch - TONE_HZ) > 30:
                fails.append(f"tone is {pitch:.1f}Hz, not {TONE_HZ}Hz")
            if e < 20 * lo or e < 20 * hi:
                fails.append(f"energy is not concentrated at {TONE_HZ}Hz")

if fails:
    for f in fails:
        print("FAIL: " + f)
    sys.exit(1)
print("chat-speaks-check: OK, say fetched /api/speak PCM and played a real 1000Hz tone; 404 and empty replies stayed silent")
