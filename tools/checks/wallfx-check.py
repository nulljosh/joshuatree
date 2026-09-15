#!/usr/bin/env python3
"""v75 (0.67.0): proves the v65 weather particle overlay still composites
over a FETCHED map wallpaper, not just the baked photo, headlessly.

Boots with QEMU user networking, escapes gui_run into the shell, forces
rain through the `weatherfx rain` knob, pulls the real map with
`wallpaper fetch` (waits for its `wall=` serial line), re-enters the
desktop with `gui`, lets the wind tick run (which is where particles are
drawn, with zero sway on a map), then pmemsaves the framebuffer and asserts:

  1. the wind band's mean color is the map, not the photo (same test as
     wallpaper-check.py, so the map really is what's under the particles);
  2. at least 40 pixels in the band carry the rain streak's exact color
     (0xC9C0B4, drawn raw by weather_fx_plot, never tinted), a color the
     topo tiles don't naturally produce in quantity. Discriminating: the
     same run with `weatherfx off` counts ~0 (checked in the v75 pass).

Writes /tmp/jt-wallfx-fb.png. Needs internet, python3-pil, curl.
Usage: tools/checks/wallfx-check.py   (from the repo root)
"""
import json, os, re, socket, subprocess, sys, time
from PIL import Image

LOG, RAW = "/tmp/jt-wallfx-serial.log", "/tmp/jt-wallfx-fb.raw"
FB, W, H, PORT = 0xfd000000, 1920, 1080, 4452
WIND_TOP, WIND_HORIZON, SC = 30, 395, 2
RAIN = (0xC9, 0xC0, 0xB4)
MODE = sys.argv[1] if len(sys.argv) > 1 else "rain"

os.chdir(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
subprocess.check_call(["make", "-s", "kernel.elf"])
for f in (LOG, RAW):
    try: os.remove(f)
    except FileNotFoundError: pass

q = subprocess.Popen(["qemu-system-i386", "-kernel", "kernel.elf", "-display", "none", "-vga", "std",
                      "-rtc", "base=localtime", "-net", "nic,model=rtl8139", "-net", "user",
                      "-qmp", f"tcp:127.0.0.1:{PORT},server,nowait", "-serial", "file:" + LOG],
                     stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
got = None
try:
    time.sleep(1.0)
    s = socket.create_connection(("127.0.0.1", PORT)); f = s.makefile("rw")
    def cmd(o):
        f.write(json.dumps(o) + "\n"); f.flush()
        while True:
            r = json.loads(f.readline())
            if "return" in r or "error" in r: return r
    f.readline(); cmd({"execute": "qmp_capabilities"})
    def key(k): cmd({"execute": "send-key", "arguments": {"keys": [{"type": "qcode", "data": k}]}}); time.sleep(0.06)
    def type_line(t):
        for c in t: key("spc" if c == " " else c)
        key("ret"); time.sleep(0.3)
    time.sleep(4.0)                      # desktop up (the first weather cycle runs here too)
    key("esc"); time.sleep(1.0)          # into the shell
    type_line("weatherfx " + MODE)
    type_line("wallpaper fetch")
    for _ in range(60):
        time.sleep(1)
        log = open(LOG, errors="replace").read()
        if re.search(r"^wall=", log, re.M) or re.search(r"^wallerr=", log, re.M): break
    type_line("gui"); time.sleep(4.0)    # desktop again, wind tick drawing particles
    cmd({"execute": "pmemsave", "arguments": {"val": FB, "size": W * H * 4, "filename": RAW}})
    cmd({"execute": "quit"})
finally:
    try: q.wait(timeout=5)
    except subprocess.TimeoutExpired: q.kill()

log = open(LOG, errors="replace").read()
m = re.search(r"^wall=(\S+) ([0-9a-f]{8})", log, re.M)
if not m:
    e = re.search(r"^wallerr=.*$", log, re.M)
    print("FAIL: wallpaper fetch never landed;", e.group(0) if e else "no wallerr= either"); print(log[-600:]); sys.exit(1)
fb = Image.frombytes("RGBA", (W, H), open(RAW, "rb").read(), "raw", "BGRA").convert("RGB")
fb.save("/tmp/jt-wallfx-fb.png")
band = fb.crop((0, WIND_TOP * SC, W, WIND_HORIZON * SC))
b = band.tobytes(); n = len(b) // 3
mean = tuple(sum(b[i::3]) / n for i in range(3))
rain = sum(1 for i in range(0, len(b), 3) if b[i] == RAIN[0] and b[i + 1] == RAIN[1] and b[i + 2] == RAIN[2])
print(f"kernel wall: {m.group(1)} fnv={m.group(2)}  band mean {tuple(round(v) for v in mean)}  rain-colored px: {rain}  mode={MODE}")
# the photo, tinted for this hour exactly as kernel.c's daynight_calc would, is the "not the map" reference
hour = time.localtime().tm_hour
if 8 <= hour < 18: night, day = 0, 10
elif 18 <= hour < 21: night, day = (hour - 18) * 22, 0
elif 5 <= hour < 8: night, day = (8 - hour) * 20, 0
else: night, day = 65, 0
def tint(c):
    if night: return tuple((v * (100 - night) + t * night) // 100 for v, t in zip(c, (0x20, 0x10, 0x09)))
    if day:   return tuple((v * (100 - day) + 0xDD * day) // 100 for v in c)
    return c
hdr = open("drivers/wallpaper.h").read(); body = hdr[hdr.index("{") + 1:hdr.rindex("}")]
photo = Image.frombytes("RGB", (960, 540), bytes(int(t) for t in body.replace("\n", "").split(",") if t.strip()))
src_y0 = (WIND_TOP - 22) * 540 // 518; src_y1 = (WIND_HORIZON - 22) * 540 // 518
pb = photo.crop((0, src_y0, 960, src_y1)).tobytes(); pn = len(pb) // 3
m_photo = tint(tuple(sum(pb[i::3]) / pn for i in range(3)))
print(f"tinted photo band mean would be {tuple(round(v) for v in m_photo)} (hour {hour})")
if max(abs(a - b) for a, b in zip(mean, m_photo)) < 25:
    print("FAIL: band is the photo, not the fetched map"); sys.exit(1)
if MODE == "rain" and rain < 40:
    print("FAIL: no rain streaks over the map"); sys.exit(1)
print("PASS: particles composite over the fetched map" if MODE == "rain" else f"note: mode {MODE}, {rain} rain-colored px (expect ~0 for off)")
