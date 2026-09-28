#!/usr/bin/env python3
"""Grade a recording of Samantha talking. Numbers, not vibes.

Usage: uv run --with numpy --with pillow tools/bench/face_bench.py [--align words.json] a.mp4 [b.mp4 ...]

Finds her face as the part of the frame that moves (works on a bare face
clip or a full-screen OS recording), then scores six things at 12fps:

  fluid   share of frames that actually change        (v1 was 3-5 stills)
  smooth  typical step vs the worst 5% of steps        (low = jumpy)
  pops    typical step vs the single worst step        (a loop seam or cut)
  steady  mouth motion vs head/eye motion when talking (low = head shakes)
  sync    how well mouth opening tracks her voice      (the big one)
  rest    mouth still when she's quiet vs talking      (low = chattering)
  eyes    eyes open while she talks                    (low = blinking all the time)
  alive   some head and shoulder motion                (0 = a frozen photo)
  head    head drift vs a real talker (NASA interview close-up: ~4.2% of frame
          height std). Ours was 0: a still photo with a moving mouth.
  whole   the whole face moves together, not just the mouth (real: lower/upper
          face motion 1.3; a pasted mouth reads 3)
  words   with --align (ElevenLabs with-timestamps JSON for the audio in
          the video): lips shut on m/b/p and silence, open on vowels.
          Loudness can't tell "mm" from "ah"; this can.

Each is 0-100; the grade is their average, with sync (or words, when
given) counted twice.
"""
import json, subprocess, sys
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
    H, W = v.shape[1:]
    if s < 0.5 * min(H, W):   # only the mouth moves (a steady face clip): the face is the centered square
        s, cy, cx = min(H, W), H / 2, W / 2
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
    # eyes: dark iris pixels in the eye band, relative to this clip's open level
    band = f[:, int(SIDE * .33):int(SIDE * .42), int(SIDE * .28):int(SIDE * .72)]
    dark = (band < 70).mean((1, 2))
    shut = dark < .85 * np.percentile(dark, 90)
    r["eyes"] = 100 * (1 - shut[talking].mean()) if talking.any() else 100
    # alive: motion outside the mouth, in a band that reads as breathing (not shaking)
    hm = head_step.mean()
    r["alive"] = 100 * min(1, hm / 0.8) if hm < 4 else 100 * max(0, 1 - (hm - 4) / 4)
    # head: global drift of the face, by phase correlation against frame 0
    def shift(a, b):
        A = np.fft.fft2(a - a.mean()); B = np.fft.fft2(b - b.mean()); R = A * np.conj(B); R /= np.abs(R) + 1e-9
        y, x = np.unravel_index(np.fft.ifft2(R).real.argmax(), R.shape)
        return (y if y < SIDE // 2 else y - SIDE, x if x < SIDE // 2 else x - SIDE)
    sh = np.array([shift(fr, f[0]) for fr in f], float)
    drift_pct = 100 * sh.std(0).mean() / SIDE
    r["head"] = 100 * min(1, drift_pct / 4.2 / 0.6)        # 60% of a lively interviewee is plenty
    d = np.abs(np.diff(f, axis=0))
    ratio = d[:, int(SIDE * .62):int(SIDE * .94)].mean() / (d[:, int(SIDE * .16):int(SIDE * .47)].mean() + 1e-9)
    r["whole"] = 100 * max(0, 1 - abs(ratio - 1.3) / 1.7)
    key = "sync"
    if ALIGN:
        a = json.load(open(ALIGN))["alignment"]
        span = list(zip(a["character_start_times_seconds"], a["character_end_times_seconds"], a["characters"]))
        def want(t):
            for s0, e0, c in span:
                if s0 <= t < e0:
                    c = c.lower()
                    return 0 if c in "mbp .,!?" else 1 if c in "aeiouhy" else None
            return 0
        w = np.array([want(i / FPS) for i in range(n)], dtype=object)
        idx = [i for i in range(n) if w[i] is not None]
        if len(idx) > 4:
            wv = np.array([w[i] for i in idx], float); ov = openness[idx]
            if wv.std() > 0 and ov.std() > 0:
                r["words"] = 100 * min(1, max(0, np.corrcoef(wv, ov)[0, 1]) / 0.5)
                key = "words"
    # Grade = how human it looks. Calibrated on a real person (NASA interview
    # close-up): a real talker blinks, moves the head, keeps the mouth busy in
    # short pauses and has hard shot-to-shot jumps, so "eyes", "rest",
    # "steady" and "pops" are diagnostics, not the grade. The graded signals
    # are the ones where real video and our failures actually differ.
    graded = ["fluid", "smooth", "alive", "head", "whole", key]
    total = (sum(r[g] for g in graded) + r[key]) / (len(graded) + 1)
    return r, total


def letter(x):
    for cut, g in ((93, "A+"), (85, "A"), (80, "A-"), (77, "B+"), (73, "B"), (70, "B-"), (67, "C+"), (63, "C"), (60, "C-"), (50, "D")):
        if x >= cut:
            return g
    return "F"


ALIGN = None

if __name__ == "__main__":
    args = sys.argv[1:]
    if args[:1] == ["--align"]:
        ALIGN, args = args[1], args[2:]
    cols = ("fluid", "smooth", "pops", "steady", "sync", "rest", "eyes", "alive", "head", "whole") + (("words",) if ALIGN else ())
    print(f"{'video':28} " + " ".join(f"{c:>6}" for c in cols) + "  total")
    for p in args:
        r, t = score(p)
        print(f"{p.rsplit('/', 1)[-1][:28]:28} " + " ".join(f"{r.get(k, 0):6.0f}" for k in cols) + f"  {t:4.0f} {letter(t)}")
