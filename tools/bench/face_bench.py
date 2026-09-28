#!/usr/bin/env python3
"""Grade a recording of Samantha talking. Numbers, not vibes.

Usage: uv run --with numpy --with pillow tools/bench/face_bench.py a.mp4 [b.mp4 ...]

Finds her face as the part of the frame that moves (works on a bare face
clip or a full-screen OS recording), then scores six things at 12fps:

  fluid   share of frames that actually change        (v1 was 3-5 stills)
  smooth  typical step vs the worst 5% of steps        (low = jumpy)
  pops    typical step vs the single worst step        (a loop seam or cut)
  steady  mouth motion vs head/eye motion when talking (low = head shakes)
  sync    how well mouth opening tracks her voice      (the big one)
  rest    mouth still when she's quiet vs talking      (low = chattering)

Each is 0-100; the grade is their average, with sync counted twice.
"""
import subprocess, sys
import numpy as np
from PIL import Image

FPS, SIDE = 12, 160


def frames(path):
    w, h = map(int, subprocess.run(["ffprobe", "-v", "error", "-select_streams", "v:0", "-show_entries",
        "stream=width,height", "-of", "csv=p=0", path], capture_output=True, text=True).stdout.split(",")[:2])
    raw = subprocess.run(["ffmpeg", "-v", "error", "-i", path, "-vf", f"fps={FPS}", "-f", "rawvideo",
        "-pix_fmt", "gray", "-"], capture_output=True).stdout
    return np.frombuffer(raw, np.uint8).reshape(-1, h, w).astype(np.float32)


def loudness(path, n):
    raw = subprocess.run(["ffmpeg", "-v", "error", "-i", path, "-vn", "-f", "s16le", "-ac", "1",
        "-ar", "12000", "-"], capture_output=True).stdout
    pcm = np.frombuffer(raw, np.int16).astype(np.float32)
    if len(pcm) < 1000:
        return np.zeros(n)
    L = np.array([np.sqrt((pcm[i * 1000:(i + 1) * 1000] ** 2).mean()) for i in range(min(n, len(pcm) // 1000))])
    return np.pad(L, (0, n - len(L)))


def face_crop(v):
    """Bounding square of the pixels that move, resized to SIDE."""
    motion = np.abs(np.diff(v, axis=0)).mean(0)
    ys, xs = np.nonzero(motion > max(2.0, np.percentile(motion, 99) * 0.2))
    y0, y1, x0, x1 = np.percentile(ys, 1), np.percentile(ys, 99), np.percentile(xs, 1), np.percentile(xs, 99)
    s = max(y1 - y0, x1 - x0); cy, cx = (y0 + y1) / 2, (x0 + x1) / 2
    box = tuple(int(round(t)) for t in (cx - s / 2, cy - s / 2, cx + s / 2, cy + s / 2))
    return np.stack([np.asarray(Image.fromarray(f).crop(box).resize((SIDE, SIDE))) for f in v.astype(np.uint8)]).astype(np.float32)


def score(path):
    v = frames(path)
    f = face_crop(v)
    n = len(f)
    L = loudness(path, n)
    talking = L > 0.15 * np.percentile(L, 95) if L.any() else np.ones(n, bool)
    m = (slice(int(SIDE * .58), int(SIDE * .76)), slice(int(SIDE * .36), int(SIDE * .64)))
    step = np.abs(np.diff(f, axis=0)).mean((1, 2))
    moving = step[step > 0.3]
    mouth_step = np.abs(np.diff(f[:, m[0], m[1]], axis=0)).mean((1, 2))
    rest_mask = np.ones((SIDE, SIDE), bool); rest_mask[m] = False
    head_step = np.abs(np.diff(f, axis=0))[:, rest_mask].mean(1)
    openness = f[:, m[0], m[1]].var((1, 2))
    t = talking[1:]
    r = {}
    r["fluid"] = 100 * len(moving) / max(1, n - 1)
    med = np.median(moving) if len(moving) else 1
    r["smooth"] = 100 * min(1, 2.0 * med / (np.percentile(moving, 95) + 1e-6)) if len(moving) else 0
    r["pops"] = 100 * min(1, 3.0 * med / (moving.max() + 1e-6)) if len(moving) else 0
    r["steady"] = 100 * min(1, (mouth_step[t].mean() / (head_step[t].mean() + 1e-6)) / 2.5) if t.any() else 0
    best = 0.0
    if L.any() and openness.std() > 0:
        for lag in range(0, 4):  # the face may trail the audio by up to 1/4 s
            a, b = L[:n - lag], openness[lag:]
            if a.std() > 0 and b.std() > 0:
                best = max(best, np.corrcoef(a, b)[0, 1])
    r["sync"] = 100 * min(1, max(0, best) / 0.6)
    quiet, loud = mouth_step[~t], mouth_step[t]
    r["rest"] = 100 * min(1, max(0, 1 - (quiet.mean() / (loud.mean() + 1e-6)))) if quiet.size and loud.size else 50
    total = (sum(r.values()) + r["sync"]) / (len(r) + 1)
    return r, total


def letter(x):
    for cut, g in ((93, "A+"), (85, "A"), (80, "A-"), (77, "B+"), (73, "B"), (70, "B-"), (67, "C+"), (63, "C"), (60, "C-"), (50, "D")):
        if x >= cut:
            return g
    return "F"


if __name__ == "__main__":
    print(f"{'video':32} fluid smooth pops steady sync rest  total")
    for p in sys.argv[1:]:
        r, t = score(p)
        print(f"{p.rsplit('/', 1)[-1][:32]:32} " + " ".join(f"{r[k]:5.0f}" for k in ("fluid", "smooth", "pops", "steady", "sync", "rest")) + f"  {t:4.0f} {letter(t)}")
