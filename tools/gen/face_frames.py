#!/usr/bin/env python3
"""Cut Samantha's Chat face loops from her character videos.

Reads idle.mp4 and talk.mp4 from ~/.samantha/characters/<name>/ (the
character-creator skill writes them) and writes 360x360 baseline JPEGs to
landing/face/: idle-0..23.jpg (2s of her idle loop at 12fps: breathing,
blinking) and talk-0..35.jpg (3s of her talking loop at 12fps). The
kernel plays them in order, like the Mac face window plays the videos:
idle while she's quiet, talk while her voice is sounding, held on pauses
(kernel/chat_face.h). Real consecutive frames, so her head, eyes and mouth
move together; blending mouths onto one still head read as uncanny.

Each loop starts where its last frame looks most like its first, so it
wraps without a jump.

Talk frames keep their order in time: the kernel plays the clip forward and,
of the next frame or two, shows the one whose mouth best matches how loud
her voice is right now (it measures each frame's mouth itself).

Usage: tools/gen/face_frames.py [character]
Env:   CROP=w:h:x:y   square crop on the face (default: the centered square of
                      the video, which is where character-creator frames the face,
                      so any character works with no tuning)
"""
import glob, os, subprocess, sys, tempfile
from PIL import Image, ImageChops

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
name = sys.argv[1] if len(sys.argv) > 1 else "samantha"
src = os.environ.get("FACE_SRC", os.path.expanduser(f"~/.samantha/characters/{name}"))
crop = os.environ.get("CROP", "ih:ih:(iw-ih)/2:0")
out = os.path.join(ROOT, "landing", "face")
SIDE, FPS = 320, 12
COUNT = {"idle": 24, "talk": 60}

for kind in COUNT:
    if not os.path.exists(f"{src}/{kind}.mp4"):
        sys.exit(f"face_frames: missing {src}/{kind}.mp4")


def seamless(paths, n):
    """n consecutive frames whose last frame best matches the first."""
    small = [Image.open(p).convert("L").resize((40, 40)) for p in paths]
    diff = lambda a, b: sum(ImageChops.difference(small[a], small[b]).get_flattened_data())
    start = min(range(len(paths) - n + 1), key=lambda s: diff(s, s + n - 1))
    return paths[start:start + n]


with tempfile.TemporaryDirectory() as work:
    picked = {}
    for kind, n in COUNT.items():
        subprocess.run(["ffmpeg", "-v", "error", "-y", "-i", f"{src}/{kind}.mp4",
                        "-vf", f"crop={crop},scale={SIDE}:{SIDE}:flags=lanczos,fps={FPS}", f"{work}/{kind}-%03d.png"], check=True)
        picked[kind] = seamless(sorted(glob.glob(f"{work}/{kind}-*.png")), n)
    for f in glob.glob(f"{out}/idle-*") + glob.glob(f"{out}/talk-*"):
        os.remove(f)
    os.makedirs(out, exist_ok=True)
    for kind, paths in picked.items():
        for i, p in enumerate(paths):
            dst = f"{out}/{kind}-{i}.jpg"
            Image.open(p).convert("RGB").save(dst, "JPEG", quality=88, progressive=False, subsampling=2)
        print(f"face_frames: {len(paths)} {kind} frames, {sum(os.path.getsize(f'{out}/{kind}-{i}.jpg') for i in range(len(paths))) // 1024}KB")
