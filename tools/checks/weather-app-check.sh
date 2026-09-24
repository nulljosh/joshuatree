#!/bin/bash
# MANUAL: never ran on the GitHub runner before 2026-09-23; promote to ci-suite.sh one at a time after three green runs on main.
# Weather app, headless, no real internet needed. Four boots of the real
# kernel against a local fake ip-api/Open-Meteo server (the kernel is pointed
# at it with the multiboot command line, -append "wxhost=10.0.2.2:PORT"):
#
#   success   valid JSON            -> wxstate=ok,      window "ok live";
#             then the server turns bad and R is pressed -> "bad stale"
#             (the last good reading stays up, labelled, never a blank)
#   bad       403 "Host not allowed" (the exact reply the demo's proxy gave
#             Open-Meteo, the real cause of the empty Weather window)
#                                   -> wxstate=bad,     window "bad sample";
#             then the server turns good and R is pressed -> "ok live"
#   timeout   server accepts, never answers
#                                   -> wxstate=timeout, window "timeout sample"
#   offline   no NIC at all         -> wxstate=offline, window "offline sample";
#             opened three times, still exactly one wxfetch (issue #13: a
#             failed fetch must not re-run on every open/repaint)
#
# The kernel mirrors what it fetched (wxstate=) and what the window actually
# drew (wxwin=<state> live|stale|sample) to serial; this reads those lines.
# Never opens a window: -display none, input over QMP.

set -e
cd "$(dirname "$0")/../.."
make -s kernel.elf

python3 - <<'PYEOF'
import http.server, json, os, socket, subprocess, sys, tempfile, threading, time

GEO = b'{"status":"success","country":"Canada","city":"Langley","zip":"V3A","lat":49.0983,"lon":-122.6498,"isp":"test"}'
# Carries the real reply's traps: current_units and daily_units repeat every
# key with string values before the real current/daily objects. Same shape and
# field list as the one request the kernel makes (current + five daily days).
WX = (b'{"latitude":49.09,"longitude":-122.57,"generationtime_ms":0.88,"utc_offset_seconds":-25200,"timezone":"America/Vancouver","timezone_abbreviation":"GMT-7","elevation":6.0,'
      b'"current_units":{"time":"iso8601","interval":"seconds","temperature_2m":"\xc2\xb0C","apparent_temperature":"\xc2\xb0C","relative_humidity_2m":"%","wind_speed_10m":"km/h","weather_code":"wmo code"},'
      b'"current":{"time":"2026-09-20T16:15","interval":900,"temperature_2m":14.2,"apparent_temperature":12.8,"relative_humidity_2m":69,"wind_speed_10m":11.4,"weather_code":3},'
      b'"daily_units":{"time":"iso8601","weather_code":"wmo code","temperature_2m_max":"\xc2\xb0C","temperature_2m_min":"\xc2\xb0C"},'
      b'"daily":{"time":["2026-09-20","2026-09-21","2026-09-22","2026-09-23","2026-09-24"],"weather_code":[3,0,61,71,95],'
      b'"temperature_2m_max":[17.6,21.2,15.4,3.1,16.5],"temperature_2m_min":[9.4,8.5,9.2,-2.6,9.0]}}')
# 2026-09-20 is a Sunday, so the forecast row the window draws must read:
ROW = "wxrow=5 Sun,Mon,Tue,Wed,Thu facts=yes"
WANT_FIELDS = ("apparent_temperature", "relative_humidity_2m", "wind_speed_10m", "daily=weather_code,temperature_2m_max,temperature_2m_min", "forecast_days=5", "timezone=auto")

def make_server(state):
    class H(http.server.BaseHTTPRequestHandler):
        def log_message(self, *a): pass
        def send(self, code, body, ctype="application/json"):
            self.send_response(code); self.send_header("Content-Type", ctype)
            self.send_header("Content-Length", str(len(body))); self.end_headers(); self.wfile.write(body)
        def do_GET(self):
            if self.path.startswith("/json/"): return self.send(200, GEO)
            mode = state["mode"]
            state["paths"].append(self.path)
            if mode == "ok": return self.send(200, WX)
            if mode == "bad": return self.send(403, b"Host not allowed", "text/plain")
            if mode == "hang": time.sleep(300); return
    srv = http.server.ThreadingHTTPServer(("127.0.0.1", 0), H)
    srv.daemon_threads = True
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    return srv

LOGICAL_W, LOGICAL_H = 960, 540
DOCK_ICON, DOCK_GAP, SLOT0_X, ICON_ROW_Y, WEATHER_SLOT = 37, 6, 247, 487, 8
WEATHER_X = SLOT0_X + WEATHER_SLOT * (DOCK_ICON + DOCK_GAP) + DOCK_ICON // 2

class Boot:
    def __init__(self, name, mode):
        self.name, self.state = name, {"mode": mode, "paths": []}
        self.dir = tempfile.mkdtemp(prefix="jt-wx-" + name + "-")
        self.log, self.sock = os.path.join(self.dir, "serial.log"), os.path.join(self.dir, "qmp.sock")
        args = ["qemu-system-i386", "-kernel", "kernel.elf", "-display", "none", "-vga", "std",
                "-qmp", "unix:%s,server,nowait" % self.sock, "-serial", "file:" + self.log]
        if mode == "offline":
            args += ["-nic", "none"]
        else:
            self.srv = make_server(self.state)
            args += ["-net", "nic,model=rtl8139", "-net", "user",
                     "-append", "wxhost=10.0.2.2:%d" % self.srv.server_address[1]]
        self.q = subprocess.Popen(args, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        s = None
        for _ in range(100):
            time.sleep(0.1)
            try:
                s = socket.socket(socket.AF_UNIX); s.connect(self.sock); break
            except OSError: s = None
        if s is None: raise RuntimeError("QMP socket never came up")
        self.f = s.makefile("rw"); self.f.readline(); self.cmd({"execute": "qmp_capabilities"})
    def cmd(self, o):
        self.f.write(json.dumps(o) + "\n"); self.f.flush()
        while True:
            r = json.loads(self.f.readline())
            if "return" in r or "error" in r: return r
    def serial(self):
        try: return open(self.log, "rb").read().decode("latin-1")
        except FileNotFoundError: return ""
    def wait(self, needle, secs):
        end = time.time() + secs
        while time.time() < end:
            if needle in self.serial(): return True
            time.sleep(0.25)
        return False
    def click_weather(self):
        self.cmd({"execute": "input-send-event", "arguments": {"events": [
            {"type": "abs", "data": {"axis": "x", "value": int(WEATHER_X * 32768 / LOGICAL_W)}},
            {"type": "abs", "data": {"axis": "y", "value": int(ICON_ROW_Y * 32768 / LOGICAL_H)}}]}})
        time.sleep(0.4)
        for down in (True, False):
            self.cmd({"execute": "input-send-event", "arguments": {"events": [{"type": "btn", "data": {"down": down, "button": "left"}}]}})
            time.sleep(0.15)
    def screendump(self, path):
        self.cmd({"execute": "screendump", "arguments": {"filename": path}})
    def key(self, k):
        self.cmd({"execute": "send-key", "arguments": {"keys": [{"type": "qcode", "data": k}]}})
    def close(self):
        try: self.cmd({"execute": "quit"})
        except Exception: pass
        try: self.q.wait(timeout=5)
        except Exception: self.q.kill()

results = {}
def scenario(name, mode, steps):
    b = None
    try:
        b = Boot(name, mode)
        err = steps(b)
        results[name] = err or "ok"
        if err: results[name] += "\n      serial: " + " | ".join(l for l in b.serial().splitlines() if l.startswith("wx"))[-400:]
    except Exception as e:
        results[name] = "exception: %r" % (e,)
    finally:
        if b: b.close()

def s_success(b):
    if not b.wait("wxstate=ok", 120): return "never reached wxstate=ok against a valid reply"
    if "wx=14" not in b.serial(): return "parsed reading is not the served 14.2C"
    if "wxextra=yes days=5" not in b.serial(): return "feels like / humidity / wind or the five daily entries were not parsed from the reply"
    missing = [f for f in WANT_FIELDS if not any(f in p for p in b.state["paths"])]
    if missing: return "the one forecast request does not ask for: " + ", ".join(missing)
    if len([p for p in b.state["paths"] if p.startswith("/v1/forecast")]) != 1: return "more than one forecast request for a single reading"
    b.click_weather()
    if not b.wait("wxwin=ok live", 20): return "window did not show the live reading"
    if not b.wait(ROW, 10): return "forecast row did not render five day cards with the served weekdays"
    if os.environ.get("WX_SCREENDUMP"): time.sleep(1.0); b.screendump(os.environ["WX_SCREENDUMP"]); time.sleep(0.5)
    b.state["mode"] = "bad"; b.key("r")
    if not b.wait("wxstate=bad", 60): return "R did not trigger a refetch"
    if not b.wait("wxwin=bad stale", 20): return "after a failed retry the window did not fall back to the last good reading"
    if b.serial().count(ROW) < 2: return "the stale fallback dropped the last good forecast row"
    if os.environ.get("WX_SCREENDUMP_STALE"): time.sleep(1.0); b.screendump(os.environ["WX_SCREENDUMP_STALE"]); time.sleep(0.5)

def s_bad(b):
    if not b.wait("wxstate=bad forecast: HTTP 403", 120): return "a 403 reply was not reported as a bad response"
    b.click_weather()
    if not b.wait("wxwin=bad sample", 20): return "window did not show the bad-response state over labelled sample data"
    if not b.wait("wxrow=5 Mon,Tue,Wed,Thu,Fri facts=yes", 10): return "sample face did not render its labelled sample forecast row"
    if os.environ.get("WX_SCREENDUMP_SAMPLE"): time.sleep(1.0); b.screendump(os.environ["WX_SCREENDUMP_SAMPLE"]); time.sleep(0.5)
    b.state["mode"] = "ok"; b.key("r")
    if not b.wait("wxwin=fetching", 20): return "R did not show the fetching state"
    if not b.wait("wxwin=ok live", 60): return "retry against a now-good server did not land a live reading"
    if not b.wait(ROW, 10): return "after a good retry the forecast row did not switch to the served days"

def s_timeout(b):
    if not b.wait("wxstate=timeout forecast: reply timeout", 240): return "a server that never answers was not reported as a timeout"
    b.click_weather()
    if not b.wait("wxwin=timeout sample", 20): return "window did not show the timeout state"

def s_offline(b):
    if not b.wait("wxstate=offline", 120): return "a NIC-less boot was not reported as offline"
    for i in range(3):
        b.click_weather(); time.sleep(1.5)
        if i == 0 and not b.wait("wxwin=offline sample", 20): return "window did not show the offline state"
        b.key("esc"); time.sleep(1.0)
    n = b.serial().count("wxfetch")
    if n != 1: return "weather_fetch ran %d times across 3 opens (want 1): a failed fetch re-runs per open" % n

threads = [threading.Thread(target=scenario, args=a) for a in (
    ("success", "ok", s_success), ("bad-response", "bad", s_bad),
    ("timeout", "hang", s_timeout), ("offline", "offline", s_offline))]
for t in threads: t.start()
for t in threads: t.join()

failed = False
for name in ("success", "bad-response", "timeout", "offline"):
    r = results.get(name, "did not run")
    print(("ok:   " if r == "ok" else "FAIL: ") + name + ("" if r == "ok" else ": " + r))
    failed |= r != "ok"
if failed: sys.exit(1)
print("PASS: Weather shows live data, and a labelled fallback plus the reason for offline, bad response and timeout; R retries")
PYEOF
