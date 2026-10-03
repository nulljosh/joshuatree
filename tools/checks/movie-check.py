#!/usr/bin/env python3
"""Movies plays a real motion-JPEG AVI with sound, audio-led, at ring 3 (2.2).

Boots headless (-display none, never a window) with `open=movi`, a FAT disk that holds four clips
(tools/media/avigen.py --movie: a 2 s 20 fps clip whose frame number is drawn as six black/white
squares, a clip with noise for frames, a non-AVI, and one over the 6 MB cap) and an SB16 wired to
QEMU's wav backend. It drives the app through QMP keys and clicks and reads the app's own serial
markers, then checks them against things the app does not control:

  1. play: frame lines start at 0, never go backwards, and none is more than one frame away from
     what the audio clock (the `played` sample count printed beside it) says is due;
  2. drift: the audio clock against the host's wall clock over the whole clip stays under 100 ms,
     and the clip ends close to 2 s after it started;
  3. pixels: with the clip paused, the frame code read off the real framebuffer equals the frame the
     app said it paused on (and mid-play it is within a few frames of the last marker);
  4. pause: no frames while paused, the clock resumes where it stopped (not 1.2 s later);
  5. seek: a click on the middle of the bar lands within 0.5 s of 1.0 s; left at the start clamps
     to 0; right at the end clamps to the end; the picture follows (pixels again);
  6. fullscreen: `f` makes the picture fill the window height, `f` again gives the controls back;
  7. sound: the wav QEMU wrote holds a 440 Hz tone for at least the length of one full play;
  8. damaged clip, a non-AVI and an over-cap file each show an error (a serial line, not a crash)
     and the list comes back; the desktop is alive afterwards (Esc closes, Mail opens from the dock).

Every wait has a deadline. Usage: tools/checks/movie-check.py (from the repo root, after make kernel.elf).
"""
import json, math, os, re, socket, struct, subprocess, sys, tempfile, threading, time
from PIL import Image
from freeport import free_port

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
os.chdir(ROOT)
subprocess.run(["make", "-s", "kernel.elf"], check=True, stdout=subprocess.DEVNULL)

WORK = tempfile.mkdtemp(prefix="jt-movie-")
LOG = os.path.join(WORK, "serial.log")
DUMP = os.path.join(WORK, "fb.raw")
WAV = os.path.join(WORK, "out.wav")
DISK = os.path.join(WORK, "disk.img")
FB = 0xfd000000; W, H = 1920, 1080
PORT = free_port()
LOGICAL_W, LOGICAL_H, SCALE = 960, 540, 2
DOCK_ICON, DOCK_GAP, SLOT0_X = 37, 6, 247
PITCH = DOCK_ICON + DOCK_GAP
ICON_ROW_Y = 487
CLOSE_X, CLOSE_Y = 94, 56
CLOSE_RED = (0xFF, 0x5F, 0x57)
VIEW_X, VIEW_Y = 78, 72      # the app viewport's origin on the logical screen (dock launch)
WIN_W, WIN_H = 804, 345
PARK = (480, 200)
US, RATE, NFRAMES = 50000, 11025, 40

# ---- fixtures: fresh FAT disk with the clips on it ----
fx = os.path.join(WORK, "fx")
subprocess.run([sys.executable, "tools/media/avigen.py", fx, "--movie"], check=True)
subprocess.run(["./tools/mkdisk.sh", DISK], check=True, stdout=subprocess.DEVNULL)
for name in sorted(os.listdir(fx)):
    subprocess.run(["mcopy", "-i", DISK, os.path.join(fx, name), "::" + name], check=True)

q = subprocess.Popen(["qemu-system-i386", "-kernel", "kernel.elf", "-append", "open=movi",
                      "-display", "none", "-vga", "std",
                      "-qmp", f"tcp:127.0.0.1:{PORT},server,nowait", "-serial", "file:" + LOG,
                      "-drive", f"file={DISK},format=raw,if=ide,index=0",
                      "-audiodev", f"wav,id=snd,path={WAV}", "-device", "sb16,audiodev=snd"],
                     stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
fails = []

# ---- a serial tail that stamps every line with the host's monotonic clock ----
lines = []          # (t, text)
_stop = False
def tail():
    pos = 0; buf = b""
    while not _stop:
        try:
            with open(LOG, "rb") as f:
                f.seek(pos); data = f.read(); pos += len(data)
        except OSError:
            data = b""
        if data:
            now = time.monotonic(); buf += data
            while b"\n" in buf:
                ln, buf = buf.split(b"\n", 1)
                txt = ln.decode(errors="replace").strip()
                lines.append((now, txt.replace("syscall: write(1) from ring 3: ", "", 1)))   # the kernel logs each write() from the app with this prefix
        time.sleep(0.004)
threading.Thread(target=tail, daemon=True).start()

def serial(): return "\n".join(t for _, t in lines)
def wait_for(pred, secs, start=0):
    """First line index >= start whose text satisfies pred, waiting up to secs."""
    end = time.monotonic() + secs; i = start
    while time.monotonic() < end:
        while i < len(lines):
            if pred(lines[i][1]): return i
            i += 1
        time.sleep(0.01)
    return -1
def has(sub): return lambda t: sub in t
FRAME = re.compile(r"movie=frame (\d+) ms=(\d+) played=(\d+)")
def frames_from(start, stop=None):
    out = []
    for t, txt in lines[start:stop]:
        m = FRAME.search(txt)
        if m: out.append((t, int(m.group(1)), int(m.group(2)), int(m.group(3))))
    return out
def last_frame():
    fr = frames_from(0)
    return fr[-1] if fr else None

try:
    s = None
    for _ in range(50):
        time.sleep(0.2)
        try: s = socket.create_connection(("127.0.0.1", PORT)); break
        except OSError: pass
    if s is None: raise SystemExit("FAIL: QEMU's QMP socket never came up")
    f = s.makefile("rw")
    def cmd(o):
        f.write(json.dumps(o) + "\n"); f.flush()
        while True:
            r = json.loads(f.readline())
            if "return" in r or "error" in r: return r
    f.readline()
    cmd({"execute": "qmp_capabilities"})
    def keys(*qcodes):
        r = cmd({"execute": "send-key", "arguments": {"keys": [{"type": "qcode", "data": k} for k in qcodes]}})
        if "error" in r: raise SystemExit(f"FAIL: QMP rejected send-key {qcodes}: {r['error']}")
    def move(x, y):
        cmd({"execute": "input-send-event", "arguments": {"events": [
            {"type": "abs", "data": {"axis": "x", "value": int(x * 32768 / LOGICAL_W)}},
            {"type": "abs", "data": {"axis": "y", "value": int(y * 32768 / LOGICAL_H)}}]}})
    def click():
        cmd({"execute": "input-send-event", "arguments": {"events": [{"type": "btn", "data": {"down": True, "button": "left"}}]}})
        time.sleep(0.1)
        cmd({"execute": "input-send-event", "arguments": {"events": [{"type": "btn", "data": {"down": False, "button": "left"}}]}})
    def frame():
        cmd({"execute": "pmemsave", "arguments": {"val": FB, "size": W * H * 4, "filename": DUMP}})
        return Image.frombytes("RGBA", (W, H), open(DUMP, "rb").read(), "raw", "BGRA").convert("RGB")
    def pixel(x, y, img=None):
        return (img or frame()).getpixel((x * SCALE + 1, y * SCALE + 1))
    def near(p, c, tol=12): return max(abs(p[i] - c[i]) for i in range(3)) <= tol

    def wav_size():
        try: return os.path.getsize(WAV)
        except OSError: return 0
    SHOTS = os.environ.get("MOVIE_SHOTS")
    def shot(name):
        if SHOTS:
            os.makedirs(SHOTS, exist_ok=True)
            frame().crop((VIEW_X * SCALE, VIEW_Y * SCALE, (VIEW_X + WIN_W) * SCALE, (VIEW_Y + WIN_H) * SCALE)).save(f"{SHOTS}/{name}.png")

    LAYOUT = re.compile(r"movie=layout (\d+) (\d+) (\d+) (\d+)")
    def layout():
        for _, t in reversed(lines):
            m = LAYOUT.search(t)
            if m: return tuple(int(v) for v in m.groups())
        return None
    def read_code(img=None):
        """The frame number drawn on the picture, read off the framebuffer. The clip is 160x120 with six 18x20 squares."""
        lay = layout()
        if not lay: return None
        px, py, pw, ph = lay
        img = img or frame()
        code = 0
        for bit in range(6):
            sx = 20 + bit * 20 + 9; sy = 60
            wx = VIEW_X + px + sx * pw // 160; wy = VIEW_Y + py + sy * ph // 120
            r, g, b = img.getpixel((wx * SCALE + 1, wy * SCALE + 1))
            code = code * 2 + (1 if (r + g + b) / 3 > 128 else 0)
        return code

    # ---- the app is up and lists the four clips, sorted ----
    if wait_for(has("autoopen=movies"), 40) < 0: fails.append("open=movi never armed the Movies auto-open")
    if wait_for(has("ring3app: launching MOVIES.BIN at ring 3"), 40) < 0: fails.append("Movies was never launched as a ring-3 program")
    if wait_for(has("movies: ring-3 window 804x345"), 15) < 0: fails.append("Movies did not report its window (expected 804x345)")
    i_list = wait_for(has("movie=list / n=4"), 10)
    if i_list < 0: fails.append("the picker did not list the four fixture files (movie=list / n=4)")
    time.sleep(0.5)
    shot("picker")

    def cur_sel():
        sel = [t for _, t in lines if t.startswith("movie=select ")]
        return sel[-1][len("movie=select "):] if sel else None
    ORDER = ["BAD.AVI", "BIG.AVI", "CLIP.AVI", "JUNK.AVI"]
    def select(name):
        for _ in range(10):
            cur = cur_sel()
            if cur == name: return True
            if cur not in ORDER: return False
            keys("down" if ORDER.index(cur) < ORDER.index(name) else "up"); time.sleep(0.3)
        return cur_sel() == name
    def go_top():
        for _ in range(6): keys("up"); time.sleep(0.1)

    # ---- 1/2. play CLIP.AVI start to finish ----
    if not select("CLIP.AVI"): fails.append("could not move the selection to CLIP.AVI")
    n0 = len(lines)
    keys("ret")
    ip = wait_for(has("movie=play CLIP.AVI frames=40 us=50000 rate=11025"), 10, n0)
    if ip < 0: fails.append("Enter on CLIP.AVI did not start it (movie=play ... frames=40 us=50000 rate=11025)")
    t_play = lines[ip][0] if ip >= 0 else time.monotonic()
    idone = wait_for(has("movie=done"), 8, max(ip, 0))
    if idone < 0: fails.append("the clip never finished (no movie=done within 8 s)")
    fr = frames_from(max(ip, 0), idone if idone >= 0 else None)
    print(f"play: {len(fr)} frames shown of {NFRAMES}")
    if not fr: fails.append("no movie=frame lines while playing")
    else:
        if fr[0][1] != 0: fails.append(f"playback did not start at frame 0 (first was {fr[0][1]})")
        nums = [x[1] for x in fr]
        if any(b < a for a, b in zip(nums, nums[1:])): fails.append("frame numbers went backwards")
        if len(fr) < NFRAMES * 0.5: fails.append(f"only {len(fr)} of {NFRAMES} frames were shown: the player is too slow or stalls")
        bad = [(n, p) for _, n, ms, p in fr if p and abs(n - (p * 1000000 // (RATE * US))) > 1]
        if bad: fails.append(f"frame number is more than one frame from the audio clock at {len(bad)} points, e.g. {bad[0]}")
        # drift: the audio clock against wall time, relative to the first frame that had audio moving
        base = [(t, ms) for t, n, ms, p in fr if p > 0]
        if len(base) > 4:
            t0, m0 = base[0]
            if os.environ.get("MOVIE_DEBUG"): print("frames (wall ms, N, clock ms, played):", [(round((t - t0) * 1000), n, ms, p) for t, n, ms, p in fr])
            drift = max(abs((t - t0) * 1000 - (ms - m0)) for t, ms in base)
            print(f"drift: audio clock vs wall clock stays within {drift:.0f} ms over {(base[-1][0] - t0):.2f} s")
            if drift > 100: fails.append(f"audio clock drifted {drift:.0f} ms from wall time (limit 100)")
        else: fails.append("the audio clock never moved (played stayed 0): no sound was queued")
    if idone >= 0 and ip >= 0:
        wall = lines[idone][0] - t_play
        print(f"the clip took {wall:.2f} s of wall time (2.00 s long)")
        if not 1.6 <= wall <= 3.2: fails.append(f"the 2 s clip took {wall:.2f} s to play")
        if not re.search(r"movie=done frames=40 shown=\d+ dropped=\d+", lines[idone][1]): fails.append("movie=done carried the wrong counts")

    # ---- 4. pause holds the frame, the clock resumes where it stopped ----
    keys("spc")                                    # the finished clip restarts from 0
    ir = wait_for(has("movie=restart"), 5, max(idone, 0))
    if ir < 0: fails.append("space after the end did not restart the clip")
    fi = wait_for(lambda t: (FRAME.search(t) and int(FRAME.search(t).group(1)) >= 10), 6, max(ir, 0))
    if fi < 0: fails.append("playback did not reach frame 10 after the restart")
    n1 = len(lines)
    keys("spc")
    ipa = wait_for(has("movie=pause ms="), 3, n1)
    if ipa < 0: fails.append("space did not pause (no movie=pause)")
    else:
        pm = re.search(r"ms=(\d+) frame=(\d+)", lines[ipa][1]); p_ms, p_frame = int(pm.group(1)), int(pm.group(2))
        time.sleep(0.4)                            # let any frame already in flight land
        nq = len(lines)
        wav_b = wav_size()
        img = frame()
        shot("paused")
        code = read_code(img)
        time.sleep(1.2)
        late = frames_from(nq)
        wav_c = wav_size()
        # the sound card's own output: while paused no new audio may reach it (QEMU's wav writer only appends what the card played)
        print(f"pause: the wav grew {(wav_c - wav_b) / 176400:.2f} s of audio during the 1.2 s pause")
        if wav_c - wav_b > 26000: fails.append(f"the sound kept playing during the pause: the wav grew {(wav_c - wav_b) / 176400:.2f} s of audio")
        if late: fails.append(f"{len(late)} frames kept coming while paused")
        if wait_for(has("movie=done"), 0.05, ipa) >= 0: fails.append("the clip ran to its end while paused")
        print(f"pause: held at frame {p_frame} ({p_ms} ms), the framebuffer shows code {code}")
        if code != p_frame: fails.append(f"paused on frame {p_frame} but the picture on the screen is frame {code}")
        n2 = len(lines)
        keys("spc")
        wav_d = wav_size(); time.sleep(0.6); wav_e = wav_size()
        print(f"resume: the wav grew {(wav_e - wav_d) / 176400:.2f} s of audio in the 0.6 s after resume")
        if wav_e - wav_d < 35000: fails.append("after resume the sound card got no audio (the wav did not grow)")
        if wait_for(has("movie=resume ms=" + str(p_ms)), 3, n2) < 0: fails.append("space did not resume from the paused position")
        nr = wait_for(lambda t: FRAME.search(t) is not None, 3, n2)
        if nr >= 0:
            fn = int(FRAME.search(lines[nr][1]).group(1))
            print(f"resume: first frame after resume is {fn} (paused on {p_frame})")
            if fn > p_frame + 4 or fn < p_frame - 4: fails.append(f"resumed at frame {fn}, not near where it paused ({p_frame}): the clock ran or jumped during the pause")
        else: fails.append("no frame after resume")

    # ---- 5. seek: bar click lands near 1.0 s; the picture agrees ----
    wait_for(lambda t: (FRAME.search(t) and int(FRAME.search(t).group(1)) >= 6), 4, len(lines) - 8)
    n3 = len(lines)
    bx = VIEW_X + 52 + (WIN_W - 110 - 52) // 2
    move(bx, VIEW_Y + WIN_H - 44 + 22); time.sleep(0.2); click()
    isk = wait_for(has("movie=seek ms="), 3, n3)
    if isk < 0: fails.append("a click on the middle of the bar did not seek")
    else:
        t_ms = int(re.search(r"ms=(\d+)", lines[isk][1]).group(1))
        nf = wait_for(lambda t: FRAME.search(t) is not None, 4, isk)
        lm = int(FRAME.search(lines[nf][1]).group(2)) if nf >= 0 else -1
        print(f"seek: asked for the middle of the bar, target {t_ms} ms, first frame after it at {lm} ms")
        if abs(t_ms - 1000) > 60: fails.append(f"bar click aimed at {t_ms} ms, not the middle (1000 ms)")
        if nf < 0 or abs(lm - 1000) > 500: fails.append(f"after the seek the player is at {lm} ms, more than 0.5 s from 1000")
        elif not (0 <= lm):
            fails.append("bad seek position")
    move(*PARK)
    # left: five seconds back from ~1 s is the start
    n4 = len(lines)
    keys("left")
    il = wait_for(has("movie=seek ms="), 3, n4)
    if il < 0 or int(re.search(r"ms=(\d+)", lines[il][1]).group(1)) > 200: fails.append("left arrow from ~1 s did not clamp to the start")
    nf = wait_for(lambda t: FRAME.search(t) is not None, 3, max(il, 0))
    if nf < 0 or int(FRAME.search(lines[nf][1]).group(2)) > 500: fails.append("after left the picture is not near the start")
    time.sleep(0.3)
    n5 = len(lines)
    keys("right")
    irt = wait_for(has("movie=seek ms="), 3, n5)
    if irt < 0 or int(re.search(r"ms=(\d+)", lines[irt][1]).group(1)) < 1800: fails.append("right arrow past the end did not clamp to the end")
    # seek while paused: the picture follows without playing
    wait_for(has("movie=done"), 3, n5)
    keys("spc"); time.sleep(0.5)                   # restart
    wait_for(lambda t: (FRAME.search(t) and int(FRAME.search(t).group(1)) >= 4), 3, len(lines) - 8)
    keys("spc"); time.sleep(0.4)                   # pause
    nq = len(lines)
    keys("right"); time.sleep(0.6)
    img = frame(); code = read_code(img)
    print(f"seek while paused: screen shows frame {code}")
    if code is None or code < 30: fails.append(f"seeking right while paused did not move the picture to the end (screen shows {code})")
    keys("left"); time.sleep(0.6)
    code = read_code()
    if code is None or code > 6: fails.append(f"seeking left while paused did not move the picture to the start (screen shows {code})")
    keys("spc"); time.sleep(0.3)                   # play on, mid-play pixel read within a few frames of the last marker
    time.sleep(0.5)
    lf = last_frame(); code = read_code()
    if lf and code is not None:
        print(f"mid-play: last marker frame {lf[1]}, screen shows {code}")
        if abs(code - lf[1]) > 8: fails.append(f"mid-play the screen shows frame {code} but the app's last marker says {lf[1]}")

    # ---- 6. fullscreen ----
    wait_for(has("movie=done"), 4, len(lines) - 5)
    keys("spc"); time.sleep(0.4)
    keys("spc"); time.sleep(0.3)                   # paused again, so the layout is steady
    n6 = len(lines)
    keys("f")
    if wait_for(has("movie=fullscreen 1"), 3, n6) < 0: fails.append("f did not report fullscreen")
    time.sleep(0.5)
    lay = layout()
    shot("fullscreen")
    print(f"fullscreen: picture rectangle {lay}")
    if not lay or lay[3] != WIN_H: fails.append(f"fullscreen did not fill the window height (picture {lay})")
    keys("f")
    if wait_for(has("movie=fullscreen 0"), 3, n6) < 0: fails.append("f did not leave fullscreen")
    time.sleep(0.5)
    lay = layout()
    if not lay or lay[3] >= WIN_H: fails.append(f"leaving fullscreen did not give the controls back (picture {lay})")

    # ---- 8. a damaged clip, a non-AVI and an over-cap file each show an error and the list returns ----
    keys("esc")
    if wait_for(has("movie=stop"), 3, n6) < 0: fails.append("esc did not stop the clip and return to the list")
    time.sleep(0.4)
    for name, code_ in (("BAD.AVI", "corrupt"), ("JUNK.AVI", "format"), ("BIG.AVI", "too-big")):
        if not select(name): fails.append(f"could not select {name}"); continue
        nn = len(lines)
        keys("ret")
        if wait_for(has("movie=error " + code_), 8, nn) < 0: fails.append(f"{name} did not show 'movie=error {code_}'")
        time.sleep(0.4)
        shot("error-" + name[:3].lower())
        keys("ret"); time.sleep(0.4)               # dismiss the message, back to the list
    if "ring3app: BUG" in serial() or "exception: ring-0" in serial() or "panic in" in serial(): fails.append("the kernel faulted or logged a BUG while the app handled bad files")

    # ---- the app closes cleanly and the desktop is alive ----
    exits = serial().count("MOVIES.BIN exited 0")
    keys("esc")
    if wait_for(has("movies: closed"), 5, n6) < 0: fails.append("esc in the list did not close the app")
    released = wait_for(has("syscall: window released, task gone"), 5, n6) >= 0
    for _ in range(50):
        if serial().count("MOVIES.BIN exited 0") > exits: break
        time.sleep(0.1)
    if not released or serial().count("MOVIES.BIN exited 0") <= exits: fails.append("Movies did not exit 0 and release its window")
    move(*PARK); time.sleep(0.5)
    for _ in range(100):
        if not near(pixel(CLOSE_X, CLOSE_Y), CLOSE_RED): break
        time.sleep(0.1)
    if near(pixel(CLOSE_X, CLOSE_Y), CLOSE_RED): fails.append("an app window is still open after Esc")
    if pixel(480, 511) != (0xEF, 0xEB, 0xE4): fails.append("desktop dock not on screen after the close")
    move(SLOT0_X + 2 * PITCH + DOCK_ICON // 2, ICON_ROW_Y); time.sleep(0.3); click()
    opened = False
    for _ in range(40):
        time.sleep(0.1)
        if near(pixel(CLOSE_X, CLOSE_Y), CLOSE_RED): opened = True; break
    print(f"Mail opens from the dock after the close: {'yes' if opened else 'NO'}")
    if not opened: fails.append("Mail did not open from a dock click after Movies closed: desktop not responsive")
    keys("esc"); time.sleep(0.5)
finally:
    _stop = True
    try: cmd({"execute": "quit"})
    except (ConnectionResetError, BrokenPipeError, OSError, ValueError, NameError): pass
    q.terminate()
    try: q.wait(5)
    except subprocess.TimeoutExpired: q.kill()

# ---- 7. the sound that actually came out ----
def goertzel(samples, rate, freq):
    w = 2 * math.pi * freq / rate; c = 2 * math.cos(w); s1 = s2 = 0.0
    for x in samples:
        s0 = x + c * s1 - s2; s2, s1 = s1, s0
    return s1 * s1 + s2 * s2 - c * s1 * s2
try:
    raw = open(WAV, "rb").read()
except OSError:
    raw = b""
if len(raw) <= 44:
    fails.append("QEMU wrote no audio: the app never reached the sound card")
else:
    ch, rate = struct.unpack_from("<HI", raw, 22); width = struct.unpack_from("<H", raw, 34)[0] // 8
    body = raw[44:]; body = body[:len(body) - len(body) % (width * ch)]
    vals = struct.unpack(f"<{len(body)//2}h", body) if width == 2 else [b - 128 for b in body]
    left = [float(v) for v in vals[::ch]]
    loud = [i for i, v in enumerate(left) if abs(v) > 500]
    secs = len(loud) / rate if rate else 0
    print(f"audio: wav {rate} Hz, {secs:.2f} s of sound above the noise floor")
    if secs < 1.8: fails.append(f"only {secs:.2f} s of sound came out (a full play is 2 s)")
    else:
        seg = left[loud[0]:loud[0] + rate // 2]
        e440, e300, e600 = (goertzel(seg, rate, fr) for fr in (440, 300, 600))
        if e440 < 20 * e300 or e440 < 20 * e600: fails.append("the sound is not the 440 Hz tone in the clip")

if fails:
    print("FAIL:")
    for x in fails: print("  - " + x)
    print("--- serial tail ---")
    print("\n".join(t for _, t in lines[-40:]))
    sys.exit(1)
print("PASS: Movies played a real clip with sound at ring 3, audio-led (frame within one of the audio clock, drift under 100 ms), paused, seeked and went fullscreen on the real framebuffer, showed errors for bad files, closed on Esc, and the desktop stayed alive")
