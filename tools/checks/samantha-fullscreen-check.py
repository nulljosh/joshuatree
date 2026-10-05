#!/usr/bin/env python3
"""Samantha full screen, headless and pixel based (2.9.0): she covers the whole screen, the conversation is glass and
captions over her picture, the two old typing bugs stay fixed, and her mouth follows her voice.

Everything runs with -display none and every host stubbed on loopback (facehost=, llmhost= and wxhost= point at one
stub through QEMU's user NAT at 10.0.2.2; the live site is never touched). The stub's portrait (/face/hd.jpg) is
synthetic so every pixel can be read without guessing: left half red, right half blue, wide dark stripes (hard detail
a blur must remove), and horizontal bars where her mouth sits. Set SAMANTHA_SHOTS=<dir> to serve her real portrait
instead and save PNGs of each state there; in that mode the checks about the synthetic portrait's colours (cover, glass) are skipped, set SYN=1 to keep them with the synthetic picture.

  desktop 1920x1080   click her dock icon, then drive her like a person (scenarios 1 to 7).
  res=1024x768        the "samantha" boot flag opens her first, on a small screen.
  phone               the 430x760 phone mode opens her; she fills what the phone leaves her.

Asserted on the real framebuffer, each with a deadline and a poll, never a fixed sleep:
  1 cover     her window is the whole screen: the four corners, the old menu bar and the old dock strip are all face.
  2 glass     outside the input panel the picture has hard stripes; inside it the same columns are a smooth blur that is
              still red on the left and blue on the right, tinted lighter, with a smooth red-to-blue seam.
  3 captions  what you send appears over the picture (the backdrop darkens it), her answer follows, both are gone
              again after her voice ends (the picture is back to exactly what it was), and in between a poll catches
              the fade half way.
  4 history   Tab opens a glass scrollback with the whole conversation in it; Tab closes it.
  5 bugs      the first typed letter: what she was handed (her own samtyped= serial line, not the stub) is the whole
              word, also for a word typed while she speaks. The degree sign: the weather answer draws a real degree
              ring in its caption, not the "?" the old 0xF8 byte turned into.
  6 mouth     the stub plays loud, silent and quiet speech; her mouth opening (samface: open=) tracks it and the pixels
              in the mouth rectangle follow that opening, silent speech leaves them exactly as the closed portrait.
  7 exit      Esc closes her cleanly and the desktop (menu bar, dock) is alive; the red dot closes her too.

Discriminating: put the 0xF8 degree back in the sysinfo copy and (5) fails by name; make the key handler eat the key that
stops speech again and (5) fails by name; draw the panel without the blur and (2) fails by name; stop the fade and (3)
fails by name; feed the mouth a constant level and (6) fails by name.

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
WX = (b'{"latitude":49.09,"longitude":-122.57,"timezone":"America/Vancouver",'
      b'"current_units":{"time":"iso8601","interval":"seconds","temperature_2m":"\xc2\xb0C","apparent_temperature":"\xc2\xb0C","relative_humidity_2m":"%","wind_speed_10m":"km/h","weather_code":"wmo code"},'
      b'"current":{"time":"2026-09-20T16:15","interval":900,"temperature_2m":14.2,"apparent_temperature":12.8,"relative_humidity_2m":69,"wind_speed_10m":11.4,"weather_code":3},'
      b'"daily_units":{"time":"iso8601","weather_code":"wmo code","temperature_2m_max":"\xc2\xb0C","temperature_2m_min":"\xc2\xb0C"},'
      b'"daily":{"time":["2026-09-20","2026-09-21","2026-09-22","2026-09-23","2026-09-24"],"weather_code":[3,0,61,71,95],'
      b'"temperature_2m_max":[17.6,21.2,15.4,3.1,16.5],"temperature_2m_min":[9.4,8.5,9.2,-2.6,9.0]}}')


def portrait():
    im = Image.new("RGB", (736, 736))
    px = im.load()
    for y in range(736):
        for x in range(736):
            base = (205, 55, 55) if x < 368 else (55, 55, 205)
            if (x // 8) % 2: base = tuple(int(c * 0.35) for c in base)
            if 400 <= y <= 470 and 290 <= x <= 480:   # her mouth: horizontal bars the warp has to move
                base = (235, 220, 190) if ((y - 400) // 6) % 2 == 0 else (150, 60, 50)
            px[x, y] = base
    b = io.BytesIO(); im.save(b, "JPEG", quality=70, subsampling=2); return b.getvalue()


SYN = portrait()
assert len(SYN) < 60000, len(SYN)


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


def m_ybot(m): return m.PH // m.scale - VM_MARGIN - VM_PH - 12
def m_ytop(m): return m_ybot(m) - 74


def scenario_desktop():
    tag = "1920x1080 desktop: "
    m = Machine(tag, (1920, 1080), f"llmhost=10.0.2.2 llmport={port} facehost=10.0.2.2:{port} wxhost=10.0.2.2:{port}", 2)
    try:
        if not m.wait(lambda: m.frame((1020, 1030)).getpixel((961, 1023)) == DOCK_COLOUR, 60): return fail(tag, "the desktop dock never appeared")
        time.sleep(1.0)
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
        time.sleep(0.4)
        base = m.frame(); m.shot(base, "01-full-screen-idle")
        PW, PH, sc = m.PW, m.PH, m.scale
        # 1. cover: the whole screen, menu bar and dock included, is her picture
        probes = {"top-left": (6, 6), "top-right": (PW - 6, 6), "bottom-left": (6, PH - 6), "bottom-right": (PW - 6, PH - 6),
                  "old menu bar": (PW // 2, 20), "old dock": (1200, 935)}
        okc = True
        for name, (x, y) in probes.items():
            p = base.getpixel((x, y)); left = x < PW // 2
            good = (p[0] > p[2] + 25) if left else (p[2] > p[0] + 25)
            if name == "old dock": good = p != DOCK_COLOUR and (p[0] > p[2] + 25 or p[2] > p[0] + 25)
            if not good: okc = False; fail_s(tag, f"her picture does not cover the {name}: {p}")
        if okc and SYNTH: print(tag + "her picture covers the whole screen, menu bar and dock included")
        # 2. glass
        x0, y0, x1, y1 = panel_rect(m)
        out = stats(base, 80, 80 + 160, y0 - 140, y0 - 120)         # picture above the panel
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
        capbox = ((LW // 2 - 120) * sc, (m_ybot(m) - 34) * sc, (LW // 2 + 120) * sc, m_ybot(m) * sc)   # her one-line caption
        m.typ("hello there"); m.keys("ret")
        samples = []; t_end = time.time() + 60; shot_cap = shot_half = False
        while time.time() < t_end:
            img = m.frame((capbox[1] - 4, capbox[3] + 4)); v = darkened(img, base, capbox); samples.append(v)
            top = max(samples)
            if top > 8 and not shot_cap and v > 0.9 * top and len(samples) > 8: shot_cap = True; m.shot(m.frame(), "02-caption-visible")
            if top > 8 and not shot_half and 0.3 * top < v < 0.7 * top and len(samples) > 2 and samples[-2] > v: shot_half = True; m.shot(m.frame(), "03-caption-half-faded")
            if top > 8 and v < 0.5 and "speak: status=" in m.serial() and captions_gone(m): break
            time.sleep(0.03)
        top = max(samples)
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
        degree_check(m, tag, capbox)
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


def degree_check(m, tag, capbox):
    """The weather caption is white text on the dark backdrop. Its third glyph (after the two digits) must be a small
    ring in the top of the line, not a question mark that reaches the baseline."""
    img = None
    def caption_up():
        nonlocal img
        img = m.frame((capbox[1] - 4, capbox[3] + 4))
        return mean_lum(img, capbox) < 200
    if not m.wait(caption_up, 20): return fail(tag, "the weather caption never appeared")
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
    time.sleep(0.3)
    closed = m.frame((box[1], box[3]))
    n0 = m.serial().count("speak: status=")
    m.typ("tell me something"); m.keys("ret")
    pairs = []; times = []; t_end = time.time() + 45
    while time.time() < t_end:
        a = last_open(m)
        img = m.frame((box[1], box[3]))
        b = last_open(m)
        if a is not None and a == b:
            d = 0; px = img.load(); cp = closed.load(); n = 0
            for y in range(box[1], box[3], 2):
                for x in range(box[0], box[2], 2):
                    d += sum(abs(px[x, y][i] - cp[x, y][i]) for i in range(3)); n += 1
            pairs.append((a, d / n)); times.append(time.time())
            if SHOTS and a >= 14 and not os.path.exists(os.path.join(SHOTS, "06-mouth-open.png")): m.shot(m.frame(), "06-mouth-open")
        if m.serial().count("speak: status=") > n0 and last_open(m) == 0 and len(pairs) > 20 and time.time() > t_end - 30: break
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
    if summary[0] > 0.2: fail(tag, f"silent speech still changes the mouth pixels ({summary[0]:.1f}); the closed mouth must be the portrait exactly")
    xs = [a for a, _ in pairs]; ys = [d for _, d in pairs]
    mx_, my_ = sum(xs) / len(xs), sum(ys) / len(ys)
    cov = sum((x - mx_) * (y - my_) for x, y in pairs); vx = sum((x - mx_) ** 2 for x in xs); vy = sum((y - my_) ** 2 for y in ys)
    r = cov / math.sqrt(vx * vy) if vx and vy else 0
    print(tag + f"correlation between her reported opening and the mouth pixels: {r:.3f} over {len(pairs)} polls")
    if r < 0.9: fail(tag, f"the mouth pixels do not follow her opening (correlation {r:.2f})")
    if len(set(a for a, _ in pairs)) < 4: fail(tag, f"her mouth only used {len(set(a for a, _ in pairs))} different openings: it is not following the loudness")
    speech["pcm"] = TONE


def scenario_small():
    tag = "1024x768 boot flag: "
    m = Machine(tag, (1024, 768), f"samantha res=1024x768 llmhost=10.0.2.2 llmport={port} facehost=10.0.2.2:{port} wxhost=10.0.2.2:{port}", 2)
    try:
        if not m.wait(lambda: "face: hd=736" in m.serial(), 120): return fail(tag, "her portrait never loaded")
        time.sleep(1.5)
        img = m.frame(); m.shot(img, "07-small-screen")
        PW, PH = m.PW, m.PH
        for name, (x, y) in {"top-left": (6, 6), "top-right": (PW - 6, 6), "bottom-left": (6, PH - 6), "bottom-right": (PW - 6, PH - 6)}.items():
            p = img.getpixel((x, y)); left = x < PW // 2
            if not ((p[0] > p[2] + 25) if left else (p[2] > p[0] + 25)): fail_s(tag, f"her picture does not cover the {name} corner: {p}")
        if not any(f.startswith(tag) for f in fails): print(tag + "her picture covers all four corners")
    finally:
        m.close()


def scenario_phone():
    tag = "430x760 phone: "
    m = Machine(tag, (860, 1520), f"phone samantha llmhost=10.0.2.2 llmport={port} facehost=10.0.2.2:{port} wxhost=10.0.2.2:{port}", 2)
    try:
        if not m.wait(lambda: "face: hd=736" in m.serial(), 120): return fail(tag, "her portrait never loaded")
        time.sleep(1.5)
        img = m.frame(); m.shot(img, "08-phone")
        PW, PH = m.PW, m.PH
        for name, (x, y) in {"top-left": (6, 96), "top-right": (PW - 6, 96), "bottom-left": (6, PH - 6), "bottom-right": (PW - 6, PH - 6)}.items():
            p = img.getpixel((x, y)); left = x < PW // 2
            if not ((p[0] > p[2] + 25) if left else (p[2] > p[0] + 25)): fail_s(tag, f"her picture does not fill the {name} of the phone: {p}")
        if not any(f.startswith(tag) for f in fails): print(tag + "her picture fills the phone screen below the back strip")
    finally:
        m.close()


def scenario_big(res):
    """A screen over 960x540 logical is past the 2.1 MB a ring-3 window buffer can be: she must still open (in an
    ordinary window, composed the same way) and show her portrait, not be refused."""
    tag = f"{res[0]}x{res[1]} boot flag: "
    m = Machine(tag, res, f"samantha res={res[0]}x{res[1]} llmhost=10.0.2.2 llmport={port} facehost=10.0.2.2:{port} wxhost=10.0.2.2:{port}", 2)
    try:
        if not m.wait(lambda: "samfocus" in m.serial() or "winrefuse" in m.serial(), 120): return fail(tag, "she never opened")
        if "winrefuse" in m.serial(): return fail(tag, "the kernel refused her window on a big screen")
        if not m.wait(lambda: "face: hd=736" in m.serial(), 180): return fail(tag, "her portrait never loaded on a big screen")
        time.sleep(3.0)
        img = m.frame(); m.shot(img, f"10-big-{res[0]}")
        px = img.load(); red = blue = 0
        for y in range(res[1] // 4, 3 * res[1] // 4, 8):
            for x in range(res[0] // 4, 3 * res[0] // 4, 8):
                p = px[x, y]
                if p[0] > p[2] + 40: red += 1
                if p[2] > p[0] + 40: blue += 1
        if SYNTH and (red < 20 or blue < 20): fail(tag, f"her portrait is not on screen (red {red}, blue {blue} samples)")
        else: print(tag + f"she opened in an ordinary window and her portrait shows (red {red}, blue {blue} samples)")
    finally:
        m.close()


def scenario_big2k(): scenario_big((2560, 1440))


scens = [s for s in (scenario_desktop, scenario_small, scenario_phone, scenario_big2k) if not os.environ.get("ONLY") or os.environ["ONLY"] in s.__name__]
for scen in scens:
    try: scen()
    except Exception as e: fails.append(f"{scen.__name__}: {type(e).__name__}: {e}")
if fails:
    print("\n".join("FAIL: " + f for f in fails)); sys.exit(1)
print("PASS: Samantha is full screen, the words are glass and fading captions, the typing bugs stay fixed, and her mouth follows her voice")
