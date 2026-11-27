#!/usr/bin/env python3
"""The README opens on one banner, the landing hero as a still (13.24.0, issue #1156).

Fails if README.md's first image is not docs/brand/readme-banner.png, if the banner is missing or not 1600 x 640
with transparent corners, if the README goes back to a wall of screenshots (landing/shots/), or if the banner's
headline drifts from the landing's <h1>. Regenerate with tools/gen/gen_readme_banner.py.
13.25.0: the README also shows one real Pi screenshot, docs/brand/readme-weather.png (tools/gen/readme-shots.py): it must
be there, 800 x 600 with rounded corners, and come after the banner.
"""
import os, re, sys
from PIL import Image
ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..")
readme = open(os.path.join(ROOT, "README.md")).read()
gen = open(os.path.join(ROOT, "tools/gen/gen_readme_banner.py")).read()
landing = open(os.path.join(ROOT, "landing/index.html")).read()
fails = []
imgs = re.findall(r'<img[^>]+src="([^"]+)"|!\[[^\]]*\]\(([^)]+)\)', readme)
first = next((a or b for a, b in imgs), None)
if first != "docs/brand/readme-banner.png": fails.append(f"the README's first image is {first}, not the banner")
if "landing/shots/" in readme: fails.append("the README shows landing screenshots again; the banner is its one picture")
shot = os.path.join(ROOT, "docs/brand/readme-weather.png")
if "docs/brand/readme-weather.png" not in [a or b for a, b in imgs][1:]: fails.append("the README does not show the Pi screenshot after the banner")
elif not os.path.exists(shot): fails.append("docs/brand/readme-weather.png is missing; tools/gen/readme-shots.py takes it")
else:
    sh = Image.open(shot)
    if sh.size != (800, 600) or sh.mode != "RGBA" or sh.getpixel((0, 0))[3] != 0: fails.append("readme-weather.png is not 800 x 600 with transparent corners")
p = os.path.join(ROOT, "docs/brand/readme-banner.png")
if not os.path.exists(p): fails.append("docs/brand/readme-banner.png is missing")
else:
    im = Image.open(p).convert("RGBA")
    if im.size != (1600, 640): fails.append(f"the banner is {im.size}, not 1600 x 640")
    if im.getpixel((0, 0))[3] or im.getpixel((1599, 639))[3]: fails.append("the banner's corners are not transparent (the hero card's 28 px radius)")
    if not im.getpixel((800, 320))[3]: fails.append("the banner's middle is transparent")
h1 = re.sub(r"<[^>]+>", "", re.search(r"<h1>(.*?)</h1>", landing).group(1)).strip()
a = re.search(r'HEAD_A, HEAD_B = "([^"]*)", "([^"]*)"', gen)
if not a or (a.group(1) + a.group(2)).strip() != h1: fails.append(f"the banner's headline is not the landing's <h1> ({h1!r})")
if fails:
    print("FAIL:"); [print("  - " + f) for f in fails]; sys.exit(1)
print(f"PASS: the README opens on the 1600 x 640 banner, its headline is the landing's ({h1!r}), and the one Pi screenshot follows it")
