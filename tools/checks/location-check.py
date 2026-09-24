#!/usr/bin/env python3
# v0.85.5: headless proof for Settings' new Location field (roadmap.md:
# "Settings gets a Location field... Location from the internet address
# says Vancouver for Langley and cannot do better"). Never touches real
# internet -- geocoding-api.open-meteo.com is faked by a local HTTP server
# and the kernel is pointed at it the same way weather-app-check.sh's own
# fake ip-api/Open-Meteo server already does (wxhost=A.B.C.D:PORT on the
# multiboot command line, see wx_override_host in kernel.c), so this check
# cannot flake on CI's own network reachability the way a live-network
# check (tools/geo-check.sh) could.
#
# Drives the same "loctest" shell command kernel.c's geotest/weatherpaneltest
# neighbors already use for pure, boot-time, no-mouse-needed verification:
# boot to the shell (esc out of the GUI, same as settingsclick-check.sh),
# type "loctest", read the pass/fail lines back off serial. loctest itself
# (kernel/kernel.c) does the real work headlessly:
#   1. loc_geocode("Langley") against the fake server, lands in
#      loc_name/loc_lat/loc_lon AND geo_lat/geo_lon/geo_city (the exact
#      fields weather_fetch_inner and the map wallpaper's wall_fetch
#      already read -- proves Settings' new field really drives both,
#      through the existing paths, not a new one)
#   2. settings_save() + settings_load() round-trip through SETTINGS.TXT,
#      simulating a reboot the same way the shell's own "mail round trip"
#      test already simulates one for MAIL.TXT -- proves persistence
#   3. an unresolvable query (the fake server's own "no results" reply)
#      fails cleanly: loc_err set, nothing overwritten, no crash -- proves
#      the fallback/bad-input path
#
# Run against pre-location-field HEAD this fails outright: "loctest" isn't
# a recognized shell command, so the shell echoes "? loctest" and this
# script's grep for "loctest PASS" finds nothing.
import http.server, json, os, subprocess, sys, tempfile, threading, time

def make_server():
    class H(http.server.BaseHTTPRequestHandler):
        def log_message(self, *a): pass
        def do_GET(self):
            body = b'{"results":[]}'
            if self.path.startswith("/v1/search?name=Langley"):
                body = json.dumps({"results": [{"id": 1, "name": "Langley",
                    "latitude": 49.1044, "longitude": -122.6607,
                    "country": "Canada"}]}).encode()
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
    srv = http.server.ThreadingHTTPServer(("127.0.0.1", 0), H)
    srv.daemon_threads = True
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    return srv

def main():
    os.chdir(os.path.join(os.path.dirname(__file__), "..", ".."))
    subprocess.run(["make", "-s", "kernel.elf"], check=True)

    srv = make_server()
    port = srv.server_address[1]

    workdir = tempfile.mkdtemp(prefix="jt-loc-")
    log = os.path.join(workdir, "serial.log")

    def send(s):
        lines = []
        for c in s:
            lines.append("sendkey spc" if c == " " else "sendkey %s" % c)
        lines.append("sendkey ret")
        return "\n".join(lines) + "\n"

    args = ["qemu-system-i386", "-kernel", "kernel.elf", "-display", "none",
            "-monitor", "stdio", "-serial", "file:" + log,
            "-net", "nic,model=rtl8139", "-net", "user",
            "-append", "wxhost=10.0.2.2:%d" % port]

    proc = subprocess.Popen(args, stdin=subprocess.PIPE, stdout=subprocess.DEVNULL,
                             stderr=subprocess.DEVNULL)
    try:
        time.sleep(5)
        proc.stdin.write(b"sendkey esc\n")
        proc.stdin.flush()
        time.sleep(2)
        proc.stdin.write(send("loctest").encode())
        proc.stdin.flush()
        # loc_geocode makes two real (fake-server) HTTP round trips plus a
        # settings save/load round trip; give it real time to land before
        # reading serial back, same order-of-magnitude wait weather-app-
        # check.sh gives its own fake-server fetches.
        for _ in range(20):
            time.sleep(1)
            try:
                if "loctest PASS" in open(log, "r", encoding="latin-1").read() or \
                   "loctest FAIL" in open(log, "r", encoding="latin-1").read():
                    break
            except FileNotFoundError:
                pass
        proc.stdin.write(b"quit\n")
        proc.stdin.flush()
    finally:
        try:
            proc.stdin.close()
        except Exception:
            pass
        try:
            proc.wait(timeout=10)
        except Exception:
            proc.kill()
            proc.wait(timeout=5)

    try:
        serial = open(log, "r", encoding="latin-1").read()
    except FileNotFoundError:
        serial = ""

    if os.environ.get("LOC_DEBUG"):
        print("---- full serial ----")
        print(serial)
        print("----------------------")
    lines = [l for l in serial.splitlines() if l.startswith("loc")]
    print("\n".join(lines) if lines else "(no loctest output on serial)")

    if "loctest start" not in serial:
        print("FAIL: loctest never ran (command not recognized, or shell never reached)")
        sys.exit(1)
    for want in (
        "loc geocode: real lookup lands in loc_*/geo_*: ok",
        "loc persist: settings_save/settings_load round trip (simulated reboot): ok",
        "loc not-found: fails clean, error set, nothing overwritten: ok",
        "loc empty input: rejected, no crash: ok",
    ):
        if want not in serial:
            print("FAIL: missing: " + want)
            sys.exit(1)
    if "loctest PASS" not in serial:
        print("FAIL: loctest reported FAIL")
        sys.exit(1)
    print("PASS: Location geocodes through the fake server, persists across a "
          "simulated reboot, and fails clean on an unknown query -- all headless, no real internet")

if __name__ == "__main__":
    main()
