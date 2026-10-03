#!/usr/bin/env python3
"""Music runs as a real ring-3 app (user/music.c, RING3_APPS row "Music", Apps folder) and plays.

Boots headless with `open=musi` (never a window), a Sound Blaster 16 wired to QEMU's wav backend
and a FAT16 disk built by tools/media/musicfix.py: ALPHA.WAV in the root, BRAVO.WAV and a
too-big ZHUGE.WAV in MUSIC/. No media is in the kernel image. The check reads the `music:` lines
the app writes to serial and drives it with real keys and a real mouse click over QMP:

  1. the library finds the three songs and the app reports where its seek bar is;
  2. space plays ALPHA: the `at playing pos=` lines (position = stream start + the driver's
     `played` counter, never the wall clock) strictly increase, and reach 1.5s;
  3. space again pauses: the app prints `paused pos=P` and every later line holds P exactly,
     across at least three wall-clock seconds;
  4. a click in the middle of the seek bar seeks within 0.5s of half the song (while paused),
     right and left arrows move it 5s each way, and the position still holds;
  5. space resumes from the seek point and advances from there;
  6. n loads BRAVO (a 16-bit stereo song, so the downmix runs) and it plays from the start;
  7. n again reaches ZHUGE, which is over the size cap: refused with a message, no crash;
  8. Esc closes it: exit 0, window released, kernel alive;
  9. what QEMU's sound card really received: a 440Hz stretch (ALPHA) and then a 660Hz stretch
     (BRAVO), found with a Goertzel probe, so the bytes reached the speaker and in the right order.

Every wait has a deadline. Discriminating: take the position from the wall clock instead of
`played` and step 3 fails; ignore the seek target and step 4 fails; make n do nothing and step 6
fails; stop feeding the ring and steps 2 and 9 fail.

Usage: tools/checks/music-check.py   (from the repo root, after make kernel.elf)
"""
import json, math, os, re, socket, struct, subprocess, sys, tempfile, time
from freeport import free_port

os.chdir(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
LOGICAL_W, LOGICAL_H = 960, 540
VIEW_X, VIEW_Y = 78, 72          # gui_launch_from_dock: the app viewport sits at (x+8, y+32) for a window at 70,40
LOG = "/tmp/jt-music-serial.log"
DISK = "/tmp/jt-music-fat.img"
WAVOUT = "/tmp/jt-music-out.wav"
ALPHA_MS = 30000

for f in (LOG, WAVOUT):
    try: os.remove(f)
    except FileNotFoundError: pass
subprocess.run([sys.executable, "tools/media/musicfix.py", DISK], check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

port = free_port()
q = subprocess.Popen(["qemu-system-i386", "-kernel", "kernel.elf", "-append", "open=musi", "-nic", "none",
                      "-display", "none", "-vga", "std",
                      "-drive", f"file={DISK},format=raw,if=ide,index=0",
                      "-audiodev", f"wav,id=snd,path={WAVOUT}", "-device", "sb16,audiodev=snd",
                      "-qmp", f"tcp:127.0.0.1:{port},server,nowait", "-serial", "file:" + LOG],
                     stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
fails = []


def lines():
    try: return open(LOG, errors="replace").read().splitlines()
    except OSError: return []


def wait_line(pattern, secs, after=0):
    """First line at index >= after matching the regex; returns (index, match) or (None, None)."""
    rx = re.compile(pattern)
    end = time.time() + secs
    while True:
        ls = lines()
        for i in range(after, len(ls)):
            m = rx.search(ls[i])
            if m: return i, m
        if time.time() > end: return None, None
        time.sleep(0.05)


def wait_n(pattern, count, secs, after):
    """Waits for `count` lines matching pattern at index >= after; returns their match objects."""
    rx = re.compile(pattern)
    end = time.time() + secs
    while True:
        out = [m for m in (rx.search(l) for l in lines()[after:]) if m]
        if len(out) >= count or time.time() > end: return out
        time.sleep(0.1)


try:
    s = None
    for _ in range(50):
        time.sleep(0.2)
        try: s = socket.create_connection(("127.0.0.1", port)); break
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

    def key(*qcodes):
        r = cmd({"execute": "send-key", "arguments": {"keys": [{"type": "qcode", "data": k} for k in qcodes]}})
        if "error" in r: raise SystemExit(f"FAIL: QMP rejected send-key {qcodes}: {r['error']}")
        time.sleep(0.3)

    def move(x, y):
        cmd({"execute": "input-send-event", "arguments": {"events": [
            {"type": "abs", "data": {"axis": "x", "value": int(x * 32768 / LOGICAL_W)}},
            {"type": "abs", "data": {"axis": "y", "value": int(y * 32768 / LOGICAL_H)}}]}})

    def click():
        cmd({"execute": "input-send-event", "arguments": {"events": [{"type": "btn", "data": {"down": True, "button": "left"}}]}})
        time.sleep(0.12)
        cmd({"execute": "input-send-event", "arguments": {"events": [{"type": "btn", "data": {"down": False, "button": "left"}}]}})

    def mark(): return len(lines())

    # 1. launched, library listed, bar located
    i, _ = wait_line(r"ring3app: launching MUSIC\.BIN at ring 3", 45)
    if i is None: fails.append("Music was never launched as a ring-3 program (open=musi or the RING3_APPS row is broken)")
    i, m = wait_line(r"music: library n=(\d+)", 15)
    if i is None or int(m.group(1)) != 3:
        fails.append(f"the library did not list the three fixture songs ({m.group(0) if m else 'no library line'}): SYS_READDIR on the root and MUSIC/")
    i, m = wait_line(r"music: ui bar=(\d+),(\d+),(\d+)", 10)
    if i is None: raise SystemExit("FAIL: the app never announced its seek bar\n" + "\n".join(lines()[-15:]))
    bx0, bx1, by = (int(m.group(k)) for k in (1, 2, 3))
    time.sleep(0.5)

    # 2. play: position advances, from the driver's played counter
    a = mark()
    key("spc")
    i, _ = wait_line(r"music: playing ALPHA\.WAV", 15, a)
    if i is None: fails.append("space did not start ALPHA.WAV (no 'music: playing ALPHA.WAV')")
    pl = [int(m.group(1)) for m in wait_n(r"music: at playing pos=(\d+)", 4, 14, a)]
    print(f"playing positions (ms): {pl}")
    if len(pl) < 4: fails.append(f"fewer than 4 'at playing' lines in 14s: {pl}")
    elif not all(pl[k] < pl[k + 1] for k in range(len(pl) - 1)): fails.append(f"the position did not strictly advance while playing: {pl}")
    elif pl[-1] < 1500: fails.append(f"after four position lines of play the position is only {pl[-1]}ms: played is not advancing")

    # 3. pause holds the position
    a = mark()
    key("spc")
    i, m = wait_line(r"music: paused pos=(\d+)", 10, a)
    if i is None:
        fails.append("space did not pause (no 'music: paused pos=')")
        P = None
    else:
        P = int(m.group(1))
        hold = [int(x.group(1)) for x in wait_n(r"music: at paused pos=(\d+)", 3, 8, i)]
        print(f"paused at {P}ms, later lines: {hold}")
        if len(hold) < 3: fails.append(f"fewer than 3 paused position lines: {hold}")
        elif any(h != P for h in hold): fails.append(f"pause did not hold the position: paused at {P}, then {hold}")
        if any("music: at playing" in l for l in lines()[i:]): fails.append("the app kept reporting 'playing' after the pause")
        if P < 1500: fails.append(f"it paused at {P}ms, less than the 1.5s it had played")

    # 4. seek: click the middle of the bar, then arrows, all while paused
    a = mark()
    move(VIEW_X + bx0 + (bx1 - bx0) // 2, VIEW_Y + by); time.sleep(0.3); click()
    i, m = wait_line(r"music: seek pos=(\d+)", 10, a)
    S = None
    if i is None: fails.append("a click on the seek bar did not seek (no 'music: seek pos=')")
    else:
        S = int(m.group(1))
        print(f"bar click -> {S}ms (wanted {ALPHA_MS // 2})")
        if abs(S - ALPHA_MS // 2) > 500: fails.append(f"bar click landed at {S}ms, more than 0.5s from the middle ({ALPHA_MS // 2}ms)")
        h = [int(x.group(1)) for x in wait_n(r"music: at paused pos=(\d+)", 2, 6, i)]
        if len(h) < 2 or any(abs(v - S) > 0 for v in h): fails.append(f"after the seek the paused position drifted: seek {S}, then {h}")
    if S is not None:
        a = mark()
        key("right")
        i, m = wait_line(r"music: seek pos=(\d+)", 10, a)
        if i is None: fails.append("the right arrow did not seek")
        else:
            S2 = int(m.group(1)); print(f"right arrow -> {S2}ms")
            if abs(S2 - (S + 5000)) > 500: fails.append(f"right arrow moved {S2 - S}ms, not about 5000")
        a = mark()
        key("left")
        i, m = wait_line(r"music: seek pos=(\d+)", 10, a)
        if i is None: fails.append("the left arrow did not seek")
        else:
            S3 = int(m.group(1)); print(f"left arrow -> {S3}ms")
            if abs(S3 - S) > 500: fails.append(f"left arrow came back to {S3}ms, not near {S}ms")
            S = S3

    # 5. resume from the seek point
    a = mark()
    key("spc")
    i, _ = wait_line(r"music: resumed pos=", 8, a)
    if i is None: fails.append("space did not resume")
    pl = [int(m.group(1)) for m in wait_n(r"music: at playing pos=(\d+)", 3, 12, a)]
    print(f"resumed positions (ms): {pl}")
    if len(pl) < 3: fails.append(f"fewer than 3 'at playing' lines after the resume: {pl}")
    elif S is not None:
        if not all(pl[k] < pl[k + 1] for k in range(len(pl) - 1)): fails.append(f"position did not advance after the resume: {pl}")
        if not (S - 200 <= pl[0] <= S + 2500): fails.append(f"playing did not resume from the seek point {S}ms: first line {pl[0]}ms")

    # 6. next song: BRAVO, a 16-bit stereo file
    a = mark()
    key("n")
    i, _ = wait_line(r"music: playing BRAVO\.WAV", 8, a)
    if i is None: fails.append("n did not move to BRAVO.WAV within 8s (ALPHA is 30s long, so it did not just end by itself)")
    else:
        if any("music: ended ALPHA" in l for l in lines()[a:i]): fails.append("ALPHA ended on its own before n took effect: the check proved nothing about n")
        pl = [int(m.group(1)) for m in wait_n(r"music: at playing pos=(\d+)", 3, 12, i)]
        print(f"BRAVO positions (ms): {pl}")
        if len(pl) < 3 or not all(pl[k] < pl[k + 1] for k in range(len(pl) - 1)): fails.append(f"BRAVO did not advance: {pl}")
        elif pl[0] > 3000: fails.append(f"BRAVO did not start from the top: first line {pl[0]}ms")

    # 7. next again: ZHUGE is over the cap, refused visibly
    a = mark()
    key("n")
    i, _ = wait_line(r"music: refused ZHUGE\.WAV", 20, a)
    if i is None: fails.append("ZHUGE.WAV (3.3MB, over the 3MB cap) was not refused")
    time.sleep(0.5)

    # 8. Esc closes it cleanly
    a = mark()
    key("esc")
    if wait_line(r"music: closed", 8, a)[0] is None: fails.append("Esc did not reach the app (no 'music: closed')")
    if wait_line(r"MUSIC\.BIN exited 0", 8, a)[0] is None: fails.append("Music did not exit 0")
    if wait_line(r"syscall: window released, task gone", 8, a)[0] is None: fails.append("the window was not released")
    log = "\n".join(lines())
    if "exception: ring-0" in log or "panic in" in log or "ring3app: BUG" in log:
        fails.append("the kernel faulted or ring3app logged a BUG line")
finally:
    try: cmd({"execute": "quit"})
    except (ConnectionResetError, BrokenPipeError, OSError, ValueError, NameError): pass
    q.terminate()
    try: q.wait(5)
    except subprocess.TimeoutExpired: q.kill()


# 9. what the sound card actually received: 440Hz (ALPHA) and then 660Hz (BRAVO)
def goertzel(samples, rate, freq):
    w = 2 * math.pi * freq / rate
    c = 2 * math.cos(w)
    s1 = s2 = 0.0
    for x in samples:
        s0 = x + c * s1 - s2
        s2, s1 = s1, s0
    return s1 * s1 + s2 * s2 - c * s1 * s2


heard = []
if os.path.exists(WAVOUT) and os.path.getsize(WAVOUT) > 44:
    raw = open(WAVOUT, "rb").read()
    ch, rate = struct.unpack_from("<HI", raw, 22)
    width = struct.unpack_from("<H", raw, 34)[0] // 8
    raw = raw[44:]
    raw = raw[:len(raw) - len(raw) % (width * ch)]
    vals = struct.unpack(f"<{len(raw) // 2}h", raw) if width == 2 else [b - 128 for b in raw]
    mono = [float(v) for v in vals[::ch]]
    win = rate // 2
    for k in range(0, len(mono) - win, win):
        seg = mono[k:k + win]
        rms = math.sqrt(sum(v * v for v in seg) / len(seg))
        if rms < 800: continue
        e440, e660 = goertzel(seg, rate, 440), goertzel(seg, rate, 660)
        if e440 > 10 * e660: heard.append(440)
        elif e660 > 10 * e440: heard.append(660)
    print(f"sound card output, half-second windows by tone: {heard}")
else:
    fails.append("QEMU's sound card received no audio at all")
if heard:
    if heard.count(440) < 4: fails.append(f"too little 440Hz (ALPHA) reached the speaker: {heard.count(440)} half-second windows")
    if heard.count(660) < 4: fails.append(f"too little 660Hz (BRAVO) reached the speaker: {heard.count(660)} half-second windows")
    if 440 in heard and 660 in heard and heard.index(660) < len(heard) - 1 - heard[::-1].index(440):
        fails.append("the tones came out of order: a 440Hz stretch after 660Hz began")
elif not fails or "no audio" not in " ".join(fails):
    fails.append("no loud tone windows in the sound card output")

if fails:
    print("FAIL:")
    for x in fails: print("  - " + x)
    print("--- serial tail ---")
    print("\n".join(lines()[-25:]))
    sys.exit(1)
print("PASS: Music played real sound (440Hz then 660Hz at the card), played advanced, pause held, bar and arrow seeks landed, next changed the song, an oversize file was refused, Esc closed it")
