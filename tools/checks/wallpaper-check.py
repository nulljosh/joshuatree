#!/usr/bin/env python3
"""v75 (0.67.0): headless, real-network proof of the map wallpaper chain,
the same shape as geo-check.sh (real ip-api answer from the host as the
reference) plus png-check.sh (kernel decode vs host decode, hash for hash).

Boots kernel.elf with QEMU user networking, lets gui_run's first weather
cycle run (geo_fetch, then wall_fetch, the default wall=1 setting), and
reads the one line wall_fetch mirrors to serial on success:

    wall=<zoom>,<tx>,<ty>,<cx>,<cy> <fnv1a of the 960x540x3 buffer>

Then, independently:
  1. tile coords: recomputed here from the host's OWN ip-api.com lat/lon
     with the standard slippy-map formula; a kernel that fetched the wrong
     place (stale constant, broken x87 math) fails on the first compare;
  2. pixels: the same twelve OpenTopoMap tiles are downloaded here, decoded by
     PIL, composed into the identical 960x540 crop at the kernel's reported
     offset, and FNV-1a hashed; a kernel whose PNG palette path, tile copy
     or crop arithmetic is off fails byte-for-byte here (proved: reverting
     png.c's PLTE support makes the kernel log `wallerr=png -4` and this
     script fail on "no wall= line");
  3. the screen: pmemsave of the real framebuffer after the fetch. The
     wind band (logical rows 30..395) must be the map, tinted for the
     current hour exactly the way kernel.c's daynight_calc does it
     (replicated below), not the baked photo: mean color over that band
     within a small tolerance of the tinted map's mean, and far from the
     photo's. Rain/snow particles are single pixels, invisible to a mean.

Writes /tmp/jt-wall-fb.png (the real screen) and /tmp/jt-wall-map.png
(the host-composed reference) for eyeballing. Needs internet on the host,
python3-pil, and curl.

Usage: tools/checks/wallpaper-check.py   (from the repo root, after make kernel.elf)
"""
import io, json, math, os, re, socket, subprocess, sys, time
from PIL import Image

LOG = "/tmp/jt-wall-serial.log"
RAW = "/tmp/jt-wall-fb.raw"
FB = 0xfd000000; W, H = 1920, 1080
PORT = 4451
WALL_W, WALL_H, TILE, ZOOM, COLS, ROWS = 960, 540, 256, 14, 4, 3  # mirror kernel.c's WALL_* constants (WALL_ZOOM 12->14 in v78/0.67.2, 14->15->16 across v0.76.14-15, reverted back to 14 in v0.76.16 (higher zoom was the wrong direction for "whole town" framing) -- keep this in sync by hand, it drifted stale once already)
MENUBAR_H, WIND_TOP, WIND_HORIZON = 22, 30, 395

os.chdir(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
subprocess.check_call(["make", "-s", "kernel.elf"])
for f in (LOG, RAW):
    try: os.remove(f)
    except FileNotFoundError: pass

# reference location, from the host's own ip-api answer (same service the kernel asks)
host = json.loads(subprocess.check_output(["curl", "-s", "--max-time", "10", "http://ip-api.com/json/"]))
lat, lon = float(host["lat"]), float(host["lon"])
n = 2 ** ZOOM
xf = (lon + 180.0) / 360.0 * n
yf = (1.0 - math.log(math.tan(math.radians(lat)) + 1 / math.cos(math.radians(lat))) / math.pi) / 2.0 * n
want_tx, want_ty = int(xf - 1.5), int(yf - 1.0)
print(f"host ip-api: {lat},{lon} -> z{ZOOM} tiles from ({want_tx},{want_ty})")

q = subprocess.Popen(["qemu-system-i386", "-kernel", "kernel.elf", "-display", "none", "-vga", "std",
                      "-rtc", "base=localtime",
                      "-net", "nic,model=rtl8139", "-net", "user",
                      "-qmp", f"tcp:127.0.0.1:{PORT},server,nowait", "-serial", "file:" + LOG],
                     stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
line = None
try:
    time.sleep(1.0)
    s = socket.create_connection(("127.0.0.1", PORT)); f = s.makefile("rw")
    def cmd(o):
        f.write(json.dumps(o) + "\n"); f.flush()
        while True:
            r = json.loads(f.readline())
            if "return" in r or "error" in r: return r
    f.readline()
    cmd({"execute": "qmp_capabilities"})
    for _ in range(90):
        time.sleep(1)
        try: log = open(LOG, errors="replace").read()
        except FileNotFoundError: continue
        m = re.search(r"^wall=(\d+),(\d+),(\d+),(\d+),(\d+) ([0-9a-f]{8})", log, re.M)
        if m: line = m; break
        if re.search(r"^wallerr=", log, re.M): break
    time.sleep(1.5)  # let the desktop repaint with the new source settle
    cmd({"execute": "pmemsave", "arguments": {"val": FB, "size": W * H * 4, "filename": RAW}})
    # QEMU can tear down the QMP socket the instant it processes quit,
    # before this side ever reads a reply -- a real race, not a bug in
    # the assertions above (which already ran); a reset here must not
    # mask a genuine PASS as a crash.
    try: cmd({"execute": "quit"})
    except (ConnectionResetError, BrokenPipeError, OSError): pass
finally:
    try: q.wait(timeout=5)
    except subprocess.TimeoutExpired: q.kill()

log = open(LOG, errors="replace").read()
geo = re.search(r"^geo=(.*)$", log, re.M)
print("kernel geo: ", geo.group(1).strip() if geo else "(none)")
if not line:
    err = re.search(r"^wallerr=.*$", log, re.M)
    print("FAIL: no wall= line on serial;", err.group(0) if err else "no wallerr= either (no network? fetch never ran?)")
    print(log[-800:]); sys.exit(1)
zoom, tx, ty, cx, cy = (int(v) for v in line.groups()[:5]); fnv_k = line.group(6)
print(f"kernel wall: z{zoom} tiles ({tx},{ty}) crop ({cx},{cy}) fnv={fnv_k}")
if (zoom, tx, ty) != (ZOOM, want_tx, want_ty):
    print(f"FAIL: kernel fetched tiles ({tx},{ty}) but the host's own location says ({want_tx},{want_ty})"); sys.exit(1)

# host-side reference: same twelve tiles, same crop
mosaic = Image.new("RGB", (COLS * TILE, ROWS * TILE))
for i in range(COLS * ROWS):
    col, row = i % COLS, i // COLS
    url = f"http://a.tile.opentopomap.org/{ZOOM}/{tx + col}/{ty + row}.png"
    png = subprocess.check_output(["curl", "-s", "--max-time", "15", url])
    im = Image.open(io.BytesIO(png)).convert("RGB")
    assert im.size == (TILE, TILE), (url, im.size)
    mosaic.paste(im, (col * TILE, row * TILE))
ref = mosaic.crop((cx, cy, cx + WALL_W, cy + WALL_H))
ref.save("/tmp/jt-wall-map.png")
data = ref.tobytes()
h = 0x811c9dc5
for b in data:
    h ^= b; h = (h * 0x01000193) & 0xffffffff
fnv_h = f"{h:08x}"
print(f"host fnv:    {fnv_h}")
if fnv_h != fnv_k:
    print("FAIL: kernel's composed wallpaper differs from the host's compose of the same tiles"); sys.exit(1)

# the screen itself: wind band must be the tinted map, not the photo
raw = open(RAW, "rb").read()
fb = Image.frombytes("RGBA", (W, H), raw, "raw", "BGRA").convert("RGB")
fb.save("/tmp/jt-wall-fb.png")
hour = time.localtime().tm_hour
if 8 <= hour < 18: night, day = 0, 10
elif 18 <= hour < 21: night, day = (hour - 18) * 22, 0
elif 5 <= hour < 8: night, day = (8 - hour) * 20, 0
else: night, day = 65, 0
def tint(c):
    if night: return tuple((v * (100 - night) + t * night) // 100 for v, t in zip(c, (0x20, 0x10, 0x09)))
    if day:   return tuple((v * (100 - day) + 0xDD * day) // 100 for v in c)
    return c
def mean(im):
    b = im.tobytes(); cnt = len(b) // 3
    return tuple(sum(b[i::3]) / cnt for i in range(3))
sc = 2
band_fb = fb.crop((0, WIND_TOP * sc, W, WIND_HORIZON * sc))
# the kernel maps logical rows MENUBAR_H..540 onto the whole 540-row source, so source rows for the band are:
area_h = 540 - MENUBAR_H
src_y0 = (WIND_TOP - MENUBAR_H) * WALL_H // area_h; src_y1 = (WIND_HORIZON - MENUBAR_H) * WALL_H // area_h
m_map = tint(mean(ref.crop((0, src_y0, WALL_W, src_y1))))
hdr = open("drivers/wallpaper.h").read()
body = hdr[hdr.index("{") + 1:hdr.rindex("}")]
photo = Image.frombytes("RGB", (WALL_W, WALL_H), bytes(int(t) for t in body.replace("\n", "").split(",") if t.strip()))
m_photo = tint(mean(photo.crop((0, src_y0, WALL_W, src_y1))))
m_fb = mean(band_fb)
d_map = max(abs(a - b) for a, b in zip(m_fb, m_map)); d_photo = max(abs(a - b) for a, b in zip(m_fb, m_photo))
print(f"screen band mean {tuple(round(v) for v in m_fb)}  tinted map {tuple(round(v) for v in m_map)} (d={d_map:.1f})  tinted photo {tuple(round(v) for v in m_photo)} (d={d_photo:.1f})  hour={hour}")
if d_map > 12 or d_photo < 25:
    print("FAIL: the framebuffer's wind band is not the tinted map (still the photo, or the tint/compose is off)"); sys.exit(1)
print(f"PASS: real map of the real location ({lat},{lon}) fetched over plain HTTP, decoded in-kernel byte-for-byte equal to the host's decode, and on screen with the hour's tint")
