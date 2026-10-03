#!/usr/bin/env python3
"""Cut Samantha's Chat face loops from her character videos.

Reads idle.mp4 and talk.mp4 from ~/.samantha/characters/<name>/ (the
character-creator skill writes them) and writes 360x360 baseline JPEGs to
landing/face/: idle-0..23.jpg (2s of her idle loop at 12fps: breathing,
blinking) and talk-0..47.jpg (4s of her talking loop at 12fps). The
kernel plays them in order, like the Mac face window plays the videos:
idle while she's quiet, talk while her voice is sounding, held on pauses
(kernel/chat_face.h). Real consecutive frames, so her head, eyes and mouth
move together; blending mouths onto one still head read as uncanny.

Each loop is the stretch whose last frame looks most like its first and
whose head moves least, so it wraps without a jump and doesn't sway.

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
out = os.environ.get("FACE_OUT", os.path.join(ROOT, "landing", "face"))   # FACE_OUT=landing/face-joshua for the portfolio face
# 1.9.41: 480px yields sharper retina-quality frames (33KB each at q=88) still under 64KB cap
SIDE, FPS = int(os.environ.get("FACE_SIDE", "480")), 12
COUNT = {"idle": 24, "talk": 48}

for kind in COUNT:
    if not os.path.exists(f"{src}/{kind}.mp4"):
        sys.exit(f"face_frames: missing {src}/{kind}.mp4")


def eyes_open(paths):
    """True per frame when her eyes are open. Open eyes put dark irises in
    the eye band; closed lids are skin. Relative to the clip's own open-eye
    level, so any character works. A long blink stretch in the source clip
    otherwise wins "steadiest" and the loop blinks the whole time."""
    dark = []
    for p in paths:
        g = Image.open(p).convert("L")
        w, h = g.size
        band = g.crop((int(w * .28), int(h * .33), int(w * .72), int(h * .42)))
        px = band.get_flattened_data()
        dark.append(sum(1 for v in px if v < 70) / len(px))
    top = sorted(dark)[int(len(dark) * .9)]
    return [d >= .85 * top for d in dark]


def seamless(paths, n, open_eyes=False):
    """n consecutive frames that loop cleanly (last frame like the first)
    and hold the head steadiest (least frame-to-frame change), so the
    loop point doesn't show and the head doesn't sway."""
    small = [Image.open(p).convert("L").resize((40, 40)) for p in paths]
    diff = lambda a, b: sum(ImageChops.difference(small[a], small[b]).get_flattened_data())
    step = [diff(i, i + 1) for i in range(len(paths) - 1)]
    def cost(s):
        return 3 * diff(s, s + n - 1) + sum(step[s:s + n - 1])
    ok = eyes_open(paths) if open_eyes else [True] * len(paths)
    starts = [s for s in range(len(paths) - n + 1) if all(ok[s:s + n])]
    if not starts:  # no open-eyed stretch that long: keep the full length, take the windows with the fewest shut frames
        # Taking the longest open run instead cut Joshua's talk loop to 8
        # frames: his glasses rims sit in the eye band, so head drift reads as
        # blinking and no run is long. A blink or two inside a 4 s loop is
        # what a real talker does anyway.
        shut = [sum(1 for o in ok[s:s + n] if not o) for s in range(len(paths) - n + 1)]
        least = min(shut)
        starts = [s for s, c in enumerate(shut) if c == least]
        print(f"face_frames: no {n}-frame open-eyed stretch, best window has {least} frames read as shut")
    start = min(starts, key=cost)
    return paths[start:start + n]


def open_start(paths):
    """Frames rotated so the loop starts where her longest open-eyed stretch
    (circular, the clip loops) begins. Her idle clip holds a long closed-eyes
    stretch, so the steadiest window is a closed-eye one and Chat opened on
    a face with its eyes shut. Starting on the open stretch keeps the blink
    mid-loop and puts open eyes on idle-0."""
    ok = eyes_open(paths)
    n = len(paths)
    if all(ok) or not any(ok):
        return paths
    best, at = 0, 0
    for i in range(n):
        if ok[i] and not ok[i - 1]:
            run = 0
            while ok[(i + run) % n]:
                run += 1
            if run > best:
                best, at = run, i
    return paths[at:] + paths[:at]


def crossfade(paths, n, k=6):
    """n frames from n+k consecutive ones. The last k fade into the frames
    just before the loop's first, so the wrap is a real motion step and the
    loop point never shows."""
    c = [Image.open(p).convert("RGB") for p in paths]
    out = c[k:n]
    for j in range(k):
        out.append(Image.blend(c[n + j], c[j], (j + 1) / (k + 1)))
    return out


with tempfile.TemporaryDirectory() as work:
    picked = {}
    for kind, n in COUNT.items():
        subprocess.run(["ffmpeg", "-v", "error", "-y", "-i", f"{src}/{kind}.mp4",
                        "-vf", f"crop={crop},scale={SIDE}:{SIDE}:flags=lanczos,fps={FPS}", f"{work}/{kind}-%03d.png"], check=True)
        frames = sorted(glob.glob(f"{work}/{kind}-*.png"))
        if kind == "idle":
            sel = open_start(frames)[:n + 6]
        else:
            sel = seamless(frames, n + 6, open_eyes=True)
        picked[kind] = crossfade(sel, len(sel) - 6)
    for f in glob.glob(f"{out}/idle-*") + glob.glob(f"{out}/talk-*"):
        os.remove(f)
    os.makedirs(out, exist_ok=True)
    for kind, paths in picked.items():
        for i, im in enumerate(paths):
            dst = f"{out}/{kind}-{i}.jpg"
            im.save(dst, "JPEG", quality=88, progressive=False, subsampling=2)
        print(f"face_frames: {len(paths)} {kind} frames, {sum(os.path.getsize(f'{out}/{kind}-{i}.jpg') for i in range(len(paths))) // 1024}KB")
