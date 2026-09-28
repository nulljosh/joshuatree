#!/usr/bin/env python3
"""Samantha's face loops wrap without a seam.

The kernel plays landing/face/idle-*.jpg and talk-*.jpg in a loop, so the
step from the last frame back to the first must look like any other step,
or her face visibly jumps every loop. Pure image math, no VM.

Not checked here: eyes open while talking. Idle and talk come from
different clips with different framing, so neither sets the other's open
level; tools/gen/face_frames.py picks open-eyed stretches when it cuts.
"""
import glob, os, re, sys
from PIL import Image, ImageChops

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
D = os.path.join(ROOT, "landing", "face")


def frames(kind):
    fs = sorted(glob.glob(f"{D}/{kind}-*.jpg"), key=lambda p: int(re.findall(r"(\d+)\.jpg$", p)[0]))
    return [Image.open(f).convert("L") for f in fs]


def steps(fs):
    small = [f.resize((40, 40)) for f in fs]
    d = lambda a, b: sum(ImageChops.difference(a, b).get_flattened_data()) / 1600
    inner = [d(small[i], small[i + 1]) for i in range(len(small) - 1)]
    return inner, d(small[-1], small[0])


talk, idle = frames("talk"), frames("idle")
if not talk or not idle:
    sys.exit(f"FAIL: no face frames in {D}")
fail = []
for kind, fs in (("idle", idle), ("talk", talk)):
    inner, wrap = steps(fs)
    worst = sorted(inner)[int(len(inner) * .95)]
    print(f"{kind}: wrap step {wrap:.2f}, 95th percentile step {worst:.2f}")
    if wrap > 1.5 * worst + 0.5:
        fail.append(f"{kind} loop has a visible seam: wrap step {wrap:.2f} vs {worst:.2f}")
if fail:
    sys.exit("FAIL: " + "; ".join(fail))
print("PASS: both face loops wrap without a seam")
