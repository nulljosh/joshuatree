#!/usr/bin/env python3
"""Narration for the 2.0 ad: Samantha's ElevenLabs voice reads each line, music2.py (30 s cut) is the bed, and ffmpeg
places every line on the beat it belongs to, ducks the bed under the voice and writes mix.wav. Run from the repo root:
python3 tools/ad-audio.py /tmp/jt-ad-audio   (key is read from ~/.config/fish/secrets.fish and never printed)."""
import os, re, json, subprocess, sys, urllib.request
OUT = sys.argv[1] if len(sys.argv) > 1 else "/tmp/jt-ad-audio"; os.makedirs(OUT, exist_ok=True); os.chdir(OUT)
key = re.search(r"ELEVENLABS_API_KEY ['\"]?([^'\" \n]+)", open(os.path.expanduser("~/.config/fish/secrets.fish")).read()).group(1)
VOICE = "EXAVITQu4vr4xnSDxMaL"
# (id, start seconds, line)
LINES = [("a", 0.7, "This is Joshua Tree 2.0."), ("b", 3.9, "Six printed layers. Nothing hidden."),
 ("c", 8.6, "Inside, a real chip, running code we wrote from scratch."), ("d", 15.6, "Every app lives in its own protected box."),
 ("e", 21.0, "And this is the desktop. Running today."), ("f", 25.2, "Hi. I'm Samantha."), ("g", 27.0, "Joshua Tree 2.0. Build one yourself.")]
for k, _, t in LINES:
    req = urllib.request.Request(f"https://api.elevenlabs.io/v1/text-to-speech/{VOICE}?output_format=mp3_44100_128",
        data=json.dumps({"text": t, "model_id": "eleven_multilingual_v2", "voice_settings": {"stability": 0.55, "similarity_boost": 0.75, "style": 0.15}}).encode(),
        headers={"xi-api-key": key, "content-type": "application/json", "accept": "audio/mpeg"})
    open(f"{k}.mp3", "wb").write(urllib.request.urlopen(req, timeout=60).read())
src = open(os.path.join(os.environ.get("OLDPWD", "."), "docs/hardware/ad/music2.py")).read()
src = src.replace("SR, BPM, DUR = 44100, 120, 37.0", "SR, BPM, DUR = 44100, 120, 30.0").replace("END = 30.9", "END = 27.4")
open("music.py", "w").write(src); subprocess.run([sys.executable, "music.py"], check=True)
ins = sum([["-i", f"{k}.mp3"] for k, _, _ in LINES], []) + ["-i", "music2.wav"]
vo = "".join(f"[{i}]aformat=sample_rates=44100:channel_layouts=mono,adelay={int(s*1000)}[{k}];" for i, (k, s, _) in enumerate(LINES))
fc = (vo + "".join(f"[{k}]" for k, _, _ in LINES) + f"amix=inputs={len(LINES)}:normalize=0,loudnorm=I=-16:TP=-1.5:LRA=7,asplit[v1][v2];"
 f"[{len(LINES)}]aformat=sample_rates=44100:channel_layouts=mono,volume=0.5[m];[m][v2]sidechaincompress=threshold=0.02:ratio=6:attack=20:release=400[md];"
 "[md]volume=0.30[mq];[v1][mq]amix=inputs=2:normalize=0,alimiter=limit=0.9,atrim=0:30,aformat=channel_layouts=stereo[out]")
subprocess.run(["ffmpeg", "-v", "error", "-y", *ins, "-filter_complex", fc, "-map", "[out]", "-ar", "44100", "mix.wav"], check=True)
print("wrote", os.path.join(OUT, "mix.wav"))
