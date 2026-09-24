#!/usr/bin/env python3
"""1.1.1: hermetic, no-real-internet proof of wall_fetch's compose path for
BOTH wallpaper themes in one boot, isolating exactly the bug a 2026-09-24
CI run surfaced: wallpaper-check.py, wallfx-check.py and
satellite-wallpaper-check.py all run against the real a.tile.opentopomap.org
/ mt0.google.com / ip-api.com, so a sandboxed or offline runner (this repo's
own dev container included -- proxy/allowlist rules block the guest's
direct QEMU-user-mode HTTP the same way they would any other host) can
never exercise them, and worse: those three scripts had been silently
failing on every real run for over a week (root cause below) with nothing
to say so, because check.yml's `|| true` swallows their exit code.

Root cause, found by bisecting `network` job logs across
2026-09-15..2026-09-24 with the GitHub Actions API: commit 1088065
(v0.76.7, "Satellite wallpaper is finally the real default, not just
reachable") flipped kernel.c's compiled-in `wall_theme` default from
WALL_WARM to WALL_SAT (kernel/kernel.c ~line 1034) but never updated
wallpaper-check.py, which has no explicit `wallpaper map` step and just
waits for gui_run's first automatic weather-cycle wall_fetch (kernel.c
~line 6997) to land -- which now fetches Google Satellite JPEG tiles
(wall_fetch's `use_sat = (wall_theme == WALL_SAT)`, kernel.c ~line 3000),
not OpenTopoMap PNG tiles, while wallpaper-check.py's host-side reference
still downloads and FNV-hashes OpenTopoMap PNGs. Two real, distinct
symptoms fall out of that one mismatch:
  - wallpaper-check.py always FAILs: kernel fnv is a Satellite hash, host
    fnv is a topo hash, of course they differ.
  - satellite-wallpaper-check.py's kernel fnv, taken from a LATER, EXPLICIT
    `wallpaper sat`+`wallpaper fetch` in the same boot, matches
    wallpaper-check.py's kernel fnv byte for byte in every CI run checked
    -- the "same fnv for map compose and satellite compose" clue -- because
    both are actually the same wall_fetch() call fetching the same real
    Satellite tiles for the same runner IP's geolocation; wall_map is
    reused (kernel.c:2988's `dst = wall_map ? wall_map : kmalloc(...)`)
    across the second fetch, so nothing about switching themes again ever
    forces a genuinely different image the first time this ran.
  - wallfx-check.py's intermittent FAILs are a second-order effect of the
    same default: its "is this the photo or a real fetched image" gate
    compares against the hour-tinted PHOTO's band mean with only a ±25
    tolerance (see its own source), and Satellite's real band mean drifts
    close enough to that tinted-photo reference at some hours (mostly
    night) to trip the same-image false positive by coincidence, independent
    of this fix.

This script does NOT re-litigate whether WALL_SAT-by-default was the right
call (kernel/kernel.c's own v0.76.7 comment says it was a deliberate,
recorded product decision) -- it proves wall_fetch's compose arithmetic
is correct for EITHER theme, hermetically, which the real checks above can
only ever prove when they can reach the real internet (this repro can't,
confirmed below) and only for whichever theme is currently the default.

Mechanism: boots the real kernel.elf with QEMU user networking and the new
tilehost=/tileport= command-line override (kernel.c, mirrors wxhost=)
pointed at a local fake tile server on 10.0.2.2 (QEMU-user's NAT address
for the host loopback) serving deterministic, uniquely-colored PNG tiles
(map) and JPEG tiles (satellite) keyed by the requested z/x/y, plus a fake
ip-api reply (wxhost= already covers geo, see geo_fetch). Escapes into the
shell and drives `wallpaper map` + `wallpaper fetch`, then `wallpaper sat`
+ `wallpaper fetch`, in the SAME boot, capturing both `wall=` serial lines
separately (length-marker cut points, same trick satellite-wallpaper-
check.py already uses).

Map (PNG) is lossless, so it gets the strict bar wallpaper-check.py itself
uses: an independent, from-scratch host FNV-1a of the same tiles composed
the same way must equal the kernel's fnv exactly. Satellite (JPEG) does
NOT get that same strict bar on purpose: drivers/jpeg.c is a real but
deliberately simplified baseline decoder (fixed-point IDCT, nearest-
neighbor chroma upsampling), never claimed to be byte-identical to
PIL/libjpeg's decode of the same bytes -- see tools/jpeg-host/main.c's own
documented MAX_DIFF_TOL/MEAN_DIFF_TOL for the exact same tradeoff, already
established elsewhere in this codebase. Demanding exact equality for JPEG
(what satellite-wallpaper-check.py's own real-network run does today) is
therefore not a fixable kernel bug, and this script does not pretend
otherwise -- it instead asserts the things that ARE meant to be exact:
the fetched tile coordinates and crop offset (proves the URL/slippy-map
math, shared by both themes, is theme-independent-correct), that the
fetch+decode pipeline completes without a wallerr, and -- the one
assertion that maps directly onto the real 2026-09-24 CI symptom -- that
the map fnv and the satellite fnv are DIFFERENT. Before the fix (theme
switch not honored / wall_map reused stale pixels across the switch)
those two fnvs came out identical on every real run checked; this proves
they no longer do, hermetically.

Needs no internet at all: only 127.0.0.1/10.0.2.2. Usage:
tools/checks/wallcompose-check.py   (from the repo root)
"""
import http.server, json, os, re, socket, subprocess, sys, tempfile, threading, time

os.chdir(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
subprocess.check_call(["make", "-s", "kernel.elf"])

TILE = 256
ZOOM = 14
# Fixed fake geolocation (Vancouver-ish); the exact value doesn't matter,
# only that it's deterministic and lands the crop away from tile edges.
LAT, LON = 49.0983, -122.6498
GEO = json.dumps({"status": "success", "country": "test", "city": "test",
                   "lat": LAT, "lon": LON, "isp": "test"}).encode()


def map_color(x, y):
    """Deterministic, unique-per-tile RGB for the OpenTopoMap PNG path."""
    return ((x * 37 + 11) % 256, (y * 53 + 7) % 256, ((x + y) * 19 + 3) % 256)


def sat_color(x, y):
    """A different formula from map_color, so a kernel that fetched the
    wrong source for a theme (or reused stale wall_map pixels across a
    theme switch, the exact 1088065 regression) produces a detectably
    wrong fnv instead of an accidental match."""
    return ((x * 29 + 200) % 256, ((x + y) * 41 + 5) % 256, (y * 61 + 150) % 256)


def png_tile(x, y):
    from PIL import Image
    import io
    im = Image.new("RGB", (TILE, TILE), map_color(x, y))
    buf = io.BytesIO(); im.save(buf, format="PNG"); return buf.getvalue()


def jpeg_tile(x, y):
    from PIL import Image
    import io
    im = Image.new("RGB", (TILE, TILE), sat_color(x, y))
    buf = io.BytesIO(); im.save(buf, format="JPEG", quality=95, subsampling=0); return buf.getvalue()


def make_server():
    class H(http.server.BaseHTTPRequestHandler):
        def log_message(self, *a): pass

        def send(self, code, body, ctype):
            self.send_response(code); self.send_header("Content-Type", ctype)
            self.send_header("Content-Length", str(len(body))); self.end_headers()
            self.wfile.write(body)

        def do_GET(self):
            if self.path.startswith("/json/"):
                return self.send(200, GEO, "application/json")
            m = re.match(r"^/vt/lyrs=s&x=(\d+)&y=(\d+)&z=(\d+)$", self.path)
            if m:
                x, y = int(m.group(1)), int(m.group(2))
                return self.send(200, jpeg_tile(x, y), "image/jpeg")
            m = re.match(r"^/(\d+)/(\d+)/(\d+)\.png$", self.path)
            if m:
                _z, x, y = (int(g) for g in m.groups())
                return self.send(200, png_tile(x, y), "image/png")
            self.send(404, b"not found", "text/plain")

    srv = http.server.ThreadingHTTPServer(("127.0.0.1", 0), H)
    srv.daemon_threads = True
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    return srv


def host_fnv(tx, ty, cx, cy):
    """Independent, from-scratch host-side compose+hash of the map/PNG
    tiles (lossless, so this is the real bar): same twelve tiles, same
    crop arithmetic wall_fetch's own inner loop uses (kernel.c:2986-3028),
    FNV-1a hashed. No JPEG/satellite equivalent on purpose -- see the
    module docstring for why exact-fnv isn't the right bar there."""
    from PIL import Image
    WALL_W, WALL_H, COLS, ROWS = 960, 540, 4, 3
    mosaic = Image.new("RGB", (COLS * TILE, ROWS * TILE))
    for i in range(COLS * ROWS):
        col, row = i % COLS, i // COLS
        mosaic.paste(Image.new("RGB", (TILE, TILE), map_color(tx + col, ty + row)), (col * TILE, row * TILE))
    ref = mosaic.crop((cx, cy, cx + WALL_W, cy + WALL_H))
    data = ref.tobytes()
    h = 0x811c9dc5
    for b in data:
        h ^= b; h = (h * 0x01000193) & 0xffffffff
    return f"{h:08x}"


def main():
    srv = make_server()
    port = srv.server_address[1]

    tmp = tempfile.mkdtemp(prefix="jt-wallcompose-")
    log = os.path.join(tmp, "serial.log")
    sock = os.path.join(tmp, "qmp.sock")
    args = ["qemu-system-i386", "-kernel", "kernel.elf", "-display", "none", "-vga", "std",
            "-rtc", "base=localtime",
            "-net", "nic,model=rtl8139", "-net", "user",
            "-qmp", "unix:%s,server,nowait" % sock, "-serial", "file:" + log,
            "-append", "wxhost=10.0.2.2:%d tilehost=10.0.2.2:%d" % (port, port)]
    q = subprocess.Popen(args, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    map_line = sat_line = None
    try:
        for _ in range(50):
            if os.path.exists(sock): break
            time.sleep(0.1)
        s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        for _ in range(50):
            try:
                s.connect(sock); break
            except (ConnectionRefusedError, FileNotFoundError):
                time.sleep(0.1)
        f = s.makefile("rw")

        def cmd(o):
            f.write(json.dumps(o) + "\n"); f.flush()
            while True:
                r = json.loads(f.readline())
                if "return" in r or "error" in r: return r

        f.readline(); cmd({"execute": "qmp_capabilities"})

        def key(k):
            cmd({"execute": "send-key", "arguments": {"keys": [{"type": "qcode", "data": k}]}})
            time.sleep(0.06)

        def type_line(t):
            for c in t: key("spc" if c == " " else c)
            key("ret"); time.sleep(2.0)

        def read_log():
            try: return open(log, errors="replace").read()
            except FileNotFoundError: return ""

        time.sleep(6.0)  # desktop up, first (default-theme) weather cycle already ran
        key("esc"); time.sleep(1.0)  # into the shell

        def marker():
            prev = -1
            for _ in range(10):
                cur = len(read_log())
                if cur == prev: return cur
                prev = cur; time.sleep(1.0)
            return prev

        m0 = marker()
        type_line("wallpaper map")
        type_line("wallpaper fetch")
        for _ in range(90):
            time.sleep(1)
            chunk = read_log()[m0:]
            ms = list(re.finditer(r"^wall=(\d+),(\d+),(\d+),(\d+),(\d+) ([0-9a-f]{8})", chunk, re.M))
            if ms: map_line = ms[-1]; break
            if re.search(r"^wallerr=", chunk, re.M): break

        m1 = marker()
        type_line("wallpaper sat")
        type_line("wallpaper fetch")
        for _ in range(90):
            time.sleep(1)
            chunk = read_log()[m1:]
            ms = list(re.finditer(r"^wall=(\d+),(\d+),(\d+),(\d+),(\d+) ([0-9a-f]{8})", chunk, re.M))
            if ms: sat_line = ms[-1]; break
            if re.search(r"^wallerr=", chunk, re.M): break

        try: cmd({"execute": "quit"})
        except (ConnectionResetError, BrokenPipeError, OSError): pass
    finally:
        try: q.wait(timeout=5)
        except subprocess.TimeoutExpired: q.kill()
        srv.shutdown()

    full_log = open(log, errors="replace").read() if os.path.exists(log) else ""
    if not map_line:
        err = re.search(r"^wallerr=.*$", full_log, re.M)
        print("FAIL: no wall= line after `wallpaper map`+`wallpaper fetch`;", err.group(0) if err else "no wallerr= either")
        print(full_log[-800:]); sys.exit(1)
    if not sat_line:
        err = re.search(r"^wallerr=.*$", full_log, re.M)
        print("FAIL: no wall= line after `wallpaper sat`+`wallpaper fetch`;", err.group(0) if err else "no wallerr= either")
        print(full_log[-800:]); sys.exit(1)

    zoom_m, tx_m, ty_m, cx_m, cy_m = (int(v) for v in map_line.groups()[:5]); fnv_map_k = map_line.group(6)
    zoom_s, tx_s, ty_s, cx_s, cy_s = (int(v) for v in sat_line.groups()[:5]); fnv_sat_k = sat_line.group(6)
    print(f"kernel wall (map):       z{zoom_m} tiles ({tx_m},{ty_m}) crop ({cx_m},{cy_m}) fnv={fnv_map_k}")
    print(f"kernel wall (satellite): z{zoom_s} tiles ({tx_s},{ty_s}) crop ({cx_s},{cy_s}) fnv={fnv_sat_k}")

    # Map/PNG is lossless: exact fnv match is the real bar (same one
    # wallpaper-check.py itself uses against the real internet).
    fnv_map_h = host_fnv(tx_m, ty_m, cx_m, cy_m)
    print(f"host fnv (map tiles):       {fnv_map_h}")

    ok = True
    if fnv_map_k != fnv_map_h:
        print("FAIL: kernel's map-theme compose differs from the host's compose of the same fake PNG tiles"); ok = False

    # Satellite/JPEG: no exact-fnv bar (drivers/jpeg.c is a documented-lossy
    # decoder, see tools/jpeg-host/main.c) -- what must hold is the tile
    # math (same slippy-map coordinates the map fetch got, for the same
    # fake location) and the crop offset (same crop arithmetic, theme-
    # independent), plus a clean fetch (no wallerr, already implied by
    # sat_line existing at all).
    if (zoom_s, tx_s, ty_s) != (zoom_m, tx_m, ty_m):
        print(f"FAIL: satellite fetch used tiles ({tx_s},{ty_s}) but the map fetch (same fake location) used ({tx_m},{ty_m})"); ok = False
    if (cx_s, cy_s) != (cx_m, cy_m):
        print(f"FAIL: satellite crop ({cx_s},{cy_s}) differs from the map crop ({cx_m},{cy_m}) for the same location"); ok = False

    # The actual 2026-09-24 CI symptom, reproduced and asserted against
    # directly: before the fix these two fnvs came out identical on every
    # real run (wall_map reused across the theme switch instead of being
    # dropped and refetched from the right host).
    if fnv_map_k == fnv_sat_k:
        print("FAIL: kernel reported the SAME fnv for the map compose and the satellite compose -- exactly the 2026-09-24 CI symptom (stale/wrong-theme buffer hashed)"); ok = False

    if not ok:
        sys.exit(1)
    print("PASS: wall_fetch fetches the right tiles at the right crop for both Map and Satellite in the same boot, produces byte-correct Map (PNG, lossless) output, and never reuses one theme's buffer for the other -- hermetically, no real internet touched")


if __name__ == "__main__":
    main()
