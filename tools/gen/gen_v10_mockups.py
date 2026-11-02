#!/usr/bin/env python3
"""The 10.0 target pictures (Joshua, 2026-11-02: "base the v10 goal on those"), drawn from the real pieces: the park
wallpaper, Tree Sans, the art/icons tiles and the window and dock colours, so every word is real text. Writes
docs/brand/v10/: update.webp (Software Update with the fallback kernel), screen-reader.webp, language.webp, and
nimbus.webp (update.webp put into the monitor of nimbus-desk.webp, a desk photo made once with Higgsfield).
Needs rsvg-convert for the icons. Usage: tools/gen/gen_v10_mockups.py"""
import subprocess, sys, tempfile
from pathlib import Path
from PIL import Image, ImageDraw, ImageFilter, ImageFont

R = Path(__file__).resolve().parents[2]
OUT = R / "docs/brand/v10"
TMP = Path(tempfile.mkdtemp(prefix="jt-v10-"))
F = R / "tools/fonts/treesans"
def font(w, size): return ImageFont.truetype(str(F / ("TreeSans-%s.ttf" % w)), size)
def disp(size): return ImageFont.truetype(str(F / "TreeSansDisplay-Bold.ttf"), size)
UNI = "/System/Library/Fonts/Supplemental/Arial Unicode.ttf"
INK, BODY, RULE, TRAY, EDGE, ACC, RED = (28, 28, 30), (245, 240, 235), (217, 211, 203), (239, 235, 228), (214, 208, 198), (181, 80, 44), (255, 95, 87)
SUB = (110, 104, 98)
W, H = 1920, 1080

def icon(name, px):
    p = TMP / ("icon-%s-%d.png" % (name, px))
    if not p.exists(): subprocess.run(["rsvg-convert", "-w", str(px), "-h", str(px), str(R / "art/icons" / (name + ".svg")), "-o", str(p)], check=True)
    return Image.open(p).convert("RGBA")

def shadow(base, box, radius, blur=28, alpha=90, dy=16):
    sh = Image.new("RGBA", base.size, (0, 0, 0, 0))
    ImageDraw.Draw(sh).rounded_rectangle((box[0], box[1] + dy, box[2], box[3] + dy), radius, fill=(32, 16, 9, alpha))
    base.alpha_composite(sh.filter(ImageFilter.GaussianBlur(blur)))

def desktop(time="9:41 PM"):
    im = Image.open(R / "docs/brand/park-wall.png").convert("RGBA")
    bar = Image.new("RGBA", (W, 52), (250, 248, 245, 200))
    im.alpha_composite(bar)
    d = ImageDraw.Draw(im)
    d.line((0, 51, W, 51), fill=(189, 184, 176))
    g = Image.open(R / "docs/brand/glyph.png").convert("RGBA").resize((34, 34), Image.LANCZOS)
    im.alpha_composite(g, (28, 9))
    d.text((74, 26), "Joshua Tree", font=font("Bold", 26), fill=INK, anchor="lm")
    d.text((W - 32, 26), time, font=font("Medium", 26), fill=INK, anchor="rm")
    # the dock
    names = ["apps", "burrow", "mail", "calendar", "notes", "reminders", "terminal", "bookrank", "weather", "stocks", "settings", "trash"]
    s, gap, pad = 96, 12, 20
    w = len(names) * s + (len(names) - 1) * gap + 2 * pad
    x0, y0 = (W - w) // 2, H - 24 - s - 2 * pad
    shadow(im, (x0, y0, x0 + w, H - 24), 36, blur=18, alpha=70, dy=8)
    d = ImageDraw.Draw(im)
    d.rounded_rectangle((x0, y0, x0 + w, H - 24), 36, fill=TRAY, outline=EDGE, width=2)
    slots = []
    for i, n in enumerate(names):
        x = x0 + pad + i * (s + gap)
        im.alpha_composite(icon(n, s), (x, y0 + pad))
        slots.append((x, y0 + pad, s))
    d.text((W // 2, y0 - 34), "Great artists steal. Picasso", font=font("Medium", 22), fill=(240, 244, 248), anchor="mm")
    return im, slots

def window(im, box, title):
    shadow(im, box, 36)
    d = ImageDraw.Draw(im)
    d.rounded_rectangle(box, 36, fill=BODY)
    x0, y0, x1, _ = box
    d.line((x0, y0 + 88, x1, y0 + 88), fill=RULE, width=2)
    for i, c in enumerate((RED, RULE, RULE)): d.ellipse((x0 + 32 + i * 40, y0 + 32, x0 + 56 + i * 40, y0 + 56), fill=c)
    d.text(((x0 + x1) // 2, y0 + 44), title, font=font("SemiBold", 30), fill=INK, anchor="mm")
    return d

def capsule(d, cx, cy, w, text, primary=True):
    h = 96
    d.rounded_rectangle((cx - w // 2, cy - h // 2, cx + w // 2, cy + h // 2), h // 2, fill=ACC if primary else (233, 227, 219))
    d.text((cx, cy), text, font=font("SemiBold", 32), fill=(255, 255, 255) if primary else INK, anchor="mm")

# 1. Software Update, with the fallback kernel
im, slots = desktop()
box = (W // 2 - 520, 210, W // 2 + 520, 860)
d = window(im, box, "Software Update")
cx = W // 2
im.alpha_composite(Image.open(R / "docs/brand/tree.png").convert("RGBA").resize((150, 150), Image.LANCZOS), (cx - 75, 350))
d.text((cx, 548), "Joshua Tree 10.0", font=disp(64), fill=INK, anchor="mm")
d.text((cx, 608), "A screen reader, twelve languages and safer updates.", font=font("Regular", 30), fill=SUB, anchor="mm")
capsule(d, cx, 694, 420, "Update and Restart")
d.text((cx, 800), "If 10.0 does not start, the Pi goes back to the version that worked.", font=font("Regular", 26), fill=SUB, anchor="mm")
im.convert("RGB").save(OUT / "update.webp", quality=90, method=6)
update_screen = im.copy()

# 2. The screen reader on the dock
im, slots = desktop()
x, y, s = slots[4]
d = ImageDraw.Draw(im)
d.rounded_rectangle((x - 10, y - 10, x + s + 10, y + s + 10), 30, outline=ACC, width=8)
cap = (W // 2 - 440, slots[0][1] - 230, W // 2 + 440, slots[0][1] - 110)
shadow(im, cap, 30, blur=16, alpha=80, dy=8)
d = ImageDraw.Draw(im)
d.rounded_rectangle(cap, 30, fill=(28, 28, 30, 235))
sx, sy = cap[0] + 52, (cap[1] + cap[3]) // 2   # a speaker: a box, a cone, two arcs
d.rectangle((sx, sy - 10, sx + 12, sy + 10), fill=(255, 255, 255)); d.polygon([(sx + 12, sy - 10), (sx + 30, sy - 24), (sx + 30, sy + 24), (sx + 12, sy + 10)], fill=(255, 255, 255))
for r in (18, 30): d.arc((sx + 26 - r, sy - r, sx + 26 + r, sy + r), -45, 45, fill=(255, 255, 255), width=4)
d.text((cap[0] + 136, sy), "Notes, app, 5 of 12. Press Return to open.", font=font("SemiBold", 34), fill=(255, 255, 255), anchor="lm")
d.text((W - 40, 82), "Screen reader on", font=font("SemiBold", 24), fill=(255, 255, 255), anchor="ra")
im.convert("RGB").save(OUT / "screen-reader.webp", quality=90, method=6)

def glyph(d, name, x, y):   # small white glyphs, one per section, no letters
    Wt = (255, 255, 255)
    if name == "About": d.ellipse((x - 2, y - 11, x + 2, y - 7), fill=Wt); d.rounded_rectangle((x - 2, y - 3, x + 2, y + 11), 2, fill=Wt)
    elif name == "Display": d.rounded_rectangle((x - 12, y - 9, x + 12, y + 7), 3, outline=Wt, width=3); d.line((x - 5, y + 11, x + 5, y + 11), fill=Wt, width=3)
    elif name == "Sound": d.polygon([(x - 11, y - 4), (x - 5, y - 4), (x + 2, y - 11), (x + 2, y + 11), (x - 5, y + 4), (x - 11, y + 4)], fill=Wt); d.arc((x - 2, y - 8, x + 12, y + 8), -50, 50, fill=Wt, width=3)
    elif name == "Dock": d.rounded_rectangle((x - 12, y + 3, x + 12, y + 10), 3, fill=Wt); [d.rounded_rectangle((x - 11 + i * 8, y - 6, x - 5 + i * 8, y), 2, fill=Wt) for i in range(3)]
    elif name == "Date and Time": d.ellipse((x - 11, y - 11, x + 11, y + 11), outline=Wt, width=3); d.line((x, y, x, y - 7), fill=Wt, width=3); d.line((x, y, x + 6, y + 2), fill=Wt, width=3)
    elif name == "Users": d.ellipse((x - 5, y - 11, x + 5, y - 1), fill=Wt); d.pieslice((x - 11, y + 1, x + 11, y + 23), 180, 360, fill=Wt)
    elif name == "Wi-Fi": [d.arc((x - r, y + 8 - r, x + r, y + 8 + r), 225, 315, fill=Wt, width=3) for r in (6, 12, 18)]; d.ellipse((x - 2, y + 6, x + 2, y + 10), fill=Wt)
    elif name == "Language": d.ellipse((x - 11, y - 11, x + 11, y + 11), outline=Wt, width=3); d.ellipse((x - 5, y - 11, x + 5, y + 11), outline=Wt, width=2); d.line((x - 11, y, x + 11, y), fill=Wt, width=2)

# 3. Settings, Language
im, slots = desktop()
box = (W // 2 - 560, 150, W // 2 + 560, 860)
d = window(im, box, "Settings")
rows = ["About", "Display", "Sound", "Dock", "Date and Time", "Users", "Wi-Fi", "Language"]
for i, r in enumerate(rows):
    y = box[1] + 120 + i * 72
    if r == "Language": d.rounded_rectangle((box[0] + 20, y - 8, box[0] + 360, y + 60), 18, fill=(233, 227, 219))
    d.rounded_rectangle((box[0] + 36, y + 6, box[0] + 76, y + 46), 12, fill=ACC)
    glyph(d, r, box[0] + 56, y + 26)
    d.text((box[0] + 96, y + 26), r, font=font("Medium" if r != "Language" else "SemiBold", 30), fill=INK, anchor="lm")
card = (box[0] + 390, box[1] + 112, box[2] - 32, box[3] - 32)
d.rounded_rectangle(card, 24, fill=(255, 255, 255))
d.text((card[0] + 32, card[1] + 44), "Language", font=font("SemiBold", 34), fill=INK, anchor="lm")
langs = [("English", 1), ("Español", 0), ("Français", 0), ("Deutsch", 0), ("Português", 0), ("Italiano", 0), ("Tiếng Việt", 0), ("日本語", 0), ("中文", 0), ("한국어", 0), ("हिन्दी", 0), ("Ελληνικά", 0)]
def lang_font(name):
    if any(0x0900 <= ord(c) < 0x0980 for c in name): return ImageFont.truetype("/System/Library/Fonts/Supplemental/Devanagari Sangam MN.ttc", 30)
    if any(ord(c) > 0x2000 for c in name): return ImageFont.truetype(UNI, 30)
    if any(0x0100 <= ord(c) for c in name): return ImageFont.truetype("/System/Library/Fonts/SFNS.ttf", 30)
    return font("Medium", 30)
for i, (name, on) in enumerate(langs):
    col, row = i // 6, i % 6
    lx, ly = card[0] + 32 + col * 340, card[1] + 112 + row * 70
    d.text((lx, ly), name, font=lang_font(name), fill=INK, anchor="lm")
    if on:
        d.line([(lx + 230, ly), (lx + 242, ly + 12), (lx + 266, ly - 14)], fill=ACC, width=6, joint="curve")
    if row < 5: d.line((lx, ly + 37, lx + 300, ly + 37), fill=(236, 231, 224), width=2)
d.text((card[0] + 32, card[3] - 36), "Menus, apps and Samantha follow the language you pick.", font=font("Regular", 24), fill=SUB, anchor="lm")
im.convert("RGB").save(OUT / "language.webp", quality=90, method=6)

# 4. Nimbus on the desk, with the real screen in its monitor
photo = Image.open(OUT / "nimbus-desk.webp").convert("RGB")
scr = (455, 100, 1468, 684)
photo.paste(update_screen.convert("RGB").resize((scr[2] - scr[0], scr[3] - scr[1]), Image.LANCZOS), scr[:2])
photo.save(OUT / "nimbus.webp", quality=90, method=6)
print("ok")
