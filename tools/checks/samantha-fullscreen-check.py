#!/usr/bin/env python3
"""Samantha full screen, headless and pixel based (2.9.0, mouth and blink polished in 2.9.1, room around her in 2.12.1): she is the whole screen, the
conversation is glass and captions over her picture, the two old typing bugs stay fixed, and her mouth follows her voice.

Everything runs with -display none and every host stubbed on loopback (facehost=, llmhost= and wxhost= point at one
stub through QEMU's user NAT at 10.0.2.2; the live site is never touched). The stub's portrait (/face/hd.jpg) is
synthetic so every pixel can be read without guessing: left half red, right half blue, wide dark stripes (hard detail
a blur must remove), horizontal bars where her mouth sits, a green patch on each eye box, and a known wall in her top
corners (warm and light, a different colour each side) with hair-like rows beneath it. Set SAMANTHA_SHOTS=<dir> to serve her real portrait
instead and save PNGs of each state there; in that mode the checks about the synthetic portrait's colours (cover, glass) are skipped, set SYN=1 to keep them with the synthetic picture.

  desktop 1920x1080   click her dock icon, then drive her like a person (scenarios 1 to 7).
  res=1920x1080       the "samantha" boot flag, the room around her (scenario 1b) without the drive.
  res=1024x768        the "samantha" boot flag opens her first, on a small screen.
  res=2560x1440       a screen past the ring-3 buffer limit: she opens in an ordinary window (804x344 logical), captions included.
  blink               a machine booted without `noblink` watches her eyes for 16 s (see scenario_blink).
  phone               the 430x760 phone mode opens her; she fills what the phone leaves her.

Asserted on the real framebuffer, each with a deadline and a poll, never a fixed sleep:
  1 room      her window is the whole screen and a wide one gives her room (2.12.1): the portrait is 86 percent of the width,
              her eye row is 36 to 44 percent down, and the two bands either side are her own wall colour (the portrait's
              top corners, blended across): the four corners match it, warm, light, never black and never the desktop; each
              band column is the same top to bottom (no streaks from stretching the edge), the band steps no more than a
              level or two from column to column and joins the portrait smoothly; the portrait's outer tenth fades into it;
              the old menu bar and dock strip are all face; the mouth and eye boxes she reports sit on her mouth and eyes.
  2 glass     outside the input panel the picture has hard stripes; inside it the same columns are a smooth blur that is
              still red on the left and blue on the right, tinted lighter, with a smooth red-to-blue seam.
  3 captions  what you send appears over the picture (the backdrop darkens it), her answer follows, and the whole stack goes together
              once the talk has been quiet: her own ticks say the last line, the fade start (at least 400 ticks, 4 s, after the last
              line and her voice) and the end (60 ticks, 0.6 s, later). Bottom up (2.12.1): a new line rises into place over ~0.28 s,
              up to three lines stack above the input with the newest at the bottom and each older one dimmer, never a fourth box
              (the phone, where there is room, measured in a column band beside the text: scenario phone). In a wide window fewer
              than two rows fit under her lips and her chin travels down toward the panel, so the stack is one compact row above
              the input; with her speech silent and her mouth settled shut no caption may darken her lips, either eye or anything
              above that row (no caption over her forehead and hair) at 1920x1080, 1024x768 and in the ordinary window a 2560x1440
              screen gets (found by its green eye patch), where not even one row clears her lips and so none may be drawn. The fade
              and the degree sign are read in that row.
  4 history   Tab opens a glass scrollback with the whole conversation in it; Tab closes it.
  5 bugs      the first typed letter: what she was handed (her own samtyped= serial line, not the stub) is the whole
              word, also for a word typed while she speaks. The degree sign: the weather answer draws a real degree
              ring in its caption, not the "?" the old 0xF8 byte turned into.
  6 mouth     the stub plays loud, silent and quiet speech; her mouth opening (samface: open=) tracks it and the pixels
              in the mouth rectangle follow that opening, silent speech leaves them exactly as the closed portrait. 2.9.1 draws the opening along her real lip seam with soft
              edges, so the pixel change is measured over the same box as before and the check is unchanged.
  7 exit      Esc closes her cleanly and the desktop (menu bar, dock) is alive; the red dot closes her too.

Discriminating: fill the bands by stretching the portrait's edge column (the streaks) or put the portrait back at 100 percent
of the width and (1) fails by name; put the 0xF8 degree back in the sysinfo copy and (5) fails by name; make the key handler eat the key that
stops speech again and (5) fails by name; draw the panel without the blur and (2) fails by name; stop the fade and (3)
fails by name; feed the mouth a constant level and (6) fails by name; make the blink draw nothing or never end and the blink
scenario fails by name; draw a caption at the top of the picture, over her lips or her eyes, or a fourth box, or fade the stack line by line, and (3) fails by name at the size where it lands. Every scenario but the blink one boots with `noblink`, because they compare exact pixels (the
closed portrait, the caption backdrop, the scrollback) and a blink is a real change those would count as a fault.

Usage: tools/checks/samantha-fullscreen-check.py   (repo root)
"""
import http.server, io, json, math, os, re, socket, subprocess, sys, tempfile, threading, time
from PIL import Image
from freeport import free_port

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
os.chdir(ROOT)
subprocess.run(["make", "-s", "kernel.elf"], check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
SHOTS = os.environ.get("SAMANTHA_SHOTS")   # a directory: serve her real portrait and save PNGs of each state
if SHOTS: os.makedirs(SHOTS, exist_ok=True)
FB = 0xfd000000
SLOT_X, SLOT_PITCH, ICON_ROW_Y, DOCK_ICON, SAMANTHA_SLOT, WEATHER_SLOT = 247, 43, 487, 37, 7, 8   # dock slot 0, pitch, logical (menubar-persist-check.py)
DOCK_COLOUR = (0xEF, 0xEB, 0xE4)
VM_MARGIN, VM_PH, PANEL_MAX = 16, 46, 640   # user/samantha.c
QMP = free_port()
REPLY = "Hi, lovely to see you."
picks = []
fails = []


def pcm(segments):
    """8-bit unsigned PCM at 16 kHz from (seconds, amplitude) segments: a 300 Hz tone, or silence at amplitude 0."""
    out = bytearray()
    for secs, amp in segments:
        for i in range(int(16000 * secs)):
            out.append(int(128 + amp * math.sin(2 * math.pi * 300 * i / 16000)) & 255 if amp else 128)
    return bytes(out)


TONE = pcm([(3.0, 70)])
MOUTH_PCM = pcm([(1.0, 100), (1.0, 0), (1.0, 14), (1.0, 100)])   # loud, silent, quiet, loud: 4 s, the call's 64 KB
speech = {"pcm": TONE}
delay = {"chat": 0.0}   # seconds the stub waits before it answers /api/chat (the stack check spaces her replies out)
WX = (b'{"latitude":49.09,"longitude":-122.57,"timezone":"America/Vancouver",'
      b'"current_units":{"time":"iso8601","interval":"seconds","temperature_2m":"\xc2\xb0C","apparent_temperature":"\xc2\xb0C","relative_humidity_2m":"%","wind_speed_10m":"km/h","weather_code":"wmo code"},'
      b'"current":{"time":"2026-09-20T16:15","interval":900,"temperature_2m":14.2,"apparent_temperature":12.8,"relative_humidity_2m":69,"wind_speed_10m":11.4,"weather_code":3},'
      b'"daily_units":{"time":"iso8601","weather_code":"wmo code","temperature_2m_max":"\xc2\xb0C","temperature_2m_min":"\xc2\xb0C"},'
      b'"daily":{"time":["2026-09-20","2026-09-21","2026-09-22","2026-09-23","2026-09-24"],"weather_code":[3,0,61,71,95],'
      b'"temperature_2m_max":[17.6,21.2,15.4,3.1,16.5],"temperature_2m_min":[9.4,8.5,9.2,-2.6,9.0]}}')


WALL_L, WALL_R = (232, 220, 198), (212, 198, 172)   # her wall, warm and light, a different colour each side
EDGE_W, WALL_ROWS = 40, 192                        # source px of wall or hair-like edge strip each side; wall rows above, hair rows below
GREEN = (40, 150, 70)
EYE_BOXES = [(314, 270, 27, 12), (468, 279, 32, 12)]   # source px: centre x, centre y, half width, half height of both eye boxes (427/367/37 and 636/379/43, 16 thousandths of 736)


def portrait():
    im = Image.new("RGB", (736, 736))
    px = im.load()
    for y in range(736):
        for x in range(736):
            base = (205, 55, 55) if x < 368 else (55, 55, 205)
            if (x // 8) % 2: base = tuple(int(c * 0.35) for c in base)
            if 400 <= y <= 470 and 290 <= x <= 480:   # her mouth: horizontal bars the warp has to move
                base = (235, 220, 190) if ((y - 400) // 6) % 2 == 0 else (150, 60, 50)
            for cx, cy, hw, hh in EYE_BOXES:   # her eyes: a solid patch the geometry has to land on
                if abs(x - cx) <= hw and abs(y - cy) <= hh: base = GREEN
            if x < EDGE_W or x >= 736 - EDGE_W:   # the edge: wall up top, rows of hair-like colour below (an edge stretched sideways would streak)
                base = (WALL_L if x < 368 else WALL_R) if y < WALL_ROWS else (150 + (y * 37) % 80, 90 + (y * 53) % 50, 40 + (y * 29) % 40)
            px[x, y] = base
    b = io.BytesIO(); im.save(b, "JPEG", quality=70, subsampling=2); return b.getvalue()


SYN = portrait()
assert len(SYN) < 60000, len(SYN)
SERVED = open(os.path.join(ROOT, "landing/face/hd.jpg"), "rb").read() if SHOTS and not os.environ.get("SYN") else SYN   # what /face/hd.jpg answers


def expected_walls(jpeg):
    """Her wall either side, worked out here from the picture she is served by the same rule the spec gives (the average of
    the top corners: the outer 4 percent across, 2 to 25 percent down), not read back from her."""
    im = Image.open(io.BytesIO(jpeg)).convert("RGB"); n = im.size[0]; px = im.load()
    xs, ya, yb = n * 4 // 100, n * 2 // 100, n * 25 // 100
    def avg(x0, x1):
        t = [0, 0, 0]; k = 0
        for y in range(ya, yb):
            for x in range(x0, x1):
                for i in range(3): t[i] += px[x, y][i]
                k += 1
        return tuple(round(v / k) for v in t)
    return avg(0, xs), avg(n - xs, n)


WALLS = expected_walls(SERVED)


def wall_at(x, PW):
    """The wall colour across the window at physical column x: the left wall at 0, the right wall at the last column, blended straight across."""
    L, R = WALLS
    return tuple(L[i] + (R[i] - L[i]) * x / (PW - 1) for i in range(3))


class Stub(http.server.BaseHTTPRequestHandler):
    def log_message(self, *a): pass

    def reply(self, code, body, ctype):
        self.send_response(code); self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body))); self.end_headers(); self.wfile.write(body)

    def do_GET(self):
        p = self.path
        if p.startswith("/api/speak"): return self.reply(200, speech["pcm"], "application/octet-stream")
        if p == "/face/hd.jpg":
            if SHOTS and not os.environ.get("SYN"): return self.reply(200, open(os.path.join(ROOT, "landing/face/hd.jpg"), "rb").read(), "image/jpeg")
            return self.reply(200, SYN, "image/jpeg")
        if p.startswith("/face/"): return self.reply(200, b"", "image/jpeg")   # no frame set: the portrait is the face
        if p.startswith("/json/"):
            return self.reply(200, json.dumps({"status": "success", "lat": 49.1, "lon": -122.6, "city": "Langley"}).encode(), "application/json")
        return self.reply(200, WX, "application/json")   # the forecast, in the exact shape the desktop's parser reads

    def do_POST(self):
        body = self.rfile.read(int(self.headers.get("Content-Length", "0")))
        if self.path == "/api/pick":
            q = json.loads(body).get("q", ""); picks.append(q)
            return self.reply(200, json.dumps({"tool": "weather", "arg": ""} if "weather" in q else {}).encode(), "application/json")
        if delay["chat"]: time.sleep(delay["chat"])
        return self.reply(200, json.dumps({"model": "samantha", "message": {"role": "assistant", "content": REPLY}, "done": True}).encode(), "application/json")


srv = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Stub); srv.daemon_threads = True
threading.Thread(target=srv.serve_forever, daemon=True).start()
port = srv.server_address[1]


def lum(p): return (p[0] * 299 + p[1] * 587 + p[2] * 114) // 1000


def stats(img, x0, x1, y0, y1):
    """(mean r, g, b, mean over rows of the luminance's std along the row) over a physical rect."""
    px = img.load(); n = 0; sr = sg = sb = 0; devs = []
    for y in range(y0, y1):
        ls = []
        for x in range(x0, x1):
            p = px[x, y]; sr += p[0]; sg += p[1]; sb += p[2]; n += 1; ls.append(lum(p))
        m = sum(ls) / len(ls); devs.append(math.sqrt(sum((v - m) ** 2 for v in ls) / len(ls)))
    return sr / n, sg / n, sb / n, sum(devs) / len(devs)


def darkened(img, base, box):
    """The caption backdrop's strength: the mean amount by which pixels in the box are darker than the same pixel of
    the baseline picture. Pixels the white text brightens count as zero, so the text cannot cancel the backdrop. The
    picture is static, so with no caption this is exactly 0."""
    x0, y0, x1, y1 = box
    a = img.load(); b = base.load(); n = 0; tot = 0
    for y in range(y0, y1, 2):
        for x in range(x0, x1, 2):
            d = lum(b[x, y]) - lum(a[x, y])
            if d > 0: tot += d
            n += 1
    return tot / n


def mean_lum(img, box):
    x0, y0, x1, y1 = box
    px = img.load(); s = 0; n = 0
    for y in range(y0, y1, 2):
        for x in range(x0, x1, 2): s += lum(px[x, y]); n += 1
    return s / n


class Machine:
    def __init__(self, tag, res, append, scale):
        self.tag, self.PW, self.PH, self.scale = tag, res[0], res[1], scale
        self.work = tempfile.mkdtemp(prefix="jt-samfs-")
        self.log, self.dump = os.path.join(self.work, "serial.txt"), os.path.join(self.work, "fb.raw")
        self.q = subprocess.Popen(["qemu-system-i386", "-kernel", "kernel.elf", "-display", "none", "-vga", "std", "-no-reboot",
                                   "-qmp", f"tcp:127.0.0.1:{QMP},server,nowait", "-serial", "file:" + self.log,
                                   "-net", "nic,model=rtl8139", "-net", "user",
                                   "-audiodev", f"wav,id=snd,path={os.path.join(self.work, 'out.wav')}", "-device", "sb16,audiodev=snd",
                                   "-append", append], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        self.s = None
        for _ in range(50):
            time.sleep(0.2)
            try: self.s = socket.create_connection(("127.0.0.1", QMP)); break
            except OSError: pass
        if self.s is None: raise RuntimeError("no QMP")
        self.f = self.s.makefile("rw"); self.f.readline(); self.cmd({"execute": "qmp_capabilities"}); CURRENT.append(self)

    def cmd(self, o):
        self.f.write(json.dumps(o) + "\n"); self.f.flush()
        while True:
            r = json.loads(self.f.readline())
            if "return" in r or "error" in r: return r

    def close(self):
        try: self.cmd({"execute": "quit"})
        except Exception: pass
        try: self.q.wait(timeout=5)
        except subprocess.TimeoutExpired: self.q.kill()

    def frame(self, rows=None):
        """The framebuffer as an RGB image; rows=(y0,y1) reads only those physical rows (the rest is black)."""
        y0, y1 = rows if rows else (0, self.PH)
        self.cmd({"execute": "pmemsave", "arguments": {"val": FB + y0 * self.PW * 4, "size": (y1 - y0) * self.PW * 4, "filename": self.dump}})
        part = Image.frombytes("RGBA", (self.PW, y1 - y0), open(self.dump, "rb").read(), "raw", "BGRA").convert("RGB")
        if not rows: return part
        full = Image.new("RGB", (self.PW, self.PH)); full.paste(part, (0, y0)); return full

    def keys(self, *qc): self.cmd({"execute": "send-key", "arguments": {"keys": [{"type": "qcode", "data": k} for k in qc]}})

    def typ(self, word):
        for ch in word: self.keys("spc" if ch == " " else ch)

    def serial(self):
        try: return open(self.log, encoding="latin-1").read()
        except FileNotFoundError: return ""

    def wait(self, pred, secs, step=0.1):
        t_end = time.time() + secs
        while time.time() < t_end:
            if pred(): return True
            time.sleep(step)
        return False

    def still(self, rows=None, need=4, secs=90, ok=None):
        """Wait until the screen has stopped changing: `need` frames in a row (0.25 s apart) are byte for byte the same, and
        `ok(frame)`, when given, holds (so a black or half-composited window does not count as still). Returns that frame, or
        None at the deadline. The checks sample only after this, never after a fixed sleep: a shared CI runner can be many
        times slower than a laptop, and a fixed sleep either wastes time or samples a picture that is not painted yet."""
        t_end = time.time() + secs; last = None; run = 0
        while time.time() < t_end:
            img = self.frame(rows); raw = img.tobytes()
            if raw == last and (ok is None or ok(img)): run += 1
            else: run = 0
            last = raw
            if run >= need: return img
            time.sleep(0.25)
        return None

    def move(self, x, y):
        self.cmd({"execute": "input-send-event", "arguments": {"events": [
            {"type": "abs", "data": {"axis": "x", "value": int(x * 32768 / (self.PW // self.scale))}},
            {"type": "abs", "data": {"axis": "y", "value": int(y * 32768 / (self.PH // self.scale))}}]}})

    def click(self, x=None, y=None):
        if x is not None: self.move(x, y); time.sleep(0.15)
        for down in (True, False):
            self.cmd({"execute": "input-send-event", "arguments": {"events": [{"type": "btn", "data": {"down": down, "button": "left"}}]}}); time.sleep(0.08)

    def open_from_dock(self, slot=SAMANTHA_SLOT):
        self.move(SLOT_X + slot * SLOT_PITCH + DOCK_ICON // 2, ICON_ROW_Y); time.sleep(0.3); self.click()

    def shot(self, img, name):
        if SHOTS: img.save(os.path.join(SHOTS, name + ".png"))


CURRENT = []


SYNTH = not SHOTS or bool(os.environ.get("SYN"))   # screenshot mode serves the real portrait, where red-and-blue colour probes mean nothing


def fail_s(tag, msg):
    """A failure about the synthetic portrait's colours: skipped in screenshot mode (see the docstring)."""
    if SYNTH: fail(tag, msg)


def fail(tag, msg):
    fails.append(tag + msg)
    if CURRENT and os.environ.get("TAIL"): print("--- serial tail ---\n" + "\n".join(CURRENT[-1].serial().splitlines()[-int(os.environ.get("TAIL") or 25):]) + "\n---")


def last_open(m):
    v = re.findall(r"samface: open=(\d+)", m.serial())
    return int(v[-1]) if v else None


def history_state(m):
    v = re.findall(r"samface: history=(\d)", m.serial())
    return v[-1] if v else None


def captions_gone(m):
    v = re.findall(r"samface: captions=(\d)", m.serial())
    return bool(v) and v[-1] == "0" and last_open(m) in (0, None)


def panel_rect(m):
    LW, LH = m.PW // m.scale, m.PH // m.scale
    pw = min(LW - 2 * VM_MARGIN, PANEL_MAX); px = (LW - pw) // 2; py = LH - VM_MARGIN - VM_PH
    return px * m.scale, py * m.scale, (px + pw) * m.scale, (py + VM_PH) * m.scale


def win_dims(m):
    """(width, height, k) of her window in logical px as she reports it: an ordinary window on a screen too big for a full-screen one is not the
    screen, and k > 1 means she composes at 1/k of it and repeats each pixel (a 26 px caption row is then 26*k window px)."""
    v = re.findall(r"samface: win=(\d+),(\d+),(\d+)", m.serial())
    return (int(v[-1][0]), int(v[-1][1]), int(v[-1][2])) if v else (m.PW // m.scale, m.PH // m.scale, 1)


def win_h(m): return win_dims(m)[1]


def m_ybot(m): return win_h(m) - VM_MARGIN - VM_PH - 12
def m_ytop(m): return m_ybot(m) - 74


SILENT = pcm([(3.0, 0)])   # speech with no sound in it: her mouth stays shut, so only a caption can darken the picture
MSG = "hello there my friend how are you doing today"


def slot_geom(m):
    """(tight, fits, boxtop, capB): the caption layout of a wide window, worked out from the mouth she reports and her window's height by the same
    arithmetic as draw_video_k, in window px. tight: fewer than two rows fit under her lips, so the stack is capped at one compact row
    (26 px high in her composed pixels, its bottom 4 px above the glass panel at capB, its top at boxtop). fits: that one row clears her lower
    lip (a short ordinary window may have no room at all, and then no caption is drawn)."""
    sc = m.scale; k = win_dims(m)[2]
    mx, my, mhw, ml1 = [v // sc // k for v in serial_geom(m, "mouth", 4)]   # in her composed pixels, where she lays the captions out
    py = win_h(m) // k - VM_MARGIN - VM_PH
    tight = (py - 12 - (my + ml1 + 8) - 26) // 16 < 2
    capB = py - 4; boxtop = capB - 26
    return tight, boxtop - 4 > my + ml1, boxtop * k, capB * k


def face_zones(m, org=(0, 0)):
    """Physical rects no caption may darken in a wide window, where her mouth is low: her lips (from the top of the upper lip to the bottom of the
    closed lower one), both eyes, and everything above the one-row slot (so a reply at the top over her forehead would fail). org is the
    window's top left on the screen (0, 0 full screen)."""
    ox, oy = org; sc = m.scale
    mx, my, mhw, ml1 = serial_geom(m, "mouth", 4)
    ex0, ey0, ew0, ex1, ey1, ew1, eh = serial_geom(m, "eyes", 7)
    tight, fits, boxtop, capB = slot_geom(m)
    return {"lips": (ox + mx - mhw * 13 // 10, oy + my - mhw * 8 // 10, ox + mx + mhw * 13 // 10, oy + my + ml1),
            "left eye": (ox + ex0 - ew0, oy + ey0 - eh * 3 // 2, ox + ex0 + ew0, oy + ey0 + eh * 3 // 2),
            "right eye": (ox + ex1 - ew1, oy + ey1 - eh * 3 // 2, ox + ex1 + ew1, oy + ey1 + eh * 3 // 2),
            "everything above the slot": (ox + 40 * sc, oy + 50 * sc, ox + 2 * mx - 40 * sc, oy + (boxtop - 8) * sc)}


def slot_box(m, org=(0, 0)):
    """Physical rect of the one-row slot under her lips, 240 logical px across (the middle of the window: the mouth sits 0.018 of the portrait right of it)."""
    sc = m.scale; mx, my, mhw, ml1 = serial_geom(m, "mouth", 4)
    tight, fits, boxtop, capB = slot_geom(m)
    cx = org[0] + mx - mhw * 18 // 87
    return (cx - 120 * sc, org[1] + (boxtop - 4) * sc, cx + 120 * sc, org[1] + capB * sc)


class Shut:
    """Is her mouth settled shut? She prints open=0 as she draws the closing frame, a moment before it is on the screen, and before her
    voice reaches the card she shapes the sentence by its letters, so a poll counts only when 0 was reported before and after the frame was
    read and for 0.4 s already. Then the picture under her lips is the closed portrait exactly, and only a caption can darken it."""
    def __init__(self): self.since = None

    def poll(self, before, after):
        if before in (0, None) and after in (0, None):
            if self.since is None: self.since = time.time()
        else: self.since = None
        return self.since is not None and time.time() - self.since >= 0.4


def window_origin(m, img):
    """Where an ordinary window's picture starts on the screen (physical px), for a screen too big for a full-screen window: the
    test portrait's left eye is a solid green patch, so its top left on the screen less its top left in the window is the origin."""
    ex0, ey0, ew0, ex1, ey1, ew1, eh = serial_geom(m, "eyes", 7)
    px = img.load(); xs = []; ys = []
    for y in range(m.PH // 12, m.PH * 6 // 10, 2):
        for x in range(0, m.PW, 2):
            p = px[x, y]
            if p[1] > p[0] + 60 and p[1] > p[2] + 40: xs.append(x); ys.append(y)   # the patch's green, as geometry_check reads it
    if not xs: return None
    return min(xs) - (ex0 - ew0), min(ys) - (ey0 - eh)


def ticks(m, what):
    """Her own clock: every tick she reported for `samface: <what>=`."""
    return [int(v) for v in re.findall(r"samface: " + what + r"=(\d+)", m.serial())]


def caption_run(m, tag, base, org=(0, 0)):
    """Her reply and your message go up over a wide window with her mouth shut (silent speech). From the first frame to the last, nothing may
    darken her lips, either eye, or anything above the one-row slot under her lips (no caption over her forehead and hair); where the
    slot does not clear her lips (a short ordinary window) no caption may show at all; otherwise it shows in the slot. The picture is
    static and `base` is the idle picture, so with no caption every one of these reads exactly 0 (taken only while her mouth is settled
    shut: an opening mouth darkens the picture too)."""
    if org is None:   # screenshot runs with the real portrait: no test patches to find the window by, so nothing is measured, only photographed
        speech["pcm"] = SILENT
        try:
            n0 = m.serial().count("samface: captions=1"); m.typ(MSG); m.keys("ret")
            if not m.wait(lambda: m.serial().count("samface: captions=1") > n0, 120): return fail(tag, "no caption ever went up")
            t_end = time.time() + 60
            while time.time() < t_end:
                img = m.frame(); d = sum(1 for y in range(0, m.PH, 4) for x in range(0, m.PW, 4) if sum(base.getpixel((x, y))) - sum(img.getpixel((x, y))) > 90)
                if d > 300: m.shot(img, "14-caption-slot-" + tag.split()[0].replace("/", "-")); break
                time.sleep(0.05)
            m.wait(lambda: captions_gone(m), 120)
        finally:
            speech["pcm"] = TONE
        return
    tight, fits, boxtop, capB = slot_geom(m)
    zones = face_zones(m, org); slot = slot_box(m, org)
    worst = {k: 0.0 for k in zones}; seen = 0.0; shut = Shut(); polls = 0
    y0 = max(0, min([slot[1]] + [z[1] for z in zones.values()]) - 4); y1 = min(m.PH, max([slot[3]] + [z[3] for z in zones.values()]) + 14)
    speech["pcm"] = SILENT
    try:
        n0 = m.serial().count("samface: captions=1")
        m.typ(MSG); m.keys("ret")
        t_end = time.time() + 120; started = False; shot = False
        while time.time() < t_end:
            a = last_open(m); img = m.frame((y0, y1)); b = last_open(m)
            settled = shut.poll(a, b)
            v = darkened(img, base, slot)
            if settled:
                polls += 1; seen = max(seen, v)
                for k, z in zones.items(): worst[k] = max(worst[k], darkened(img, base, z))
            if SHOTS and not shot and v > 20: shot = True; m.shot(m.frame(), "14-caption-slot-" + tag.split()[0].replace("/", "-"))
            if m.serial().count("samface: captions=1") > n0: started = True
            if started and captions_gone(m): break
            time.sleep(0.03)
    finally:
        speech["pcm"] = TONE
    print(tag + f"captions (one compact row at the bottom, y {boxtop} to {capB}, {'clears' if fits else 'cannot clear'} her lips): darkest in the slot {seen:.1f}; on " + ", ".join(f"{k} {w:.2f}" for k, w in worst.items()) + f" ({polls} polls with her mouth settled shut)")
    if polls < 5: fail(tag, f"her mouth was never settled shut while the captions were up ({polls} polls), so nothing could be measured")
    if not tight: fail(tag, "this check expects a window with no room under her lips for two rows; the slot arithmetic says there is")
    if fits and seen <= 8: fail(tag, f"no caption showed in the one-row slot above the input (darkest {seen:.1f})")
    if not fits and seen > 0.5: fail(tag, f"a caption was drawn at {seen:.1f} levels although no row clears her lips")
    for k, w in worst.items():
        if w > 0.5: fail(tag, f"a caption landed on {k} (it was darkened by {w:.1f} levels; nothing may be drawn there)")


def wide_geom(m):
    """(S, band) in logical px for a wide window by the spec: the portrait is 86 percent of the width (never shorter than the window), centred."""
    LW, LH = m.PW // m.scale, m.PH // m.scale
    S = max(LW * 86 // 100, LH)
    return S, (LW - S) // 2


def serial_geom(m, what, n):
    v = re.findall(r"samface: " + what + "=" + ",".join([r"(\d+)"] * n), m.serial())
    return [int(x) * m.scale for x in v[-1]] if v else None


def geometry_check(m, tag, img, desktop=None):
    """(1) room: the picture is framed by the 2.12.1 spec, measured on the real framebuffer `img` of a settled wide window.
    `desktop` is the desktop's own picture before she opened, to prove the corners are not the wallpaper showing through."""
    PW, PH, sc = m.PW, m.PH, m.scale
    LW, LH = PW // sc, PH // sc
    S, band = wide_geom(m)
    bw = band * sc
    ok = True
    def bad(msg):
        nonlocal ok
        ok = False; fail(tag, msg)
    # the four corners are her wall colour: where the spec says it is, warm, light, never black, never the desktop
    for name, (x, y) in {"top-left": (6, 6), "top-right": (PW - 7, 6), "bottom-left": (6, PH - 7), "bottom-right": (PW - 7, PH - 7)}.items():
        p = img.getpixel((x, y)); w = wall_at(x, PW)
        if max(abs(p[i] - w[i]) for i in range(3)) > 10: bad(f"the {name} corner is {p}, not her wall colour {tuple(round(v) for v in w)}")
        if p == (0, 0, 0) or lum(p) < 150 or p[0] < p[2] + 10: bad(f"the {name} corner {p} is not warm and light like her wall")
        if desktop is not None and sum(abs(p[i] - desktop.getpixel((x, y))[i]) for i in range(3)) < 30: bad(f"the {name} corner {p} is the desktop wallpaper showing through")
    # the band: the same top to bottom, almost flat from column to column, exactly the blend of the two walls
    ptop = panel_rect(m)[1]   # the glass panel can cross a band on a narrow screen (1024 wide): it is judged by its own checks
    ys = [y for y in range(100, ptop - 6, 6)]
    # Her own two walls as drawn at the ends of the window; they match the walls worked out here from the picture to within the
    # difference between her JPEG decoder and PIL's, and everything between must be the straight blend of the two.
    fbl = tuple(sum(img.getpixel((0, y))[i] for y in ys) / len(ys) for i in range(3)); fbr = tuple(sum(img.getpixel((PW - 1, y))[i] for y in ys) / len(ys) for i in range(3))
    for name, got, want in (("left", fbl, WALLS[0]), ("right", fbr, WALLS[1])):
        if max(abs(got[i] - want[i]) for i in range(3)) > 8: bad(f"her {name} wall is {tuple(round(v) for v in got)} at the window's edge, not the portrait's top corner colour {want}")
    worst_col = worst_step = worst_wall = 0.0; prev = None
    for x in range(2, bw - 2):
        col = [img.getpixel((x, y)) for y in ys]; ls = [lum(p) for p in col]
        mean = sum(ls) / len(ls); sd = math.sqrt(sum((v - mean) ** 2 for v in ls) / len(ls))
        worst_col = max(worst_col, sd)
        if prev is not None: worst_step = max(worst_step, abs(mean - prev))
        prev = mean
        w = tuple(fbl[i] + (fbr[i] - fbl[i]) * x / (PW - 1) for i in range(3))
        worst_wall = max(worst_wall, max(abs(sum(p[i] for p in col) / len(col) - w[i]) for i in range(3)))
    print(tag + f"side band {bw} px wide: most a column varies top to bottom {worst_col:.2f}, biggest step between columns {worst_step:.2f}, furthest from the walls' blend {worst_wall:.1f}")
    if worst_col > 1.5: bad(f"the side band has streaks: a column varies {worst_col:.1f} levels top to bottom (an edge column stretched sideways, or the portrait showing in it)")
    if worst_step > 2: bad(f"the side band is not smooth from column to column (steps of {worst_step:.1f})")
    if worst_wall > 2.5: bad(f"the side band is not her left wall blended straight across to her right wall ({worst_wall:.1f} levels off)")
    # the portrait joins it smoothly: no step between neighbouring columns over the band's edge and the first stretch of the portrait, in the rows below the wall
    jump = 0.0
    for y in range(int(PH * 0.45), min(int(PH * 0.85), ptop - 6), 6):
        for x in range(bw - 6, bw + EDGE_W * S // 736 * sc - 4):   # across the band's edge and the test portrait's hair strip, up to its stripes
            jump = max(jump, abs(lum(img.getpixel((x, y))) - lum(img.getpixel((x + 1, y)))))
    print(tag + f"biggest step between neighbouring columns where the band meets the portrait: {jump:.1f}")
    if jump > 6: bad(f"the portrait does not blend into the band: a step of {jump:.1f} levels between neighbouring columns at its edge")
    # the old menu bar and dock strip are all face (the picture is the whole screen), a probe clear of the red and blue seam
    for name, (x, y) in {"old menu bar": (PW // 2 + 120, 20), "old dock": (1200 * PW // 1920, min(935 * PH // 1080, ptop - 8))}.items():   # kept above the glass panel, a blur of the picture that says nothing about the desktop
        p = img.getpixel((x, y))
        if desktop is not None and p == desktop.getpixel((x, y)): bad(f"her picture does not cover the {name}: {p}")
        elif SYNTH and not p[2] > p[0] + 25: bad(f"her picture does not cover the {name}: {p}")
    # the portrait's outer tenth fades into the wall: the darkest stripe a little way in (a stretch where the weight is about
    # 0.4 to 0.7) is much lighter than the darkest stripe inside, because it is mixed with the light wall. Without the fade they are equal.
    if SYNTH:
        def darkest(x0, x1):
            return sum(min(lum(img.getpixel((x, y))) for x in range(x0, x1)) for y in range(120, 380, 10)) / len(range(120, 380, 10))
        f_ = S // 10   # the fade is a tenth of the portrait; inside it is full strength
        z0 = EDGE_W * S // 736 + 1; z1 = z0 + f_ // 4   # just past the test portrait's wall strip: the weight there is about 0.4 to 0.7
        el, er = bw, PW - bw   # physical x of the portrait's left and right edge
        inner_l = darkest(el + (f_ + 20) * sc, el + (f_ + 100) * sc); fade_l = darkest(el + z0 * sc, el + z1 * sc)
        inner_r = darkest(er - (f_ + 100) * sc, er - (f_ + 20) * sc); fade_r = darkest(er - z1 * sc, er - z0 * sc)
        print(tag + f"darkest stripe in the faded edge against inside the portrait: left {fade_l:.0f} vs {inner_l:.0f}, right {fade_r:.0f} vs {inner_r:.0f}")
        if fade_l - inner_l < 40 or fade_r - inner_r < 40: bad(f"the portrait's edges are not feathered into the wall (the stripes in the fade are only {fade_l - inner_l:.0f} and {fade_r - inner_r:.0f} levels lighter than inside, want 40)")
    # her eye row sits 36 to 44 percent down, and the eye and mouth boxes she reports are on her eyes and mouth
    eyes = serial_geom(m, "eyes", 7); mouth = serial_geom(m, "mouth", 4)
    if eyes is None or mouth is None: return bad("she never reported where her eyes and mouth are")
    ex0, ey0, ew0, ex1, ey1, ew1, eh = eyes
    row = (ey0 + ey1) / 2
    if SYNTH:
        def green(p): return p[1] > p[0] + 60 and p[1] > p[2] + 40
        spans = []
        for cx, cy, ew in ((ex0, ey0, ew0), (ex1, ey1, ew1)):
            rows = [y for y in range(max(0, cy - 4 * eh), min(PH, cy + 4 * eh)) if green(img.getpixel((cx, y)))]
            cols = [x for x in range(max(0, cx - 2 * ew), min(PW, cx + 2 * ew)) if green(img.getpixel((x, cy)))]
            if not rows or not cols: bad(f"the eye box she reports at ({cx},{cy}) has no eye in it"); continue
            spans.append(((min(rows) + max(rows)) / 2, (min(cols) + max(cols)) / 2, (max(cols) - min(cols)) / 2, (max(rows) - min(rows)) / 2))
            gy, gx, gw, gh = spans[-1]
            print(tag + f'eye patch: reported ({cx},{cy}) half {ew}x{eh}, on screen ({gx:.1f},{gy:.1f}) half {gw:.1f}x{gh:.1f}')
            if abs(gy - cy) > 5 or abs(gx - cx) > 5 or abs(gw - ew) > 5 or abs(gh - eh) > 5: bad(f"the eye box she reports (centre {cx},{cy}, half {ew}x{eh}) is not on the eye: it is at {gx:.0f},{gy:.0f}, half {gw:.0f}x{gh:.0f}")
        if len(spans) == 2: row = (spans[0][0] + spans[1][0]) / 2
        mx, my, mhw, ml1 = mouth
        def near(p, c): return all(abs(p[i] - c[i]) < 30 for i in range(3))
        top = [y for y in range(0, PH * 9 // 10) if near(img.getpixel((mx, y)), (235, 220, 190)) or near(img.getpixel((mx, y)), (150, 60, 50))]
        if not top: bad("the mouth bars of the test portrait are not under the mouth she reports")
        elif not (min(top) <= my <= max(top)): bad(f"the mouth she reports (seam at {my}) is not on her lips: the bars are rows {min(top)} to {max(top)}")
    pct = 100 * row / PH
    print(tag + f"eye row {row:.0f} of {PH} px: {pct:.1f} percent down")
    if not 36 <= pct <= 44: bad(f"her eye row is {pct:.1f} percent down the window, want 36 to 44")
    if ok: print(tag + "room around her: 86 percent wide, eyes 40 percent down, the bands are her wall, the edges feather into it")
    return ok


def scenario_desktop():
    tag = "1920x1080 desktop: "
    m = Machine(tag, (1920, 1080), f"noblink llmhost=10.0.2.2 llmport={port} facehost=10.0.2.2:{port} wxhost=10.0.2.2:{port}", 2)
    try:
        if not m.wait(lambda: m.frame((1020, 1030)).getpixel((961, 1023)) == DOCK_COLOUR, 60): return fail(tag, "the desktop dock never appeared")
        if m.still(need=3, ok=lambda im: im.getpixel((961, 1023)) == DOCK_COLOUR) is None: return fail(tag, "the desktop never stopped changing before she opened")
        before = m.frame(); m.shot(before, "desktop-before")
        if before.getpixel((1200, 935)) != DOCK_COLOUR: return fail(tag, "the dock probe pixel is not dock-coloured before she opens; the cover assertion would mean nothing")
        if not m.wait(lambda: re.search(r"\nwx=", m.serial()) is not None, 90): return fail(tag, "the desktop never fetched the stub weather (no wx= line)")
        m.open_from_dock()
        if not m.wait(lambda: "samfocus" in m.serial(), 40): return fail(tag, "her window never opened (no samfocus)")
        # 5. the first key, typed the instant her input has focus and before her portrait has even loaded
        m.typ("whats"); time.sleep(0.2); m.keys("ret")
        if not m.wait(lambda: "samtyped=" in m.serial(), 40): return fail(tag, "the first message never reached her (no samtyped=)")
        first = re.findall(r"samtyped=(.*)", m.serial())[0].strip()
        if first != "whats": fail(tag, f"first typed letter lost: she was handed {first!r}, want 'whats'")
        else: print(tag + "first message typed at focus reached her whole: 'whats'")
        if not m.wait(lambda: "face: hd=736" in m.serial(), 90): return fail(tag, "her portrait never loaded (no 'face: hd=736')")
        if not m.wait(lambda: "speak: status=" in m.serial(), 60): return fail(tag, "her first reply was never spoken")
        # a word typed while she speaks: its first key used to stop the speech and vanish
        m.typ("what"); time.sleep(0.2); m.keys("ret")
        if not m.wait(lambda: m.serial().count("samtyped=") >= 2, 30): return fail(tag, "the message typed while she spoke never arrived")
        second = re.findall(r"samtyped=(.*)", m.serial())[1].strip()
        if second != "what": fail(tag, f"a key typed while she was speaking was eaten: she was handed {second!r}, want 'what'")
        else: print(tag + "message typed while she spoke reached her whole: 'what'")
        # wait for everything to go quiet: her own marker says every caption has faded and the mouth is shut
        if not m.wait(lambda: m.serial().count("chatreply=") >= 2 and captions_gone(m), 90): return fail(tag, "the captions never faded out after her answers")
        base = m.still(need=3)   # captions are gone (serial says so); the picture itself settles a frame or two later
        if base is None: return fail(tag, "her picture never settled after the captions faded")
        m.shot(base, "01-full-screen-idle")
        PW, PH, sc = m.PW, m.PH, m.scale
        # 1. room: the whole screen is her picture, framed by the 2.12.1 spec (her wall in the bands, 86 percent wide, eyes 40 percent down)
        geometry_check(m, tag, base, before)
        # 2. glass
        x0, y0, x1, y1 = panel_rect(m)
        # picture above the panel, inside the portrait and clear of its faded edge (the first ~134 px are her wall, then a faded edge up to ~300 at 1920 wide; 2.12.1 moved this probe in from x=80)
        S_, band_ = wide_geom(m); ox0 = (band_ + S_ // 10 + 20) * m.scale
        out = stats(base, ox0, ox0 + 160, y0 - 140, y0 - 120)
        inl = stats(base, x0 + 40, x0 + 40 + 40, y0 + 12, y0 + 16)  # panel, left, clear of the text
        inr = stats(base, x1 - 120, x1 - 120 + 40, y0 + 12, y0 + 16)
        print(tag + f"deviation outside {out[3]:.1f}, inside left {inl[3]:.1f}, right {inr[3]:.1f}; mean outside {sum(out[:3]) / 3:.0f}, inside {sum(inl[:3]) / 3:.0f}")
        if out[3] < 20: fail_s(tag, f"the test picture has no hard detail outside the panel ({out[3]:.1f}); the glass assertions would mean nothing")
        if max(inl[3], inr[3]) > 9: fail_s(tag, f"the panel is not blurred: stripes survive inside it (deviation {max(inl[3], inr[3]):.1f})")
        if not inl[0] > inl[2] + 12: fail_s(tag, f"the panel's left is not a reddish copy of the picture behind it: {inl[:3]}")
        if not inr[2] > inr[0] + 12: fail_s(tag, f"the panel's right is not a bluish copy of the picture behind it: {inr[:3]}")
        if sum(inl[:3]) / 3 < sum(out[:3]) / 3 + 15: fail_s(tag, "the panel is not tinted lighter than the picture above it")
        seam = [base.getpixel((x, y0 + 14)) for x in range(PW // 2 - 40, PW // 2 + 40)]
        mid = sum(1 for p in seam if abs(p[0] - p[2]) < 60)
        if mid < 10: fail_s(tag, f"the panel has no smooth red-to-blue seam ({mid} intermediate pixels): a flat box, not a blur of the picture")
        # 3. captions: send, watch the backdrop come, hold, fade, and go. The picture is static, so after the fade the
        #    region must be the baseline again, and the fade itself must pass through in-between values.
        LW = PW // sc
        bot_box = ((LW // 2 - 120) * sc, (m_ybot(m) - 34) * sc, (LW // 2 + 120) * sc, m_ybot(m) * sc)   # a one-line caption just above the input line (a window with room under her lips)
        # Her mouth sits low in a wide window (2.12.1): fewer than two rows fit under her lips, and an opening mouth and chin travel down
        # toward the glass, so the stack is capped at one compact row right above the input (bottom up; never over her forehead).
        tight, fits, boxtop, capB = slot_geom(m)
        capbox = slot_box(m) if tight else bot_box   # her caption's row; the fade and the degree sign are read here
        zones = face_zones(m); zy0 = min(z[1] for z in zones.values()); zy1 = max(z[3] for z in zones.values())
        zone_worst = {k: 0.0 for k in zones}; shut = Shut(); zone_polls = 0
        speech["pcm"] = SILENT   # her mouth stays shut: only a caption can darken her lips, and an open mouth cannot be mistaken for one
        n_lines = len(ticks(m, "line"))
        m.typ(MSG); m.keys("ret")
        samples = []; t_end = time.time() + 90; shot_cap = shot_half = False
        while time.time() < t_end:
            img = m.frame((capbox[1] - 4, capbox[3] + 4)); v = darkened(img, base, capbox); samples.append(v)
            if len(samples) % 4 == 0:
                a = last_open(m); zimg = m.frame((zy0, zy1)); b = last_open(m)
                settled = shut.poll(a, b); zone_polls += settled
                if settled:
                    for k, z in zones.items(): zone_worst[k] = max(zone_worst[k], darkened(zimg, base, z))
            top = max(samples)
            if top > 8 and not shot_cap and v > 0.9 * top and len(samples) > 8: shot_cap = True; m.shot(m.frame(), "02-caption-visible")
            if top > 8 and not shot_half and 0.3 * top < v < 0.7 * top and len(samples) > 2 and samples[-2] > v: shot_half = True; m.shot(m.frame(), "03-caption-half-faded")
            if top > 8 and v < 0.5 and "speak: status=" in m.serial() and captions_gone(m): break
            time.sleep(0.03)
        speech["pcm"] = TONE
        top = max(samples)
        print(tag + "captions against her face: one compact row at the bottom, darkest on " + ", ".join(f"{k} {w:.2f}" for k, w in zone_worst.items()) + f" ({zone_polls} polls with her mouth settled shut)")
        if zone_polls < 5: fail(tag, f"her mouth was never settled shut while the captions were up ({zone_polls} polls), so nothing could be measured")
        if not tight: fail(tag, "at 1920x1080 fewer than two rows fit under her lips; the arithmetic in slot_geom says there is room")
        for k, w in zone_worst.items():
            if w > 0.5: fail(tag, f"a caption landed on {k} (it was darkened by {w:.1f} levels; nothing may be drawn there)")
        # the whole stack goes together, 4 s of quiet after the last line and her voice (her ticks, not the host's clock), then a 0.6 s fade
        ln = ticks(m, "line"); fd = ticks(m, "fade"); gn = ticks(m, "gone")
        if len(ln) <= n_lines or not fd or not gn: fail(tag, f"her fade markers are missing (lines {ln}, fade {fd}, gone {gn})")
        else:
            print(tag + f"last line at tick {ln[-1]}, the whole stack began to fade at {fd[-1]}, gone at {gn[-1]}")
            if fd[-1] - ln[-1] < 400: fail(tag, f"the stack began to fade {fd[-1] - ln[-1]} ticks after the last line, want at least 400 (4 s of quiet)")
            if not 60 <= gn[-1] - fd[-1] <= 75: fail(tag, f"the whole stack took {gn[-1] - fd[-1]} ticks to fade, want 60 (0.6 s)")
        print(tag + f"caption backdrop: darkens the strip by up to {top:.1f} levels, {samples[-1]:.2f} left at the end, {len(samples)} polls")
        if top <= 8: fail(tag, "no caption appeared over the picture after she answered")
        elif samples[-1] >= 0.5: fail(tag, "the caption never went away after her voice ended")
        else:
            # the last fade: walk back from the end to the last time the strip was at full strength
            i = len(samples) - 1
            while i > 0 and samples[i] < 0.9 * top: i -= 1
            mids = [v for v in samples[i:] if 0.1 * top < v < 0.9 * top]
            print(tag + f"the last fade passed through {len(mids)} in-between strengths")
            if len(mids) < 2: fail(tag, f"the caption does not fade: it went from full to nothing in {len(mids)} in-between polls")
            else: print(tag + "captions appeared, faded out and left the picture as it was")
        # 4. history
        hx0, hy0, hx1, hy1 = x0, 56 * sc, x1, (y0 // sc - 14) * sc
        m.keys("tab")
        if not m.wait(lambda: history_state(m) == "1", 15): return fail(tag, "Tab did not reach her (no history=1)")
        if not m.wait(lambda: stats(m.frame((hy0, hy1)), hx0 + 20, hx0 + 44, hy0 + 80, hy0 + 84)[3] < 9, 15):
            m.shot(m.frame(), "04-scrollback-missing"); fail(tag, "Tab did not open a glass scrollback over the picture")
        else:
            hist = m.frame(); m.shot(hist, "04-scrollback")
            ink = sum(1 for y in range(hy0 + 40, hy0 + 330, 2) for x in range(hx0 + 40, hx1 - 40, 2) if max(hist.getpixel((x, y))) < 0x55)
            print(tag + f"scrollback: glass over the picture, {ink} dark text pixels")
            if ink < 60: fail(tag, f"the scrollback has no conversation in it ({ink} dark pixels)")
        m.keys("tab")
        if not m.wait(lambda: history_state(m) == "0", 15): return fail(tag, "closing the scrollback never reached her (no history=0)")
        if not m.wait(lambda: abs(mean_lum(m.frame((hy0, hy1)), (hx0 + 20, hy0 + 20, hx1 - 20, hy1 - 20)) - mean_lum(base, (hx0 + 20, hy0 + 20, hx1 - 20, hy1 - 20))) < 2, 15):
            fail(tag, "Tab did not close the scrollback")
        # 5. the degree sign, in the weather caption
        if not m.wait(lambda: captions_gone(m), 60): return fail(tag, "the captions never faded before the weather question")
        m.typ("weather"); m.keys("ret")
        if not m.wait(lambda: "chattool=weather:" in m.serial(), 40): return fail(tag, "the weather tool never ran")
        wx = re.findall(r"chattool=weather:(.*)", m.serial())[0]
        if wx.strip() == "none": return fail(tag, "the weather tool had no reading to speak")
        print(tag + f"weather text she was given: {wx.encode('latin-1')!r}")
        degree_check(m, tag, capbox, base)
        # 6. the mouth
        mouth_check(m, tag, base)
        # 7. Esc
        m.wait(lambda: captions_gone(m), 60)
        m.keys("esc")
        if not m.wait(lambda: "samantha: closed" in m.serial(), 20): return fail(tag, "Esc did not close her")
        after = None
        def desktop_back():
            nonlocal after
            after = m.frame(); return after.getpixel((1200, 935)) == DOCK_COLOUR
        if not m.wait(desktop_back, 20): return fail(tag, "after Esc the dock never came back")
        ink = sum(1 for y in range(8, 44) for x in range(80, 280) if max(after.getpixel((x, y))) < 0x50)
        if ink < 30: fail(tag, f"after Esc the menu bar is not back ({ink} dark pixels)")
        else: print(tag + "Esc closed her; the dock and the menu bar are back")
        m.shot(after, "09-after-esc")
        n_closed = m.serial().count("samantha: closed")
        m.open_from_dock()
        if not m.wait(lambda: m.serial().count("samfocus") >= 2, 40): return fail(tag, "she did not open a second time")
        time.sleep(2.0)
        m.click(24, 24)   # the red dot
        if not m.wait(lambda: m.serial().count("samantha: closed") > n_closed, 20): fail(tag, "the red dot did not close her")
        else: print(tag + "the red dot closed her")
        if "rebooting" in m.serial().lower() or "panic" in m.serial().lower(): fail(tag, "the machine panicked")
    finally:
        m.close()


def degree_check(m, tag, capbox, base):
    """The weather caption is white text on the dark backdrop. Its third glyph (after the two digits) must be a small
    ring in the top of the line, not a question mark that reaches the baseline."""
    img = None; seen = 0.0
    mid = (m.PW // 2 - 36 * m.scale, capbox[1], m.PW // 2 + 36 * m.scale, capbox[3])   # where a short caption's text sits
    def caption_up():
        nonlocal img, seen
        img = m.frame((capbox[1] - 4, capbox[3] + 4)); seen = max(seen, darkened(img, base, mid))
        return seen > 12   # the backdrop is over the picture (against the idle picture, wherever on the screen the caption is)
    if not m.wait(caption_up, 20): m.shot(m.frame(), "05-weather-missing"); return fail(tag, f"the weather caption never appeared (darkest {seen:.1f})")
    time.sleep(0.4); img = m.frame((capbox[1] - 4, capbox[3] + 4))
    m.shot(m.frame(), "05-weather-caption")
    px = img.load()
    x0, x1 = capbox[0], capbox[2]
    ys = [y for y in range(capbox[1], capbox[3]) if sum(1 for x in range(x0, x1, 2) if min(px[x, y]) > 215) > 3]
    if not ys: return fail(tag, "no white text found in the weather caption")
    top, bot = min(ys), max(ys)
    cols = []
    for x in range(x0, x1):
        yy = [y for y in range(top - 2, bot + 3) if min(px[x, y]) > 200]
        cols.append((min(yy), max(yy)) if yy else None)
    groups, cur = [], []
    for c in cols:
        if c: cur.append(c)
        elif cur: groups.append(cur); cur = []
    if cur: groups.append(cur)
    glyphs = [(min(a for a, _ in g), max(b for _, b in g), len(g)) for g in groups]
    if len(glyphs) < 3: return fail(tag, f"the weather caption has too few glyphs to read ({len(glyphs)})")
    gt, gb, _ = glyphs[2]; dt, db, _ = glyphs[0]
    print(tag + f"glyph rows (physical): digit {glyphs[0][:2]}, digit {glyphs[1][:2]}, third {glyphs[2][:2]}")
    if gb - gt > (db - dt) * 0.6 or gb > dt + (db - dt) * 0.65: fail(tag, f"the degree sign draws as '?' or a box: third glyph rows {gt}-{gb}, digits {dt}-{db}")
    else: print(tag + "the degree sign is a real ring in the top of the line")


def mouth_check(m, tag, base):
    """Her voice is 1 s loud, 1 s silent, 1 s quiet, 1 s loud. The opening she reports and the pixels in the mouth
    rectangle must both follow it."""
    geom = re.findall(r"samface: mouth=(\d+),(\d+),(\d+),(\d+)", m.serial())
    if not geom: return fail(tag, "she never reported where her mouth is")
    cx, cy, hw, depth = [int(v) * m.scale for v in geom[-1]]
    box = (cx - hw, cy - 6 * m.scale, cx + hw, cy + depth - 10 * m.scale)   # the lip zone, clear of the caption backdrop's feathered edge
    if not m.wait(lambda: captions_gone(m), 60): return fail(tag, "the captions never faded before the mouth test")
    speech["pcm"] = MOUTH_PCM
    closed = m.still((box[1], box[3]), need=4)   # the closed portrait, once it has really stopped changing (not after a fixed sleep)
    if closed is None: return fail(tag, "the closed mouth never settled before the mouth test")
    n0 = m.serial().count("speak: status=")
    m.typ("tell me something"); m.keys("ret")
    pairs = []; times = []; t_end = time.time() + 120; quiet = []; zero_since = None
    while time.time() < t_end:
        a = last_open(m)
        img = m.frame((box[1], box[3]))
        b = last_open(m)
        # She prints "open=0" as she draws the closing frame, a moment before that frame is on the screen, so a poll that sees
        # 0 straight after the opening changed may still be looking at the last frame with the mouth ajar (a slow runner made
        # this 0.2). The silent assertion therefore reads only polls where 0 has been reported for 0.3 s already.
        if a == 0 and b == 0:
            if zero_since is None: zero_since = time.time()
        elif a is not None: zero_since = None
        settled_zero = a == 0 and b == 0 and zero_since is not None and time.time() - zero_since >= 0.3
        if a is not None and a == b:
            d = 0; px = img.load(); cp = closed.load(); n = 0
            for y in range(box[1], box[3], 2):
                for x in range(box[0], box[2], 2):
                    d += sum(abs(px[x, y][i] - cp[x, y][i]) for i in range(3)); n += 1
            pairs.append((a, d / n)); times.append(time.time())
            if settled_zero: quiet.append(d / n)
            if SHOTS and a >= 14 and len(pairs) > 40 and not os.path.exists(os.path.join(SHOTS, "06-mouth-open.png")): m.shot(m.frame(), "06-mouth-open")   # well into her speech, after the captions have cross-faded
        if m.serial().count("speak: status=") > n0 and last_open(m) == 0 and len(pairs) > 20 and time.time() > t_end - 105: break
        time.sleep(0.05)
    if len(pairs) < 20: return fail(tag, f"too few mouth samples ({len(pairs)})")
    by = {}
    for a, d in pairs: by.setdefault(a, []).append(d)
    summary = {a: sum(v) / len(v) for a, v in sorted(by.items())}
    print(tag + "mouth opening -> mean pixel change vs the closed portrait: " + ", ".join(f"{a}: {d:.1f}" for a, d in summary.items()))
    levels = sorted(summary)
    if levels[-1] < 10: return fail(tag, f"her mouth never opened wide for loud speech (top opening {levels[-1]})")
    if 0 not in summary: return fail(tag, "her mouth never closed for the silent second")
    # the silent second is in the middle of her speech: between the first and last time she was open there must be a
    # stretch of at least 0.3 s with the mouth shut
    live = [i for i, (a, _) in enumerate(pairs) if a > 0]
    gap = 0.0; run_start = None
    for i in range(live[0], live[-1] + 1):
        if pairs[i][0] == 0:
            run_start = times[i] if run_start is None else run_start; gap = max(gap, times[i] - run_start)
        else: run_start = None
    print(tag + f"longest shut stretch inside her speech: {gap:.2f} s")
    if gap < 0.3: fail(tag, f"her mouth stayed open through the silent second of her speech (longest shut stretch {gap:.2f} s)")
    if not quiet: fail(tag, "no poll caught the mouth settled shut inside her speech, so silence could not be measured")
    else:
        q = sum(quiet) / len(quiet)
        print(tag + f"settled shut polls: {len(quiet)}, mean pixel change {q:.2f}")
        if q > 0.2: fail(tag, f"silent speech still changes the mouth pixels ({q:.1f}); the closed mouth must be the portrait exactly")
    # She prints each opening as she draws it and the frame reaches the screen a moment later, so on a slow host a poll in the
    # middle of a change pairs the new number with the old picture. The correlation is therefore taken over steady polls
    # only: those whose reported opening is the same as in the polls on either side, so the picture on screen is the one the
    # number describes. The threshold is unchanged, and a constant mouth still has no variation to correlate.
    held = [pairs[i] for i in range(1, len(pairs) - 1) if pairs[i - 1][0] == pairs[i][0] == pairs[i + 1][0]]
    if len(held) < 20: return fail(tag, f"too few steady mouth samples ({len(held)} of {len(pairs)})")
    xs = [a for a, _ in held]; ys = [d for _, d in held]
    mx_, my_ = sum(xs) / len(xs), sum(ys) / len(ys)
    cov = sum((x - mx_) * (y - my_) for x, y in held); vx = sum((x - mx_) ** 2 for x in xs); vy = sum((y - my_) ** 2 for y in ys)
    r = cov / math.sqrt(vx * vy) if vx and vy else 0
    print(tag + f"correlation between her reported opening and the mouth pixels: {r:.3f} over {len(held)} steady polls of {len(pairs)}")
    if r < 0.9: fail(tag, f"the mouth pixels do not follow her opening (correlation {r:.2f})")
    if len(set(a for a, _ in pairs)) < 4: fail(tag, f"her mouth only used {len(set(a for a, _ in pairs))} different openings: it is not following the loudness")
    speech["pcm"] = TONE


PAGE = (0xFA, 0xF8, 0xF6)   # BG in user/samantha.c: what she paints before her portrait is up
RED_DOT = (24 * 2, 24 * 2, (0xFF, 0x5F, 0x57))   # her close dot, physical px at scale 2: drawn only by her window, never by the desktop


def dot_up(img): return img.getpixel(RED_DOT[:2]) == RED_DOT[2]


def corners_painted(PW, PH, top, dot=True, wall=False):
    """True once all four corners show her picture. A wide window (`wall`) shows her wall colour in the corners (geometry_check
    judges whether it is the right one); a tall one (the phone) has the synthetic portrait's red on the left and blue on the
    right. Black (window not composited yet), the desktop wallpaper (no red dot) and the plain page colour she paints before the
    portrait is up are none of those."""
    def ok(img):
        if dot and not dot_up(img): return False   # the wallpaper is also a stable, non-black picture; only her window has the red dot
        for x, y in ((6, top), (PW - 7, top), (6, PH - 7), (PW - 7, PH - 7)):
            p = img.getpixel((x, y))
            if wall:   # painted, not the page colour she shows before the portrait is up: geometry_check judges which colour it is
                if p == (0, 0, 0) or max(abs(p[i] - PAGE[i]) for i in range(3)) < 6: return False
            elif not SYNTH:
                if p == (0, 0, 0): return False
            elif not ((p[0] > p[2] + 25) if x < PW // 2 else (p[2] > p[0] + 25)): return False
        return True
    return ok


def scenario_small():
    tag = "1024x768 boot flag: "
    m = Machine(tag, (1024, 768), f"samantha noblink res=1024x768 llmhost=10.0.2.2 llmport={port} facehost=10.0.2.2:{port} wxhost=10.0.2.2:{port}", 2)
    try:
        if not m.wait(lambda: "face: hd=736" in m.serial(), 120): return fail(tag, "her portrait never loaded")
        img = m.still(ok=corners_painted(m.PW, m.PH, 6, wall=True), secs=120)   # wait for the picture to be painted, however slow the host
        if img is None: return fail(tag, "her picture never finished painting on the small screen")
        m.shot(img, "07-small-screen")
        geometry_check(m, tag, img)
        caption_run(m, tag, img)   # her reply over the picture: not on her lips or her eyes
    finally:
        m.close()


def scenario_wide():
    """1920x1080 by the boot flag: the same room around her as the desktop drive (scenario 1), without driving her."""
    tag = "1920x1080 boot flag: "
    m = Machine(tag, (1920, 1080), f"samantha noblink res=1920x1080 llmhost=10.0.2.2 llmport={port} facehost=10.0.2.2:{port} wxhost=10.0.2.2:{port}", 2)
    try:
        if not m.wait(lambda: "face: hd=736" in m.serial(), 120): return fail(tag, "her portrait never loaded")
        img = m.still(ok=corners_painted(m.PW, m.PH, 6, wall=True), secs=120)
        if img is None: return fail(tag, "her picture never finished painting")
        m.shot(img, "11-wide-boot-flag")
        geometry_check(m, tag, img)
    finally:
        m.close()


def bands(img, base, x0, x1, y0, y1):
    """The caption boxes in a column band: [(top, bottom, strength)], top to bottom. A box is a run of rows whose mean darkening against the idle
    picture is over 4 percent of what the backdrop could darken there; boxes whose feathers touch are one run, split where the darkening dips between two flat tops. strength is the box's mean
    darkening in those percent, so a dimmer, older line reads lower."""
    a = img.load(); b = base.load(); rows = []
    for y in range(y0, y1):
        t = 0; n = 0
        for x in range(x0, x1, 4):
            d = lum(b[x, y]) - lum(a[x, y]); t += d if d > 0 else 0; n += max(lum(b[x, y]) - 16, 1)   # as a share of what the backdrop could darken: a bright shirt and dark hair compare alike
        rows.append(100 * t / n)
    runs = []; cur = None
    for i, v in enumerate(rows):
        if v > 4:
            if cur and i - cur[1] <= 6: cur[1] = i; cur[2].append(v)
            else:
                if cur: runs.append(cur)
                cur = [i, i, [v]]
    if cur: runs.append(cur)
    out = []
    for r0, r1, vs in runs:
        sm = [sum(vs[max(0, i - 2):i + 3]) / len(vs[max(0, i - 2):i + 3]) for i in range(len(vs))]
        cuts = [0]; peak = sm[0]; lo = None
        for i, v in enumerate(sm):
            if lo is None:
                if v > peak: peak = v
                elif v < 0.8 * peak: lo = (v, i)
            else:
                if v < lo[0]: lo = (v, i)
                elif v > lo[0] * 1.25 + 0.5: cuts.append(lo[1]); peak = v; lo = None
        cuts.append(len(vs))
        for c0, c1 in zip(cuts, cuts[1:]):
            seg = vs[c0:c1]
            if len(seg) >= 12: out.append((y0 + r0 + c0, y0 + r0 + c1 - 1, sum(seg) / len(seg)))
    return out


def centroid(img, base, x0, x1, y0, y1):
    """The darkening-weighted mean row in a column band (None when nothing is darkened): where a caption box is, whatever its fade alpha."""
    a = img.load(); b = base.load(); t = w = 0
    for y in range(y0, y1):
        for x in range(x0, x1, 2):
            d = lum(b[x, y]) - lum(a[x, y])
            if d > 0: t += d * y; w += d
    return t / w if w > 8 else None


def stack_check(m, tag, base):
    """The conversation-aware stack in a window with room under her lips (the phone), measured on the framebuffer in a narrow column band beside the
    text of full-width caption boxes: a new line rises into place over ~0.28 s of her clock; up to three lines stack with the newest at the bottom
    just above the input and each older one dimmer; a fourth never shows a fourth box; and the whole stack fades together once the exchange has been
    quiet for 4 s after the last line and her voice (her ticks), a 0.6 s fade, not while the exchange is still going."""
    global REPLY
    PW, PH = m.PW, m.PH
    ptop = panel_rect(m)[1]
    x0, x1 = 48, 70   # the padding left of the text inside a full-width caption box (two rows, so the box is as wide as its text): no glyphs, only the backdrop
    y0, y1 = ptop - 420, ptop - 2
    LONG = "hello there my friend how are you doing today and what is new with you"
    old_reply = REPLY; REPLY = "Hi, lovely to see you. It is good to have you here, and I am glad you stopped by to say hello."
    speech["pcm"] = SILENT; delay["chat"] = 1.0
    try:
        n0 = len(ticks(m, "line")); m.typ(LONG); m.keys("ret")
        # (your own line is drawn once and then she waits on the network, so it is her reply that animates)
        if not m.wait(lambda: len(ticks(m, "line")) >= n0 + 2 and any(int(r[0]) == ticks(m, "line")[n0 + 1] and int(r[2]) == 0 for r in re.findall(r"samface: rise=(\d+),(\d+),(\d+)", m.serial())), 60): return fail(tag, "her reply never finished rising")
        born = ticks(m, "line")[n0 + 1]
        rise = [(int(t), int(y)) for b_, t, y in re.findall(r"samface: rise=(\d+),(\d+),(\d+)", m.serial()) if int(b_) == born]
        print(tag + f"the new line rose on her clock (tick since it appeared, rows below its place): {rise}")
        if len(rise) < 3 or rise[0][1] < 8 or rise[-1][1] != 0 or any(b[1] > a[1] for a, b in zip(rise, rise[1:])) or any(b[0] <= a[0] for a, b in zip(rise, rise[1:])):
            fail(tag, f"the new line did not ease up into place (ticks and offsets {rise}): want 3 or more steps from at least 8 rows down to 0, never moving back down")
        if not 24 <= rise[-1][0] <= 40: fail(tag, f"the rise ended at tick {rise[-1][0]} after the line appeared, want about 28 (0.28 s)")
        if not m.wait(lambda: len(ticks(m, "line")) >= n0 + 2, 60): return fail(tag, "her reply never arrived")
        m.typ(LONG); m.keys("ret")   # a second message straight away: the exchange goes on
        most = 0; three = None; t_end = time.time() + 90; last = None; run = 0
        def risen(k):   # the k-th new line has finished rising on her clock, so it is at full strength (your own line waits on the network, so the stack is three lines once her second reply lands)
            return len(ticks(m, "line")) >= n0 + k and any(int(r[0]) == ticks(m, "line")[n0 + k - 1] and int(r[2]) == 0 for r in re.findall(r"samface: rise=(\d+),(\d+),(\d+)", m.serial()))
        while time.time() < t_end:
            img = m.frame((y0, y1)); bs = bands(img, base, x0, x1, y0, y1); most = max(most, len(bs))
            if risen(4) and len(bs) == 3:   # hers, yours, hers: all risen, none fading
                run = run + 1 if last == bs else 1; last = bs
                if run >= 3 and three is None: three = bs; m.shot(m.frame(), "15-three-lines")
            if three is not None: break
            time.sleep(0.01)
        print(tag + f"the most boxes on screen at once: {most}; three stacked: {three}")
        if most > 3: fail(tag, f"{most} caption boxes at once, want at most 3")
        if three is None: fail(tag, "three lines never stacked together")
        else:
            st = [b[2] for b in three]
            if not (st[0] < st[1] < st[2]): fail(tag, f"three stacked lines are not dimmer going up: strengths {[round(v, 1) for v in st]}")
            if not (three[0][1] < three[1][0] and three[1][1] < three[2][0] and three[2][1] <= ptop): fail(tag, f"the three boxes are not stacked bottom up above the input: {three}")
        if not m.wait(lambda: captions_gone(m), 120): return fail(tag, "the stack never cleared")
        ln = ticks(m, "line"); fd = ticks(m, "fade"); gn = ticks(m, "gone")
        print(tag + f"lines at ticks {ln}, whole stack began to fade at {fd}, gone at {gn}")
        if not fd or not gn: return fail(tag, "no fade or gone marker from her")
        if fd[-1] - ln[-1] < 400: fail(tag, f"the stack faded {fd[-1] - ln[-1]} ticks after the last line, want at least 400")
        if not 60 <= gn[-1] - fd[-1] <= 75: fail(tag, f"the whole stack took {gn[-1] - fd[-1]} ticks to fade, want 60")
        if len(fd) != 1 or fd[0] < ln[-1]: fail(tag, f"the stack faded while the exchange was still going (fade ticks {fd}, lines {ln})")
    finally:
        speech["pcm"] = TONE; delay["chat"] = 0.0; REPLY = old_reply


def scenario_phone():
    tag = "430x760 phone: "
    m = Machine(tag, (860, 1520), f"phone samantha noblink llmhost=10.0.2.2 llmport={port} facehost=10.0.2.2:{port} wxhost=10.0.2.2:{port}", 2)
    try:
        if not m.wait(lambda: "face: hd=736" in m.serial(), 120): return fail(tag, "her portrait never loaded")
        img = m.still(ok=corners_painted(m.PW, m.PH, 96, dot=False), secs=120)
        if img is None: return fail(tag, "her picture never finished painting on the phone")
        m.shot(img, "08-phone")
        PW, PH = m.PW, m.PH
        for name, (x, y) in {"top-left": (6, 96), "top-right": (PW - 6, 96), "bottom-left": (6, PH - 6), "bottom-right": (PW - 6, PH - 6)}.items():
            p = img.getpixel((x, y)); left = x < PW // 2
            if not ((p[0] > p[2] + 25) if left else (p[2] > p[0] + 25)): fail_s(tag, f"her picture does not fill the {name} of the phone: {p}")
        if not any(f.startswith(tag) for f in fails): print(tag + "her picture fills the phone screen below the back strip")
        stack_check(m, tag, img)
    finally:
        m.close()


def scenario_big(res):
    """A screen over 960x540 logical is past the 2.1 MB a ring-3 window buffer can be: she must still open (in an
    ordinary window, composed the same way) and show her portrait, not be refused."""
    tag = f"{res[0]}x{res[1]} boot flag: "
    m = Machine(tag, res, f"samantha noblink res={res[0]}x{res[1]} llmhost=10.0.2.2 llmport={port} facehost=10.0.2.2:{port} wxhost=10.0.2.2:{port}", 2)
    try:
        if not m.wait(lambda: "samfocus" in m.serial() or "winrefuse" in m.serial(), 120): return fail(tag, "she never opened")
        if "winrefuse" in m.serial(): return fail(tag, "the kernel refused her window on a big screen")
        if not m.wait(lambda: "face: hd=736" in m.serial(), 180): return fail(tag, "her portrait never loaded on a big screen")
        def painted(im):   # the same red and blue count as below, so "still" means "still and showing her", not still and black
            px = im.load(); r = b = 0
            for y in range(res[1] // 4, 3 * res[1] // 4, 8):
                for x in range(res[0] // 4, 3 * res[0] // 4, 8):
                    p = px[x, y]; r += p[0] > p[2] + 40; b += p[2] > p[0] + 40
            return (r >= 20 and b >= 20) if SYNTH else (r + b >= 20)   # an ordinary window here: no red dot to look for
        img = m.still(ok=painted, secs=180)
        if img is None: return fail(tag, "her picture never finished painting on a big screen")
        m.shot(img, f"10-big-{res[0]}")
        px = img.load(); red = blue = 0
        for y in range(res[1] // 4, 3 * res[1] // 4, 8):
            for x in range(res[0] // 4, 3 * res[0] // 4, 8):
                p = px[x, y]
                if p[0] > p[2] + 40: red += 1
                if p[2] > p[0] + 40: blue += 1
        if SYNTH and (red < 20 or blue < 20): fail(tag, f"her portrait is not on screen (red {red}, blue {blue} samples)")
        else: print(tag + f"she opened in an ordinary window and her portrait shows (red {red}, blue {blue} samples)")
        # a short window (here 804x344 logical) has no room under her lips and none above her eyebrows either: her reply takes one row
        # right at the top (and says "..." where it goes on); it still must not land on her lips or her eyes
        if SYNTH:
            org = window_origin(m, img)
            if org is None: return fail(tag, "could not find her window on the screen (no green eye patch)")
            print(tag + f"her window's picture starts at {org} on the screen")
            caption_run(m, tag, img, org)
        else: caption_run(m, tag, img, None)
    finally:
        m.close()


def scenario_big2k(): scenario_big((2560, 1440))


def scenario_blink():
    """She blinks on her own (2.9.1). This is the one machine booted without `noblink`; every other scenario passes it
    because they compare exact pixels and a blink would be a change they cannot tell from a fault. Here the eyes are
    watched until four are reported on serial and two are seen (90 s deadline): at least two blinks, each one a real change in the eye boxes that goes back to exactly the open
    picture, 3 to 6 s apart (the schedule is a counter through a hash, not the clock), and the mouth never moves."""
    tag = "blink: "
    m = Machine(tag, (1024, 768), f"samantha res=1024x768 llmhost=10.0.2.2 llmport={port} facehost=10.0.2.2:{port} wxhost=10.0.2.2:{port}", 2)
    try:
        if not m.wait(lambda: "face: hd=736" in m.serial() and "samface: eyes=" in m.serial(), 120): return fail(tag, "her portrait never loaded")
        g = [int(v) * m.scale for v in re.findall(r"samface: eyes=(\d+),(\d+),(\d+),(\d+),(\d+),(\d+),(\d+)", m.serial())[-1]]
        mg = [int(v) * m.scale for v in re.findall(r"samface: mouth=(\d+),(\d+),(\d+),(\d+)", m.serial())[-1]]
        y0, y1 = min(g[1], g[4]) - 2 * g[6], max(g[1], g[4]) + 2 * g[6]
        boxes = [(g[0] - g[2], g[0] + g[2]), (g[3] - g[5], g[3] + g[5])]
        mbox = (mg[0] - mg[2], mg[1] - 6 * m.scale, mg[0] + mg[2], mg[1] + mg[3] - 10 * m.scale)
        def eyes(img):
            px = img.load(); out = []
            for (xa, xb) in boxes:
                out.append(bytes(c for y in range(y0, y1, 2) for x in range(xa, xb, 2) for c in px[x, y]))
            return out
        def mouth(img):
            px = img.load(); return bytes(c for y in range(mbox[1], mbox[3], 2) for x in range(mbox[0], mbox[2], 2) for c in px[x, y])
        def differs(a, b): return sum(1 for u, v in zip(a, b) if abs(u - v) > 12)
        rows = (min(y0, mbox[1]), max(y1, mbox[3]))
        # The window is composited some time after the portrait loads, and a slow runner can take many seconds: the open
        # picture is the first one that has been the same for a second, and is not black.
        if not m.wait(lambda: corners_painted(m.PW, m.PH, 6, wall=True)(m.frame()), 120, step=0.25): return fail(tag, "her window never showed her picture")   # not the wallpaper behind her
        first = m.still(rows, need=4)
        if first is None: return fail(tag, "her open eyes never settled to a stable picture")
        base_e, base_m = eyes(first), mouth(first)
        t0 = time.time(); starts = []; closed = False; worst = 0; moved = 0; peak = None
        # Watch until she has reported 4 blinks and two of them were caught on screen, or 90 s of wall clock. A blink lasts
        # 120 ms of her clock, which is less than that on the host, so a slow poll can miss one: the deadline is long and the
        # loop ends on what was seen, not on a fixed 16 s.
        def reported(): return len(re.findall(r"samface: blink=(\d+)", m.serial()))
        while time.time() - t0 < 90:
            if reported() >= 4 and len(starts) >= 2 and not closed: break
            img = m.frame() if SHOTS else m.frame(rows)   # screenshot runs keep the whole frame of the deepest blink
            e = eyes(img); d = max(differs(e[0], base_e[0]), differs(e[1], base_e[1]))
            if mouth(img) != base_m: moved += 1
            if d > 40 and not closed: closed = True; starts.append(time.time() - t0); worst = 0
            if closed:
                worst = max(worst, d)
                if SHOTS and (peak is None or d > peak): peak = d; m.shot(img, "05-mid-blink")
            if closed and e == base_e: closed = False   # back to exactly the open picture
            time.sleep(0.02)
        t1 = time.time()
        while closed and time.time() - t1 < 5.0:   # the deadline may land in the middle of a blink: give it its 120 ms
            e = eyes(m.frame(rows))
            if e == base_e: closed = False
        gaps = [b - a for a, b in zip(starts, starts[1:])]
        ticks = [int(v) for v in re.findall(r"samface: blink=(\d+)", m.serial())]   # her own clock: 100 ticks a second, whatever the host does
        tgaps = [b - a for a, b in zip(ticks, ticks[1:])]
        print(tag + f"{len(starts)} blinks seen on screen, {len(ticks)} reported, gaps {tgaps} ticks; mouth changed in {moved} polls")
        if len(starts) < 2: fail(tag, f"only {len(starts)} blinks seen on screen in {time.time() - t0:.0f} s ({len(ticks)} reported)")
        if closed: fail(tag, "the eyes never went back to exactly the open picture after a blink")
        if moved: fail(tag, f"the mouth changed {moved} times while she only blinked")
        # the gap after a blink is 300 to 599 ticks (3 to 6 s on her 100 Hz clock) plus the 12 ticks the blink itself takes; the host's wall
        # clock is not the measure (QEMU's timer runs fast while the guest idles), so the ticks she reports are
        if len(ticks) < 2 or any(not (312 <= x <= 611) for x in tgaps): fail(tag, f"blinks not 3 to 6 s apart on her clock: {tgaps} ticks")
        if len(set(tgaps)) < 2 and len(tgaps) > 2: fail(tag, f"every gap is the same, so the schedule is not varying: {tgaps}")
    finally:
        m.close()


scens = [s for s in (scenario_desktop, scenario_wide, scenario_small, scenario_phone, scenario_big2k, scenario_blink) if not os.environ.get("ONLY") or os.environ["ONLY"] in s.__name__]
for scen in scens:
    try: scen()
    except Exception as e: fails.append(f"{scen.__name__}: {type(e).__name__}: {e}")
if fails:
    print("\n".join("FAIL: " + f for f in fails)); sys.exit(1)
print("PASS: Samantha is full screen, the words are glass and fading captions, the typing bugs stay fixed, and her mouth follows her voice")
