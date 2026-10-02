#!/usr/bin/env python3
"""Animate Samantha's mouth for any sentence from one lip-synced clip.

Usage: uv run --with numpy --with pillow --with opencv-python-headless tools/gen/face_visemes.py \
         <lipsynced-talk.mp4> <words.json> <speech.wav> <out.mp4>

words.json is ElevenLabs with-timestamps output for speech.wav. The clip is
any lip-synced render of her (character-creator's lipsync.sh). Every frame
of it gets measured: how open her mouth is, how wide. Each sound in the
sentence has a target shape (lips shut on m/b/p, wide on ee, round on oo,
open on ah). Each output frame shows the real frame closest to that target
that's also close to the frame on screen, blended a little into the next.

Measured shapes, not letter labels: labeling render frames by the letter
being spoken was noisy (face_bench words 34); measuring the mouth got 75,
better than the render itself, on a sentence she never rendered (v23).
"""
import glob, json, os, subprocess, sys, tempfile
import numpy as np
from PIL import Image

FPS = 25
JUMP = 0.3   # how much a cut costs vs a slightly wrong mouth
# (open, wide) as ranks within the clip, 0..1: "aa" asks for the most open
# frames the render has. Absolute targets left her mumbling (Joshua: C+ on v23).
SHAPE = {"mbp": (0, .4), "rest": (.08, .45), "fv": (.2, .5), "tt": (.4, .55),
         "ee": (.62, .95), "aa": (1, .7), "oo": (.78, .05)}
SOUND = {**{c: "mbp" for c in "mbp"}, **{c: "fv" for c in "fv"}, **{c: "oo" for c in "ouwq"},
         **{c: "aa" for c in "ah"}, **{c: "ee" for c in "eiy"}, **{c: "tt" for c in "sztdnlckgjrx"}}


def sound_at(span, t):
    for s, e, c in span:
        if s <= t < e:
            return SOUND.get(c.lower(), "rest")
    return "rest"


def main(clip, words, wav, out):
    a = json.load(open(words))["alignment"]
    span = list(zip(a["character_start_times_seconds"], a["character_end_times_seconds"], a["characters"]))
    with tempfile.TemporaryDirectory() as w:
        subprocess.run(["ffmpeg", "-v", "error", "-i", clip, "-vf",
                        f"crop=ih:ih:(iw-ih)/2:0,scale=320:320,fps={FPS}", f"{w}/s-%04d.png"], check=True)
        imgs = [Image.open(f).convert("RGB") for f in sorted(glob.glob(f"{w}/s-*.png"))]
        boxes = [np.asarray(im.convert("L"), float)[180:245, 110:210] for im in imgs]   # mouth box at 320px
        # One dark threshold for the whole clip, not per frame. A per-frame
        # percentile always marks 12% of the box "dark", so counting the rows
        # they span read Joshua's open mouth (dark pixels bunched inside it) as
        # closed and his closed mouth (shadows scattered over the box) as open:
        # the whole plan came out inverted (face_bench words 0). The dark area
        # under one clip-wide threshold grows as the mouth opens, on any face.
        thr = np.percentile(np.stack(boxes), 8)
        op, wd = [], []
        for g in boxes:
            d = g < thr
            op.append(d.sum()); wd.append(d.any(0).sum())
        if os.environ.get("FV_DEBUG"):
            print("debug open-area: most open frame %d, most closed frame %d" % (int(np.argmax(op)), int(np.argmin(op))))
        op, wd = (np.array(v, float) for v in (op, wd))
        rank = lambda v: v.argsort().argsort() / max(1, len(v) - 1)
        op, wd = rank(op), rank(wd)
        small = [np.asarray(im.convert("L").resize((48, 48)), float) for im in imgs]
        # her voice's loudness per frame, as a rank: loud syllables open the mouth
        pcm = np.frombuffer(subprocess.run(["ffmpeg", "-v", "error", "-i", wav, "-f", "s16le", "-ac", "1",
                            "-ar", "12000", "-"], capture_output=True).stdout, np.int16).astype(float)
        hop = 12000 // FPS
        loud = np.array([np.sqrt((pcm[i:i + hop] ** 2).mean()) for i in range(0, len(pcm) - hop, hop)])
        loud = np.clip(loud / (np.percentile(loud, 90) + 1e-9), 0, 1)
        # Plan the whole sentence at once (Viterbi): each output frame pays for
        # how far its mouth is from the sound's target, and every cut to a
        # non-consecutive render frame pays extra. So it plays real runs of
        # the render and only jumps when the words need it. Frame-by-frame
        # picking jumped every frame and read as "photos stapled together".
        N, T = len(imgs), int((span[-1][1] + 0.3) * FPS)
        tgt = []
        for k in range(T):
            snd = sound_at(span, k / FPS)
            to, tw = SHAPE[snd]
            if snd != "mbp":   # lips still shut on m/b/p however loud
                to = 0.45 * to + 0.55 * (loud[k] if k < len(loud) else 0)
            tgt.append((to, tw))
        # Ease targets over ~80ms: real speech blends each sound into the next.
        k5 = np.array([1, 4, 6, 4, 1], float) / 16
        tgt = list(zip(*(np.convolve(np.pad([t[i] for t in tgt], 2, mode="edge"), k5, "valid") for i in (0, 1))))
        feat = np.stack([np.asarray(im.convert("L").resize((32, 32)), float).ravel() for im in imgs])
        nxt = np.roll(feat, -1, 0)                                  # what naturally follows each frame
        jump = ((nxt[:, None, :] - feat[None, :, :]) ** 2).mean(2)  # [from i, to j] cost of a cut
        jump = JUMP + jump / jump.mean()
        idx = np.arange(N)
        jump[idx, (idx + 1) % N] = 0                                # playing on is free
        unit = lambda k: 4 * (op - tgt[k][0]) ** 2 + (wd - tgt[k][1]) ** 2
        cost, back = unit(0), []
        for k in range(1, T):
            tot = cost[:, None] + jump
            b = tot.argmin(0); back.append(b)
            cost = tot[b, idx] + unit(k)
        path = [int(cost.argmin())]
        for b in reversed(back):
            path.append(int(b[path[-1]]))
        path.reverse()
        if os.environ.get("FV_DEBUG"):
            tv = np.array([t[0] for t in tgt]); pv = op[path]
            print("debug corr(target open, chosen open)=%.2f cuts=%d/%d" % (np.corrcoef(tv, pv)[0, 1], sum(1 for k in range(1, T) if path[k] != (path[k-1]+1) % N), T))
        # Composite: a continuous base (the render playing straight through,
        # so eyes, hair and breathing never cut) with only the mouth patch
        # swapped in from the planned frame, nudged a few pixels to line up
        # and feathered in. Whole-frame cuts read as "photos stapled
        # together" (v26, Joshua: C) because hair and eyes jumped with them.
        yy, xx = np.mgrid[0:320, 0:320]
        ell = ((xx - 160) / 62.0) ** 2 + ((yy - 226) / 52.0) ** 2   # lips and chin: the jaw drops with the vowel
        mask = np.clip((1.35 - ell) / 0.5, 0, 1)[..., None]          # soft ellipse over mouth and lips
        ring = (ell > 0.9) & (ell < 1.6)                               # skin around it, for alignment
        arr = [np.asarray(im, float) for im in imgs]
        gray = [a.mean(2) for a in arr]
        patches = []
        for k, j in enumerate(path):
            best, bd = (0, 0), None
            for dy in range(-4, 5):
                for dx in range(-4, 5):
                    d = np.abs(np.roll(gray[j], (dy, dx), (0, 1))[ring] - gray[k % N][ring]).mean()
                    if bd is None or d < bd:
                        bd, best = d, (dy, dx)
            patches.append(np.roll(arr[j], best, (0, 1)))
        # Morph across each cut: optical flow from the shape before to the
        # shape after, and real in-between shapes on the frames around it.
        # A crossfade or a hard cut both read as "still photos" (v27: B).
        import cv2
        dis = cv2.DISOpticalFlow_create(cv2.DISOPTICAL_FLOW_PRESET_MEDIUM)
        Y0, Y1, X0, X1 = 150, 280, 80, 240
        gy, gx = np.mgrid[Y0:Y1, X0:X1].astype(np.float32)
        morphed = [p.copy() for p in patches]
        cuts = [k for k in range(2, len(path) - 2) if path[k] != (path[k - 1] + 1) % N]
        for c in cuts:
            A, B = patches[c - 2], patches[c + 1]
            ga = cv2.cvtColor(A[Y0:Y1, X0:X1].astype(np.uint8), cv2.COLOR_RGB2GRAY)
            gb = cv2.cvtColor(B[Y0:Y1, X0:X1].astype(np.uint8), cv2.COLOR_RGB2GRAY)
            f = dis.calc(ga, gb, None)
            for k, t in ((c - 1, 1 / 3), (c, 2 / 3)):
                wa = cv2.remap(A.astype(np.float32), gx - t * f[..., 0], gy - t * f[..., 1], cv2.INTER_LINEAR)
                wb = cv2.remap(B.astype(np.float32), gx + (1 - t) * f[..., 0], gy + (1 - t) * f[..., 1], cv2.INTER_LINEAR)
                morphed[k][Y0:Y1, X0:X1] = (1 - t) * wa + t * wb
        # A real talker's head never stops: measured on a NASA interview
        # close-up, about 4% of frame height of drift in each direction, and
        # the whole face moves together. Ours had 0 (a frozen photo with a
        # moving mouth). Add a slow sway plus a small nod on each stressed
        # syllable (loudness peak), and a slight tilt, on a zoomed frame so
        # the edges never show.
        # Slow overlapping waves, not noise: smoothed random walks still
        # wobbled frame to frame (v29, Joshua: "janky").
        rng = np.random.default_rng(7)
        tt = np.arange(T) / FPS
        def sway(amp, *hz):
            ph = rng.uniform(0, 2 * np.pi, len(hz))
            return amp * sum(w * np.sin(2 * np.pi * f * tt + p0) for (f, w), p0 in zip(hz, ph))
        sx = sway(9.0, (0.17, .6), (0.31, .4))
        sy = sway(6.0, (0.13, .6), (0.27, .4))
        rot = sway(1.3, (0.11, .7), (0.23, .3))
        lf = np.array([loud[k] if k < len(loud) else 0 for k in range(T)])
        # Move with the voice: livelier while she speaks, settling in pauses
        # (a real talker's head motion rides the speech energy).
        env = np.convolve(lf, np.ones(int(0.6 * FPS)) / int(0.6 * FPS), "same")
        env = 0.45 + 0.9 * env / (env.max() + 1e-9)
        sx, sy, rot = sx * env, sy * env, rot * env
        peaks, last = [], -99
        for k in range(3, T - 3):
            if lf[k] > 0.75 and lf[k] == lf[k - 3:k + 4].max() and k - last >= int(0.6 * FPS):
                peaks.append(k); last = k
        nod = np.zeros(T)
        L = int(0.5 * FPS)   # a nod: down and back up over half a second, raised-cosine
        for k in peaks:
            for d in range(L):
                if k + d < T:
                    nod[k + d] += 3 * 0.5 * (1 - np.cos(2 * np.pi * d / L))
        # Blinks. Keeping her eyes open (v18) left her never blinking, which
        # reads as a stare. Real people blink every 3 to 5 s, about 0.25 s
        # long. BLINK_CLIP is the same render before its eyes were fixed, so
        # its frames line up; take its first close and reopen.
        blink_frames = []
        bc = os.environ.get("BLINK_CLIP")
        if bc:
            subprocess.run(["ffmpeg", "-v", "error", "-i", bc, "-vf", f"crop=ih:ih:(iw-ih)/2:0,scale=320:320,fps={FPS}",
                            f"{w}/b-%04d.png"], check=True)
            bf = [np.asarray(Image.open(f).convert("RGB"), float) for f in sorted(glob.glob(f"{w}/b-*.png"))]
            dark = np.array([(b.mean(2)[105:135, 90:230] < 70).mean() for b in bf])
            shut = dark < .85 * np.percentile(dark, 90)
            c = int(np.argmax(shut)) if shut.any() else -1
            if c > 3:
                o = c + int(np.argmin(shut[c:])) if not shut[c:].all() else -1
                if o > c:
                    blink_frames = bf[c - 3:c + 1] + bf[o:o + 3]      # closing, then opening
        ey = np.clip(np.minimum((yy - 88) / 10.0, (150 - yy) / 10.0), 0, 1) * np.clip(np.minimum((xx - 70) / 12.0, (250 - xx) / 12.0), 0, 1)
        emask = ey[..., None]
        blink_at = {}
        if blink_frames:
            t = int(FPS * rng.uniform(1.0, 2.5))
            while t < T - len(blink_frames):
                for d, fr in enumerate(blink_frames):
                    blink_at[t + d] = fr
                t += int(FPS * rng.uniform(3.0, 5.0))
        for k in range(len(path)):
            base = arr[k % N]
            if k in blink_at:
                base = base * (1 - emask) + blink_at[k] * emask
            im = Image.fromarray((base * (1 - mask) + morphed[k] * mask).astype(np.uint8))
            im = im.rotate(rot[k], resample=Image.BICUBIC, center=(160, 200),
                           translate=(sx[k], sy[k] + nod[k]))
            z = 1.16  # zoom so moving never shows an edge
            im = im.resize((int(320 * z), int(320 * z)), Image.BICUBIC).crop((26, 26, 346, 346))
            im.save(f"{w}/o-{k:04d}.png")
        subprocess.run(["ffmpeg", "-v", "error", "-y", "-framerate", str(FPS), "-i", f"{w}/o-%04d.png", "-i", wav,
                        "-c:v", "libx264", "-pix_fmt", "yuv420p", "-c:a", "aac", "-shortest", out], check=True)
    print("face_visemes:", out)


if __name__ == "__main__":
    if len(sys.argv) != 5:
        sys.exit(__doc__)
    main(*sys.argv[1:])
