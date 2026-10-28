#!/usr/bin/env python3
"""A feature film for Joshua Tree in the grammar of a launch tour: hard cuts on a steady beat, each card a flat colour
block with one plain label beside a real screenshot, a fast run of games on single beats, then the mark builds out of
squares and lands on the name. The grammar is borrowed from Butterfly's Pond launch video (2026-10-27); the pictures,
palette, type and score are Joshua Tree's own.

    uv run -q --with pillow --with numpy python cards.py [OUT.mp4]

Every picture is a real screen from the landing's shots or the Pi. The score is original and generated from the same
timeline: 120 BPM in D, a sub kick on every beat, a plucked D major 9 arpeggio, a soft pad, a bell and a low boom on
the name. Frames stream to ffmpeg, so nothing is held in memory.
"""
import os, subprocess, sys, wave
import numpy as np
from PIL import Image, ImageDraw, ImageFont

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, "../../.."))
R = lambda p: os.path.join(ROOT, p)
OUT = os.path.abspath(sys.argv[1]) if len(sys.argv) > 1 else os.path.join(HERE, "cards.mp4")
W, H, FPS, BPM = 1920, 1080, 30, 120
BEAT = 60 / BPM
PAL = {"paper": (250, 248, 244), "ink": (20, 19, 17), "grey": (110, 110, 110), "terra": (181, 80, 44), "clay": (146, 63, 32),
       "sand": (236, 228, 214)}
LABEL = ImageFont.truetype(R("tools/fonts/treesans/TreeSansDisplay-SemiBold.ttf"), 84)
BIG = ImageFont.truetype(R("tools/fonts/treesans/TreeSansDisplay-Bold.ttf"), 168)
TAG = ImageFont.truetype(R("tools/fonts/treesans/TreeSans-SemiBold.ttf"), 54)
SMALL = ImageFont.truetype(R("tools/fonts/treesans/TreeSans-Medium.ttf"), 34)
MARK = Image.open(R("docs/brand/mark-paper.png")).convert("RGBA")
SHOT = lambda n: R("landing/shots/%s.webp" % n)
BUILDS = lambda n: R("docs/hardware/ad/builds/%s.jpg" % n)

# The timeline, in beats: (kind, beats, ...). One card is two beats; a game is one.
# A split card's picture: (path, focus x, focus y, zoom). Zoom 1 shows a window whole; more cuts in around the focus.
TL = [("open", 4),
      ("full", 4, SHOT("desktop-apps"), "Joshua Tree"),
      ("split", 2, "Written from scratch", "ink", "terra", (BUILDS("code"), 0.3, 0.5, 1.25)),
      ("split", 2, "Runs on a Pi 4", "terra", "sand", (R("landing/nimbus-hero.jpg"), 0.5, 0.5, 1.0)),
      ("split", 2, "Weather", "paper", "terra", (SHOT("app-weather"), 0.5, 0.5, 1.0)),
      ("split", 2, "Stocks", "sand", "ink", (SHOT("app-stocks"), 0.5, 0.5, 1.0)),
      ("split", 2, "Calendar", "ink", "clay", (SHOT("app-calendar"), 0.5, 0.5, 1.0)),
      ("split", 2, "Terminal", "terra", "sand", (SHOT("app-terminal"), 0.5, 0.5, 1.0)),
      ("split", 2, "Samantha", "ink", "terra", (R("landing/hero/samantha.webp"), 0.5, 0.5, 1.0)),
      ("split", 2, "She writes apps", "paper", "grey", (BUILDS("humans"), 0.5, 0.5, 1.0)),
      ("split", 2, "An app store", "clay", "ink", (BUILDS("dice"), 0.5, 0.5, 1.0)),
      ("split", 2, "Breathe", "sand", "terra", (SHOT("app-windgate"), 0.5, 0.5, 1.0)),
      ("split", 1, "Games", "ink", "terra", (SHOT("app-chess"), 0.5, 0.5, 1.0)),
      ("split", 1, "Games", "ink", "clay", (SHOT("app-solitaire"), 0.5, 0.5, 1.0)),
      ("split", 1, "Games", "ink", "grey", (SHOT("app-depths"), 0.5, 0.5, 1.0)),
      ("split", 1, "Games", "ink", "terra", (SHOT("app-hamurapi"), 0.5, 0.5, 1.0)),
      ("split", 1, "Games", "ink", "clay", (SHOT("app-bricks"), 0.5, 0.5, 1.0)),
      ("split", 3, "Its own compiler", "terra", "ink", (BUILDS("pi-fib"), 0.5, 0.5, 1.0)),
      ("split", 2, "That builds itself", "paper", "clay", (BUILDS("pi-self"), 0.5, 0.5, 1.0)),
      ("three", 2),
      ("count", 2, SHOT("desktop-apps")),
      ("end", 8)]
T, t = [], 0.0
for c in TL:
    T.append(t); t += c[1] * BEAT
DUR = t
T_END = T[-1]

def cover(img, w, h, z=1.0):                    # img filling w x h, cropped around its centre, zoomed by z
    img = img.convert("RGB"); s = max(w / img.width, h / img.height) * z
    im = img.resize((int(img.width * s + 1), int(img.height * s + 1)), Image.LANCZOS)
    x, y = (im.width - w) // 2, (im.height - h) // 2
    return im.crop((x, y, x + w, y + h))

def fit(img, w, h):                             # img inside w x h, whole
    img = img.convert("RGB"); s = min(w / img.width, h / img.height)
    return img.resize((int(img.width * s), int(img.height * s)), Image.LANCZOS)

def rounded(im, r=20):
    m = Image.new("L", (im.width * 4, im.height * 4), 0)
    ImageDraw.Draw(m).rounded_rectangle((0, 0, m.width - 1, m.height - 1), r * 4, fill=255)
    return m.resize(im.size, Image.LANCZOS)

def ink_on(bg): return PAL["ink"] if sum(PAL[bg]) > 500 else PAL["paper"]

def label(d, text, x, y, colour, font=LABEL):
    d.text((x, y), text, font=font, fill=colour, anchor="ls")

cache = {}
def pic(path):
    if path not in cache: cache[path] = Image.open(path)
    return cache[path]

LW = 768                                        # the label's block; the picture gets the rest
BOX = (W - LW - 128, H - 224)

def region(im, fx, fy, z):                     # the box-shaped region around (fx, fy), z times narrower than the image
    w = im.width / z; h = w * BOX[1] / BOX[0]
    if h > im.height: h = im.height; w = h * BOX[0] / BOX[1]
    x = min(max(0, im.width * fx - w / 2), im.width - w); y = min(max(0, im.height * fy - h / 2), im.height - h)
    return im.crop((int(x), int(y), int(x + w), int(y + h)))

def lines(d, text):                             # one line, or two split at the space nearest the middle when it is too wide
    if d.textlength(text, font=LABEL) <= LW - 200: return [text]
    sp = [i for i, c in enumerate(text) if c == " "]; i = min(sp, key=lambda i: abs(i - len(text) / 2))
    return [text[:i], text[i + 1:]]

def split(text, lbg, rbg, shot, k):             # k: 0 to 1 through the card, for a slow push-in on the picture
    f = Image.new("RGB", (W, H), PAL[rbg]); d = ImageDraw.Draw(f)
    d.rectangle((0, 0, LW, H), fill=PAL[lbg])
    ls = lines(d, text)
    for i, l in enumerate(ls): label(d, l, 112, H // 2 + 28 + (i - (len(ls) - 1) / 2) * 100, ink_on(lbg))
    path, fx, fy, z = shot; im = pic(path).convert("RGB"); g = 1.0 + 0.03 * k
    if "samantha" in path or "nimbus" in path:   # a face or a render fills its block; a screen sits on the colour
        p = cover(im, W - LW, H, 1.0 + 0.04 * k); f.paste(p, (LW, 0)); return f
    p = fit(im, BOX[0], BOX[1]) if z == 1.0 else region(im, fx, fy, z).resize(BOX, Image.LANCZOS)
    p = p.resize((int(p.width * g), int(p.height * g)), Image.LANCZOS)
    f.paste(p, (LW + (W - LW - p.width) // 2, (H - p.height) // 2), rounded(p, 22))
    return f

def tag(d, text, x, y, bg, fg, font=TAG):       # a label in its own box, as the tour's closing words sit
    l, t_, r, b = d.textbbox((x, y), text, font=font, anchor="ls")
    d.rectangle((l - 18, t_ - 14, r + 18, b + 18), fill=bg); d.text((x, y), text, font=font, fill=fg, anchor="ls")

def full(path, text, k):
    f = cover(pic(path), W, H, 1.02 + 0.04 * k); d = ImageDraw.Draw(f)
    tag(d, text, 112, H - 112, PAL["terra"], PAL["paper"])
    return f

def mark(size, colour):                         # the mark, recoloured: its alpha over a flat colour
    m = MARK.resize((size, size), Image.LANCZOS); a = m.split()[3]
    return Image.new("RGB", (size, size), colour), a

def three(k):
    f = Image.new("RGB", (W, H)); d = ImageDraw.Draw(f); w = W // 3
    for i, bg in enumerate(("sand", "terra", "ink")): d.rectangle((i * w, 0, (i + 1) * w, H), fill=PAL[bg])
    label(d, "i386", w // 2 - d.textlength("i386", font=LABEL) // 2, H // 2 + 28, PAL["ink"])
    m, a = mark(300, PAL["paper"]); f.paste(m, (w + (w - 300) // 2, (H - 300) // 2), a)
    label(d, "ARM64", 2 * w + w // 2 - d.textlength("ARM64", font=LABEL) // 2, H // 2 + 28, PAL["paper"])
    return f

def count(path, k):
    f = Image.new("RGB", (W, H), PAL["terra"]); d = ImageDraw.Draw(f)
    label(d, "38", 112, H // 2 + 40, PAL["paper"], BIG); label(d, "apps", 112, H // 2 + 130, PAL["paper"])
    p = fit(pic(path), W // 2 + 80, H - 300); f.paste(p, (W - p.width - 112, (H - p.height) // 2), rounded(p))
    return f

GRID = 9                                        # the open and the end: a field of squares on ink, lighting up
def field(lit, cell=96, gap=12):
    f = Image.new("RGB", (W, H), PAL["ink"]); d = ImageDraw.Draw(f)
    n = GRID; side = n * cell + (n - 1) * gap; x0, y0 = (W - side) // 2, (H - side) // 2
    for r in range(n):
        for c in range(n):
            x, y = x0 + c * (cell + gap), y0 + r * (cell + gap)
            on = lit.get((r, c))
            d.rectangle((x, y, x + cell, y + cell), outline=(54, 52, 48), width=2, fill=PAL[on] if on else None)
    return f, (x0, y0, side)

rng = np.random.default_rng(7)
ORDER = [tuple(x) for x in rng.permutation([(r, c) for r in range(GRID) for c in range(GRID)])]
def open_(k):                                   # squares light in terracotta, a few at a time, then the mark sits in the middle
    n = int(len(ORDER) * min(1, k * 1.25)); lit = {ORDER[i]: ("terra" if i % 5 else "clay") for i in range(n)}
    f, (x0, y0, side) = field(lit)
    if k > 0.8:
        g = Image.new("RGB", (side, side), PAL["ink"]); a = Image.new("L", (side, side), int(255 * min(1, (k - 0.8) / 0.12)))
        f.paste(g, (x0, y0), a); m, ma = mark(420, PAL["paper"]); f.paste(m, ((W - 420) // 2, (H - 420) // 2), ma)
    return f

def end(k, secs):                               # the mark, the name, then the words land in boxes, block by block
    f = Image.new("RGB", (W, H), PAL["ink"]); d = ImageDraw.Draw(f)
    m, a = mark(260, PAL["paper"]); f.paste(m, ((W - 260) // 2, 170), a)
    name = "Joshua Tree"; d.text((W // 2, 610), name, font=BIG, fill=PAL["paper"], anchor="ms")
    s = k * secs
    for i, (text, bg, fg) in enumerate((("Free", PAL["terra"], PAL["paper"]), ("Download the ISO", PAL["paper"], PAL["ink"]))):
        at = 1.0 + i * 0.5
        if s < at: continue
        x = W // 2 - (d.textlength("Free", font=TAG) + 70 + d.textlength("Download the ISO", font=TAG)) // 2
        if i: x += d.textlength("Free", font=TAG) + 70
        if s - at < 0.2:                        # three quick blocks before the word: the box builds the way the mark did
            l, t_, r, b = d.textbbox((x, 780), text, font=TAG, anchor="ls"); step = int((s - at) / 0.2 * 3) + 1
            for j in range(step): d.rectangle((l - 18 + j * (r - l + 36) // 3, t_ - 14, l - 18 + (j + 1) * (r - l + 36) // 3, b + 18), fill=bg)
        else:
            tag(d, text, x, 780, bg, fg)
    if s > 2.2: d.text((W // 2, 900), "joshuatree.jaybulb.com", font=SMALL, fill=(170, 166, 158), anchor="ms")
    return f

def grain(img, amount=0.012):
    a = np.asarray(img, dtype=np.int16); n = rng.normal(0, 255 * amount, a.shape[:2])[..., None]
    return np.clip(a + n, 0, 255).astype(np.uint8)

silent = OUT + ".video.mp4"
enc = subprocess.Popen(["ffmpeg", "-v", "error", "-y", "-f", "rawvideo", "-pix_fmt", "rgb24", "-s", f"{W}x{H}", "-r", str(FPS), "-i", "-",
                        "-c:v", "libx264", "-pix_fmt", "yuv420p", "-crf", "20", "-preset", "slow", silent], stdin=subprocess.PIPE)
frame = 0
for i, c in enumerate(TL):
    t1 = T[i + 1] if i + 1 < len(T) else DUR
    m = int(round(t1 * FPS)) - frame
    for j in range(m):
        k = j / max(1, m - 1)
        if c[0] == "open": f = open_(k)
        elif c[0] == "full": f = full(c[2], c[3], k)
        elif c[0] == "split": f = split(c[2], c[3], c[4], c[5], k)
        elif c[0] == "three": f = three(k)
        elif c[0] == "count": f = count(c[2], k)
        else: f = end(k, c[1] * BEAT)
        enc.stdin.write(grain(f).tobytes()); frame += 1
enc.stdin.close(); enc.wait()

# ---- the score: 120 BPM in D, cut to the same beats ----
SR = 48000; N = int((DUR + 0.5) * SR); mix = np.zeros((N, 2))
tt = lambda d: np.arange(int(d * SR)) / SR
hz = lambda m: 440 * 2 ** ((m - 69) / 12)
def soft(sig, a=0.004, r=0.04):
    n = len(sig); env = np.ones(n); ka, kr = min(n, int(a * SR)), min(n, int(r * SR))
    env[:ka] = np.linspace(0, 1, ka); env[n - kr:] *= np.linspace(1, 0, kr); return sig * env
def put(t0, sig, g, pan=0.0):
    i = int(t0 * SR); sig = soft(sig)[: max(0, N - i)]
    mix[i:i + len(sig), 0] += sig * g * np.cos((pan + 1) * np.pi / 4); mix[i:i + len(sig), 1] += sig * g * np.sin((pan + 1) * np.pi / 4)
def pluck(m, d=0.6):
    x = tt(d); f = hz(m); return (np.sin(2 * np.pi * f * x) + 0.3 * np.sin(2 * np.pi * 4 * f * x) * np.exp(-x * 18)) * np.exp(-x * 7)
def kick(d=0.35):
    x = tt(d); f = 45 + 90 * np.exp(-x * 30); return np.sin(2 * np.pi * np.cumsum(f) / SR) * np.exp(-x * 9)
def tick(d=0.05):
    x = tt(d); n = rng.standard_normal(len(x)); n = n - np.convolve(n, np.ones(8) / 8, "same"); return n * np.exp(-x * 90)
def pad(notes, d):
    x = tt(d); s = sum(np.sin(2 * np.pi * hz(m) * x * (1 + det)) for m in notes for det in (-0.002, 0.002))
    env = np.minimum(1, x / 0.6) * np.minimum(1, (d - x) / 0.6); return s * env / (2 * len(notes))
def bell(m, d=4.0):
    x = tt(d); f = hz(m); return np.sin(2 * np.pi * f * x + 1.2 * np.sin(2 * np.pi * 2.76 * f * x) * np.exp(-x * 3)) * np.exp(-x * 1.3)
def boom(d=3.0):
    x = tt(d); return np.sin(2 * np.pi * (36.7 * x + 30 * (1 - np.exp(-x * 8)) / 8)) * np.exp(-x * 1.6)

CHORDS = [[50, 62, 66, 69, 76], [47, 62, 66, 71, 74], [43, 59, 62, 66, 69], [45, 61, 64, 69, 73]]   # D, Bm7, Gmaj7, A: one a bar
ARP = [0, 2, 3, 4, 3, 2, 1, 2]                  # the chord's tones on eighths
beats = int(T_END / BEAT)
for b in range(beats):
    t0 = b * BEAT; ch = CHORDS[(b // 4) % 4]
    if b >= 4: put(t0, kick(), 0.55)                                         # the pulse starts with the first picture
    if b >= 8: put(t0 + BEAT / 2, tick(), 0.05, 0.3 if b % 2 else -0.3)
    for e in range(2):
        put(t0 + e * BEAT / 2, pluck(ch[1:][ARP[(b * 2 + e) % 8] % 4] + 12), 0.10 if b >= 4 else 0.07, -0.25 + 0.5 * e)
    if b % 4 == 0:
        put(t0, pad(ch[1:], 4 * BEAT + 0.3), 0.16); put(t0, soft(np.sin(2 * np.pi * hz(ch[0] - 12) * tt(4 * BEAT))), 0.10)
put(T_END, bell(86), 0.12, -0.1); put(T_END, bell(81), 0.07, 0.2)            # the name: a bell, then the boom under it
put(T_END, boom(), 0.5); put(T_END, pad([62, 66, 69, 76], DUR - T_END), 0.18)
for ch in range(2):                                                          # a short room: two quiet echoes
    mix[int(0.11 * SR):, ch] += 0.18 * mix[:-int(0.11 * SR), 1 - ch]
mix *= 10 ** (-1.5 / 20) / np.abs(mix).max()
wav = OUT + ".wav"
with wave.open(wav, "wb") as w:
    w.setnchannels(2); w.setsampwidth(2); w.setframerate(SR); w.writeframes((mix * 32767).astype("<i2").tobytes())
subprocess.run(["ffmpeg", "-v", "error", "-y", "-i", silent, "-i", wav, "-c:v", "copy", "-c:a", "aac", "-b:a", "192k", "-shortest", OUT], check=True)
os.remove(silent); os.remove(wav)
print(OUT, "%.1f s" % DUR)
