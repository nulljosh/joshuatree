#!/usr/bin/env python3
"""YouTube clips are the whole video (12.0, tools/claude-relay/yt.py fit()): the Mac fits a video into the bytes the Pi
can take by lowering the bitrate across its full length, not by cutting off the end.

On a generated video (ffmpeg's testsrc2 with a tone, so no network and no yt-dlp):
  1. 200 s into 9 MB: the clip holds all 200 seconds, is at most 9 MB, and is 256 wide (the budget is modest).
  2. 15 s into 12 MB: all 15 seconds, 320 wide (Movies' most, the budget is generous).
  3. 200 s into 2 MB: too little for watchable motion JPEG, so it is cut short, and still at most 2 MB.
Discriminating: go back to a fixed -q:v 9 at 256 wide and step 1 comes back cut short.
Skips (exit 0) without ffmpeg and ffprobe.
Usage: tools/checks/yt-fit-check.py   (from the repo root)
"""
import os, shutil, subprocess, sys
from pathlib import Path

root = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(root / "tools/claude-relay"))
sys.path.insert(0, str(Path(__file__).resolve().parent))
from scratch import scratch_dir
if not (shutil.which("ffmpeg") and shutil.which("ffprobe")):
    print("SKIP: ffmpeg or ffprobe not installed"); sys.exit(0)
import yt

tmp = Path(scratch_dir("yt-fit"))
fails = []
def check(name, ok, detail=""):
    print(("  ok: " if ok else "  FAIL: ") + name + ("" if ok else "  " + repr(detail)[-300:]))
    if not ok: fails.append(name)

def video(name, seconds):
    out = tmp / name
    subprocess.run(["ffmpeg", "-loglevel", "error", "-y", "-f", "lavfi", "-i", "testsrc2=size=640x360:rate=24:duration=%d" % seconds,
                    "-f", "lavfi", "-i", "sine=frequency=440:duration=%d" % seconds, "-c:v", "mpeg4", "-q:v", "5",
                    "-c:a", "aac", "-shortest", str(out)], check=True, capture_output=True)
    return str(out)
def width(data):
    p = tmp / "clip.avi"; p.write_bytes(data)
    r = subprocess.run(["ffprobe", "-v", "error", "-select_streams", "v", "-show_entries", "stream=width", "-of", "csv=p=0", str(p)],
                       capture_output=True, text=True)
    return int(r.stdout.strip() or 0)

MB = 1 << 20
long_, short = video("long.mp4", 200), video("short.mp4", 15)
data, secs = yt.fit(long_, 9 * MB)
check("200 s into 9 MB keeps all 200 seconds", secs >= 199, secs)
check("and stays at most 9 MB, 256 wide", len(data) <= 9 * MB and width(data) == 256, (len(data), width(data)))
data, secs = yt.fit(short, 12 * MB)
check("15 s into 12 MB keeps all 15 seconds, 320 wide", secs >= 14.9 and len(data) <= 12 * MB and width(data) == 320, (secs, len(data), width(data)))
data, secs = yt.fit(long_, 2 * MB)
check("200 s into 2 MB is cut short instead of turning to mush, at most 2 MB", secs < 199 and len(data) <= 2 * MB, (secs, len(data)))

if fails: print("FAIL: " + "; ".join(fails)); sys.exit(1)
print("PASS: a YouTube clip is the whole video at the bitrate the Pi's bytes allow, 320 wide when generous, cut only when the bitrate would be too low")
