#!/usr/bin/env python3
"""Cut Samantha's Chat face frames from her character videos.

Reads idle.mp4 and talk.mp4 from ~/.samantha/characters/<name>/ (the
character-creator skill writes them) and writes 480x480 baseline JPEGs to
landing/face/: idle-0..5.jpg, evenly spaced through the idle loop, and
talk-0..11.jpg, ordered from closed lips to a wide open mouth. The kernel
picks a talk frame by how loud her voice is right now (kernel/chat_face.h),
so the order is what makes the mouth follow the audio.

How open the mouth is: the share of very dark (inside the mouth) or very
bright (teeth) pixels in a box around the lips. Every talk candidate is
scored and 12 are taken at even steps from least to most open.

Her head moves in the talk clip, so showing whole talk frames made the head
jump between mouth shapes. Instead each talk frame is idle-0 with only the
mouth pasted in: the lower face of the talk frame is aligned to idle-0 by a
small shift search, then the lip box is blended in through a feathered
ellipse. One steady head, only the mouth moves.

Usage: tools/gen/face_frames.py [character]
Env:   CROP=w:h:x:y   square crop on her face (default fits the 854x480 loops)
       MOUTH=x0,y0,x1,y1  lip box inside the crop
"""
import glob, os, subprocess, sys, tempfile
from PIL import Image, ImageChops, ImageDraw, ImageFilter

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
name = sys.argv[1] if len(sys.argv) > 1 else "samantha"
src = os.environ.get("FACE_SRC", os.path.expanduser(f"~/.samantha/characters/{name}"))
crop = os.environ.get("CROP", "480:480:180:0")
mouth = tuple(int(v) for v in os.environ.get("MOUTH", "195,270,295,335").split(","))
ALIGN = (150, 240, 330, 380)   # lower face used to line a talk frame up with idle-0
PASTE = (180, 255, 310, 352)   # lip box blended into idle-0
out = os.path.join(ROOT, "landing", "face")
IDLE, TALK, SIDE = 6, 12, 480

for kind in ("idle", "talk"):
    if not os.path.exists(f"{src}/{kind}.mp4"):
        sys.exit(f"face_frames: missing {src}/{kind}.mp4")


def frames(kind, work):
    subprocess.run(["ffmpeg", "-v", "error", "-y", "-i", f"{src}/{kind}.mp4",
                    "-vf", f"crop={crop},scale={SIDE}:{SIDE}:flags=lanczos,fps=12", f"{work}/{kind}-%03d.png"], check=True)
    return sorted(glob.glob(f"{work}/{kind}-*.png"))


def openness(path):
    px = Image.open(path).convert("L").crop(mouth).get_flattened_data()
    return sum(1 for v in px if v < 85 or v > 200) / len(px)


def composite(base, path):
    """idle-0 with the mouth of the talk frame at path, aligned and feathered."""
    im = Image.open(path).convert("RGB")
    size = ((ALIGN[2] - ALIGN[0]) // 2, (ALIGN[3] - ALIGN[1]) // 2)
    ref = base.convert("L").crop(ALIGN).resize(size)
    g = im.convert("L")
    best = min((sum(ImageChops.difference(g.crop((ALIGN[0] + dx, ALIGN[1] + dy, ALIGN[2] + dx, ALIGN[3] + dy)).resize(size), ref).get_flattened_data()), dx, dy)
               for dy in range(-30, 31, 2) for dx in range(-30, 31, 2))
    _, dx, dy = best
    w, h = PASTE[2] - PASTE[0], PASTE[3] - PASTE[1]
    mask = Image.new("L", (w, h), 0)
    ImageDraw.Draw(mask).ellipse((8, 8, w - 8, h - 8), fill=255)
    out = base.copy()
    out.paste(im.crop((PASTE[0] + dx, PASTE[1] + dy, PASTE[2] + dx, PASTE[3] + dy)), (PASTE[0], PASTE[1]), mask.filter(ImageFilter.GaussianBlur(9)))
    return out


def pick(items, n):
    return [items[round(i * (len(items) - 1) / (n - 1))] for i in range(n)]


with tempfile.TemporaryDirectory() as work:
    idle = pick(frames("idle", work), IDLE)
    talk = [p for _, p in pick(sorted((openness(p), p) for p in frames("talk", work)), TALK)]
    for f in glob.glob(f"{out}/idle-*") + glob.glob(f"{out}/talk-*"):
        os.remove(f)
    os.makedirs(out, exist_ok=True)
    base = Image.open(idle[0]).convert("RGB")
    images = [("idle", i, Image.open(p).convert("RGB")) for i, p in enumerate(idle)]
    images += [("talk", i, composite(base, p)) for i, p in enumerate(talk)]
    for kind, i, im in images:
        dst = f"{out}/{kind}-{i}.jpg"
        im.save(dst, "JPEG", quality=85, progressive=False, subsampling=2)
        print(f"face_frames: {dst} {os.path.getsize(dst)} bytes")
