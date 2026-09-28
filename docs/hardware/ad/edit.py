# Cut the ad: Blender shots + live OS footage + type cards, Samantha's VO over a soft bed.
import subprocess, glob, os
os.chdir(os.path.dirname(os.path.abspath(__file__)))
WEBM = glob.glob("os/*.webm")[0]
FONT = "/System/Library/Fonts/SFNS.ttf"  # San Francisco
BG = "0xF4EEE3"; INK = "0x1A1814"; SOFT = "0x8A8378"
VO_AT = 0.8
FPS = 24
V = "scale=1920:1080:flags=lanczos,setsar=1,fps=24,format=yuv420p"
OS_CROP = "crop=1298:730:151:14,delogo=x=1170:y=684:w=122:h=42"  # hides the landing page's Full screen button

def text_png(text, size, color, out):
    # ponytail: this ffmpeg build has no drawtext, so type is set by ImageMagick and overlaid
    subprocess.run(["magick", "-background", "none", "-fill", "#" + color[2:], "-font", FONT, "-pointsize", str(size),
                    f"label:{text}", out], check=True)
    return out
os.makedirs("cut", exist_ok=True)
CAP = text_png("Strata enclosure. Concept render.", 22, SOFT, "cut/cap.png")

def run(args): subprocess.run(["ffmpeg", "-v", "error", "-y", *args], check=True)

def shot(name, dur, out, caption=True):
    n = len(glob.glob(f"shots/{name}/*.png"))
    speed = n / FPS / dur  # stretch or squeeze the render to the slot
    fc = f"[0:v]setpts=PTS/{speed:.4f},{V},trim=duration={dur}"
    run(["-framerate", str(FPS), "-i", f"shots/{name}/%04d.png", "-filter_complex", fc, "-an", "-c:v", "libx264", "-crf", "16", out])

def footage(t, dur, out):
    # cover the landing page's Full screen button with the wallpaper just left of it
    fc = ("[0:v]crop=1298:730:151:14,split[a][b];[b]crop=130:56:1036:674[p];"
          f"[a][p]overlay=1166:674,{V}")
    run(["-ss", str(t), "-t", str(dur), "-i", WEBM, "-filter_complex", fc, "-an", "-c:v", "libx264", "-crf", "16", out])

CLAY = "0xB9542C"
subprocess.run(["magick", "-size", "1600x900", "xc:black", "-fill", "white", "-draw", "roundrectangle 0,0 1599,899 30,30", "cut/mask.png"], check=True)
def frame(raw, dur, bg, cap, out):
    # Anthropic-release look: product inset in a rounded frame on a flat colour field, small lower-third caption
    capp = text_png(cap, 26, "0xF4EEE3" if bg == CLAY else SOFT, f"cut/cap-{abs(hash(cap))}.png")
    fc = ("[0:v]scale=1600:900,format=rgba[v];[1:v]format=gray[m];[v][m]alphamerge[r];"
          "[2:v][r]overlay=160:52[a];[a][3:v]overlay=160:H-96,"
          f"fade=t=in:st=0:d=0.2,fade=t=out:st={dur - 0.2}:d=0.2,format=yuv420p")
    run(["-i", raw, "-loop", "1", "-framerate", str(FPS), "-i", "cut/mask.png", "-f", "lavfi", "-i", f"color=c={bg}:s=1920x1080:r={FPS}",
         "-loop", "1", "-framerate", str(FPS), "-i", capp, "-filter_complex", fc, "-t", str(dur), "-an", "-c:v", "libx264", "-crf", "16", out])

def card(text, dur, out, size=120):
    t = text_png(text, size, INK, f"cut/card-{text.strip('.')}.png")
    fc = (f"[0:v][1:v]overlay=(W-w)/2:(H-h)/2,fade=t=in:st=0:d=0.25,fade=t=out:st={dur - 0.25}:d=0.25,format=yuv420p")
    run(["-f", "lavfi", "-i", f"color=c={BG}:s=1920x1080:r={FPS}:d={dur}", "-i", t, "-filter_complex", fc,
         "-c:v", "libx264", "-crf", "16", out])

def end(dur, out):
    title = text_png("Joshua Tree", 96, INK, "cut/title.png")
    sub = text_png("Join the waitlist.  joshuatree.heyitsmejosh.com", 34, SOFT, "cut/sub.png")
    fc = (f"[0:v]scale=1998:1124,crop=1920:1080:39:'t*4',fps={FPS}[bg];"  # slow drift up
          f"[1:v]format=rgba,fade=t=in:st=0.4:d=0.6:alpha=1[t];[2:v]format=rgba,fade=t=in:st=1.8:d=0.6:alpha=1[s];"
          f"[bg][t]overlay=(W-w)/2:96[a];[a][s]overlay=(W-w)/2:218,trim=duration={dur},fade=t=out:st={dur - 1.2}:d=1.2,format=yuv420p")
    run(["-loop", "1", "-framerate", str(FPS), "-i", "shots/end/0000.png", "-loop", "1", "-framerate", str(FPS), "-i", title,
         "-loop", "1", "-framerate", str(FPS), "-i", sub, "-filter_complex", fc, "-t", str(dur), "-an", "-c:v", "libx264", "-crf", "16", out])

# (start, kind, args). Slot lengths come from the VO timings in ad-align.json plus VO_AT.
EDL = [
    (3.1,  "shot",    ("hero",)),
    (3.9,  "footage", (1.3,)),     # boot splash into the desktop: "written from scratch"
    (1.0,  "footage", (77.5,)),    # terminal: "The kernel."
    (1.0,  "footage", (12.5,)),    # Mail compose over Files: "The windows."
    (1.8,  "footage", (58.0,)),    # Notes: "The apps."
    (1.4,  "card",    ("Strata.",)),
    (3.3,  "shot",    ("turn",)),
    (2.3,  "shot",    ("explode",)),
    (4.7,  "shot",    ("mark",)),
    (0.8,  "footage", (12.5,)),    # Mail.
    (0.8,  "footage", (58.5,)),    # Notes.
    (0.9,  "footage", (47.5,)),    # Weather.
    (2.2,  "footage", (37.5,)),    # Twenty seven apps in all.
    (3.7,  "footage", (90.3,)),    # And me. Hi. I'm Samantha.
    (5.6,  "end",     ()),
]
parts = []
CAPS = {"hero": "Introducing Joshua Tree", "turn": "Strata enclosure. Concept render.", "explode": "Six layers. Two millimetre gaps.", "mark": "The tree, engraved into the lid."}
for i, (dur, kind, a) in enumerate(EDL):
    out = f"cut/{i:02d}.mp4"; raw = f"cut/{i:02d}-raw.mp4"
    if kind == "shot": shot(a[0], dur, raw); frame(raw, dur, CLAY, CAPS[a[0]], out)
    elif kind == "footage": footage(a[0], dur, raw); frame(raw, dur, BG, "Joshua Tree OS. Live, in a browser.", out)
    elif kind == "card": card(a[0], dur, out)
    else: end(dur, out)
    parts.append(out)
open("cut/list.txt", "w").write("".join(f"file '{os.path.basename(p)}'\n" for p in parts))
run(["-f", "concat", "-safe", "0", "-i", "cut/list.txt", "-c", "copy", "cut/picture.mp4"])
total = sum(d for d, _, _ in EDL)
run(["-i", "cut/picture.mp4", "-i", "ad.mp3", "-i", "music2.wav", "-filter_complex",
     f"[1:a]adelay={int(VO_AT * 1000)}:all=1,aformat=channel_layouts=stereo,asplit[vo][vo2];"
     f"[2:a]asplit[lo][hi];[lo]lowpass=f=220,aformat=channel_layouts=stereo[l];"
     f"[hi]highpass=f=220,aformat=channel_layouts=stereo,adelay=0|14,aecho=0.8:0.5:90:0.18[h];"  # bass centred, top end widened
     f"[l][h]amix=inputs=2:normalize=0,equalizer=f=90:t=q:w=1:g=-4,equalizer=f=2200:t=q:w=1:g=4,volume=0.75,afade=t=out:st={total - 2}:d=2[m];"
     f"[m][vo]sidechaincompress=threshold=0.1:ratio=1.6:attack=40:release=600[duck];"
     f"[duck][vo2]amix=inputs=2:normalize=0,loudnorm=I=-16:TP=-1.5[a]",
     "-map", "0:v", "-map", "[a]", "-c:v", "copy", "-c:a", "aac", "-b:a", "192k", "-t", str(total), "-movflags", "+faststart",
     "joshua-tree-ad.mp4"])
print("AD", total, "s")
