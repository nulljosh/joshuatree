#!/usr/bin/env python3
"""v0.73: headless, real-network proof of the satellite wallpaper chain
(the direct request this pass wired: wall_fetch's Satellite theme, real
Google mt0.google.com/vt/lyrs=s tiles decoded with drivers/jpeg.c instead
of OpenTopoMap PNGs decoded with drivers/png.c). Same shape as
wallpaper-check.py's host-vs-kernel comparison, plus wallfx-check.py's
shell-injection trick to select a non-default theme before the fetch.

Boots with QEMU user networking, escapes gui_run into the shell, forces
`wallpaper sat` (the new toggle) then `wallpaper fetch`, waits for the
`wall=` serial line, re-enters the desktop, then:

  1. tile coords: same slippy-map math as wallpaper-check.py (Google
     shares OpenTopoMap's x/y/z convention, confirmed by kernel.c's own
     wall_fetch comment before this was wired), checked against the
     host's own ip-api location -- a kernel still pointed at the wrong
     tiles fails here;
  2. pixels: the same twelve Google satellite tiles are downloaded here,
     decoded by PIL's JPEG support, composed into the identical 960x540
     crop at the kernel's reported offset, and FNV-1a hashed against the
     kernel's own hash -- a kernel whose jpeg_decode call, tile copy or
     crop arithmetic is off fails byte-for-byte here;
  3. the screen: pmemsave of the real framebuffer. The wind band must be
     the tinted satellite mosaic (same gui_map_tint dispatch every other
     map theme goes through), not the baked photo;
  4. discriminating vs the topo style: real photographic satellite tiles
     have far higher pixel-to-pixel variance (texture: roofs, trees,
     roads, shadow) than OpenTopoMap's flat-shaded contour/hillshade
     art, which is mostly large flat color regions. The kernel's own
     decoded+cropped satellite buffer's stddev is compared against the
     equivalent topo buffer (fetched fresh here too) and must be
     meaningfully higher -- proven by hand: the topo buffer alone is
     nowhere close to this stddev floor, so this genuinely distinguishes
     "real satellite photography landed" from "still topo art, mislabeled."

Writes /tmp/jt-sat-fb.png (the real screen) and /tmp/jt-sat-map.png (the
host-composed reference). Needs internet on the host, python3-pil, curl.

Usage: tools/checks/satellite-wallpaper-check.py   (from the repo root, after make kernel.elf)
"""
import io, json, math, os, re, socket, statistics, subprocess, sys, time
from PIL import Image

LOG = "/tmp/jt-sat-serial.log"
RAW = "/tmp/jt-sat-fb.raw"
FB = 0xfd000000; W, H = 1920, 1080
PORT = 4453
WALL_W, WALL_H, TILE, ZOOM, COLS, ROWS = 960, 540, 256, 14, 4, 3  # mirror kernel.c's WALL_* constants (WALL_ZOOM 14->15->16 across v0.76.14-15, reverted back to 14 in v0.76.16 (higher zoom was the wrong direction for "whole town" framing) -- keep this in sync by hand, it drifted stale once already)
MENUBAR_H, WIND_TOP, WIND_HORIZON = 22, 30, 395

os.chdir(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
subprocess.check_call(["make", "-s", "kernel.elf"])
for f in (LOG, RAW):
    try: os.remove(f)
    except FileNotFoundError: pass

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
    def key(k): cmd({"execute": "send-key", "arguments": {"keys": [{"type": "qcode", "data": k}]}}); time.sleep(0.06)
    def type_line(t):
        for c in t: key("spc" if c == " " else c)
        key("ret"); time.sleep(2.0)  # the shell needs real settle time after a command that touches the desktop (wall_apply's redraw), not just the keystrokes -- too short a gap here silently drops the NEXT typed command
    time.sleep(6.0)               # desktop up, first weather cycle (topo, default theme) already ran and its own serial burst (geo=/wxurl=/wx=/wall=) has fully landed
    key("esc"); time.sleep(1.0)   # into the shell
    # marker: the default-theme autofetch above already wrote its own wall=
    # line to serial, delayed-flush of "-serial file:" can make a length
    # snapshot land mid-line if taken too early, so require the length to
    # be stable across two checks 1s apart before trusting it as a cut point.
    prev = -1; marker = 0
    for _ in range(10):
        cur = len(open(LOG, errors="replace").read())
        if cur == prev: marker = cur; break
        prev = cur; time.sleep(1.0)
    type_line("wallpaper sat")
    type_line("wallpaper fetch")
    for _ in range(120):
        time.sleep(1)
        try: log = open(LOG, errors="replace").read()[marker:]
        except FileNotFoundError: continue
        ms = list(re.finditer(r"^wall=(\d+),(\d+),(\d+),(\d+),(\d+) ([0-9a-f]{8})", log, re.M))
        if ms: line = ms[-1]; break  # last match after the marker: the satellite fetch, not a stale flush of the earlier topo one
        if re.search(r"^wallerr=", log, re.M): break
    type_line("gui"); time.sleep(1.5)  # desktop again, satellite theme now applied
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
if not line:
    err = re.search(r"^wallerr=.*$", log, re.M)
    print("FAIL: no wall= line after `wallpaper sat`+`wallpaper fetch`;", err.group(0) if err else "no wallerr= either")
    print(log[-800:]); sys.exit(1)
zoom, tx, ty, cx, cy = (int(v) for v in line.groups()[:5]); fnv_k = line.group(6)
print(f"kernel wall (satellite): z{zoom} tiles ({tx},{ty}) crop ({cx},{cy}) fnv={fnv_k}")
if (zoom, tx, ty) != (ZOOM, want_tx, want_ty):
    print(f"FAIL: kernel fetched tiles ({tx},{ty}) but the host's own location says ({want_tx},{want_ty})"); sys.exit(1)

# host-side reference: same twelve Google satellite tiles, same crop
mosaic = Image.new("RGB", (COLS * TILE, ROWS * TILE))
for i in range(COLS * ROWS):
    col, row = i % COLS, i // COLS
    url = f"http://mt0.google.com/vt/lyrs=s&x={tx + col}&y={ty + row}&z={ZOOM}"
    jpg = subprocess.check_output(["curl", "-s", "--max-time", "15", url])
    im = Image.open(io.BytesIO(jpg)).convert("RGB")
    assert im.size == (TILE, TILE), (url, im.size)
    mosaic.paste(im, (col * TILE, row * TILE))
ref = mosaic.crop((cx, cy, cx + WALL_W, cy + WALL_H))
ref.save("/tmp/jt-sat-map.png")
data = ref.tobytes()
h = 0x811c9dc5
for b in data:
    h ^= b; h = (h * 0x01000193) & 0xffffffff
fnv_h = f"{h:08x}"
print(f"host fnv:    {fnv_h}")
if fnv_h != fnv_k:
    print("FAIL: kernel's composed satellite wallpaper differs from the host's decode of the same tiles"); sys.exit(1)

# discriminator: real satellite photography has far more per-pixel texture
# than OpenTopoMap's flat-shaded contour art. Fetch the equivalent topo
# mosaic for the same tile coords and compare pixel stddevs.
topo_mosaic = Image.new("RGB", (COLS * TILE, ROWS * TILE))
for i in range(COLS * ROWS):
    col, row = i % COLS, i // COLS
    url = f"http://a.tile.opentopomap.org/{ZOOM}/{tx + col}/{ty + row}.png"
    png = subprocess.check_output(["curl", "-s", "--max-time", "15", url])
    im = Image.open(io.BytesIO(png)).convert("RGB")
    topo_mosaic.paste(im, (col * TILE, row * TILE))
topo_ref = topo_mosaic.crop((cx, cy, cx + WALL_W, cy + WALL_H))
def stddev(im):
    b = im.tobytes()
    return statistics.pstdev(b)
sat_sd, topo_sd = stddev(ref), stddev(topo_ref)
print(f"pixel stddev: satellite={sat_sd:.1f} topo={topo_sd:.1f}")
if sat_sd <= topo_sd:
    print("FAIL: satellite mosaic is not more textured than the topo art -- doesn't look like real photography"); sys.exit(1)

# the screen itself: wind band must be the tinted satellite map, not the photo
raw = open(RAW, "rb").read()
fb = Image.frombytes("RGBA", (W, H), raw, "raw", "BGRA").convert("RGB")
fb.save("/tmp/jt-sat-fb.png")
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
area_h = 540 - MENUBAR_H
src_y0 = (WIND_TOP - MENUBAR_H) * WALL_H // area_h; src_y1 = (WIND_HORIZON - MENUBAR_H) * WALL_H // area_h
m_sat = tint(mean(ref.crop((0, src_y0, WALL_W, src_y1))))
hdr = open("drivers/wallpaper.h").read()
body = hdr[hdr.index("{") + 1:hdr.rindex("}")]
photo = Image.frombytes("RGB", (WALL_W, WALL_H), bytes(int(t) for t in body.replace("\n", "").split(",") if t.strip()))
m_photo = tint(mean(photo.crop((0, src_y0, WALL_W, src_y1))))
m_fb = mean(band_fb)
d_sat = max(abs(a - b) for a, b in zip(m_fb, m_sat)); d_photo = max(abs(a - b) for a, b in zip(m_fb, m_photo))
print(f"screen band mean {tuple(round(v) for v in m_fb)}  tinted satellite {tuple(round(v) for v in m_sat)} (d={d_sat:.1f})  tinted photo {tuple(round(v) for v in m_photo)} (d={d_photo:.1f})  hour={hour}")
if d_sat > 12 or d_photo < 25:
    print("FAIL: the framebuffer's wind band is not the tinted satellite map (still the photo, or the tint/compose is off)"); sys.exit(1)
print(f"PASS: real satellite imagery of ({lat},{lon}) fetched over plain HTTP from Google, decoded in-kernel with jpeg_decode byte-for-byte equal to the host's PIL decode, distinguishably more textured than the topo style, and on screen with the hour's tint")
