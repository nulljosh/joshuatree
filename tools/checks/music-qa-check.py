#!/usr/bin/env python3
"""The Music QA pass (2.44.1): the bugs the walk-through found, each fix's assertion failing when that fix is reverted.

Three headless boots with `open=musi`, a Sound Blaster 16 and FAT16 disks from tools/media/musicfix.py (--set qa,
many, empty). The checks read the app's `music:` serial lines and, for what only the screen can show, the
framebuffer (QMP pmemsave, as ring3mines-check.py does). Songs are clicked, keys are real, the wheel is a real
QMP wheel event.

Boot A (qa: ALPHA.WAV 3 s, BAD.WAV, CORRUPT.MP3, DELTA.MP3 3:25, EMPTY.WAV, FINAL.WAV):
  1. the queue never holds more than half a second ahead of the speaker at 8 kHz (`ahead=` on the position
     lines): the volume is applied before a sample is queued, so what is queued is how late a volume change is;
     it was a second and a half (an 8192 byte lead plus a 4096 byte chunk);
  2. ALPHA ends into BAD.WAV, which will not play: the next song starts by itself (it stopped on BAD.WAV);
  3. CORRUPT.MP3 has one frame with damaged side info: it plays to its end (it hung at that frame for ever,
     the decoder reading short of what the header scan had counted);
  4. DELTA.MP3 (4.5 million frames at 22.05 kHz) reports its real length, 3:25 (frames * 1000 overflowed 32 bits
     and it read 0:10), a click 97% along the seek bar lands past 3:15, and it ends on its own after the seek;
  5. a seek while playing lands 5 s on and the position goes on at one second a second (a drift proof: it passed
     before the fixes too);
  6. FINAL after EMPTY.WAV (0 bytes, skipped) ends the list, and a stopped row shows no pause bars;
  7. after a click on a song that will not play, n moves on to the next one (it asked for the same song again).
Boot B (many: fourteen 1 s songs):
  8. no row sits under the key hint: a click where the eleventh row used to be plays nothing;
  9. the wheel scrolls the list under the pointer one row a step, up and down, and does nothing over the desktop
     (no window ever received a wheel step before, and the list snapped back to the selection on every repaint);
 10. shuffle with repeat off plays all fourteen once each and then stops (it picked with replacement, for ever);
 11. a half-screen window (snapped left) still shows the Shuffle and Repeat pills whole, and the Repeat pill clicks.
Also in boot A: the play button is at least 8 px above the volume bar (it was 4); the divider between the panes stops
above the key hint; a stopped song's row shows no pause bars.
Boot C (empty): n, p and space on an empty library say nothing ("ended list"), and the empty state's hint is four
lines (it was three and left out MP3).
Esc closes it with exit 0 on every boot.

Usage: tools/checks/music-qa-check.py   (from the repo root, after make kernel.elf; needs mtools)
"""
import json, os, re, socket, subprocess, sys, time
from PIL import Image
from freeport import free_port
from scratch import scratch_dir

os.chdir(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
TMP = scratch_dir("musicqa")
FB = 0xfd000000; W, H = 1920, 1080
LOGICAL_W, LOGICAL_H = 960, 540
VIEW_X, VIEW_Y = 78, 72     # the app viewport sits at (window x + 8, y + 32) for a window at 70, 40
LIST_Y, ROW_H = 46, 26      # user/music.c
TINT, ACCENT, BG = (0xF2, 0xE4, 0xDC), (0xB5, 0x50, 0x2C), (0xFA, 0xF8, 0xF6)
fails = []


def fail(msg):
    fails.append(msg)
    print("  FAIL: " + msg)


class Boot:
    def __init__(self, label, which):
        self.label = label
        self.view = (VIEW_X, VIEW_Y)    # where the app's viewport sits on the logical screen; a snapped window moves it
        self.log = os.path.join(TMP, label + ".log")
        self.dump = os.path.join(TMP, label + ".raw")
        disk = os.path.join(TMP, label + ".img")
        subprocess.run([sys.executable, "tools/media/musicfix.py", disk, os.path.join(TMP, label + "-files"), "--set", which],
                       check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        port = free_port()
        self.q = subprocess.Popen(["qemu-system-i386", "-kernel", "kernel.elf", "-append", "open=musi", "-nic", "none",
                                   "-display", "none", "-vga", "std", "-name", "jt-musicqa-" + label,
                                   "-drive", f"file={disk},format=raw,if=ide,index=0",
                                   "-audiodev", "wav,id=snd,path=" + os.path.join(TMP, label + ".wav"), "-device", "sb16,audiodev=snd",
                                   "-qmp", f"tcp:127.0.0.1:{port},server,nowait", "-serial", "file:" + self.log],
                                  stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        s = None
        for _ in range(50):
            time.sleep(0.2)
            try: s = socket.create_connection(("127.0.0.1", port)); break
            except OSError: pass
        if s is None: raise SystemExit("FAIL: QEMU's QMP socket never came up")
        self.f = s.makefile("rw")
        self.f.readline()
        self.cmd({"execute": "qmp_capabilities"})

    def cmd(self, o):
        self.f.write(json.dumps(o) + "\n"); self.f.flush()
        while True:
            r = json.loads(self.f.readline())
            if "return" in r or "error" in r: return r

    def lines(self):
        try: return open(self.log, errors="replace").read().splitlines()
        except OSError: return []

    def mark(self): return len(self.lines())

    def wait(self, pattern, secs, after=0):
        """First line at index >= after matching the regex: (index, match), or (None, None) at the deadline."""
        rx = re.compile(pattern); end = time.time() + secs
        while True:
            ls = self.lines()
            for i in range(after, len(ls)):
                m = rx.search(ls[i])
                if m: return i, m
            if time.time() > end: return None, None
            time.sleep(0.05)

    def wait_n(self, pattern, count, secs, after=0):
        rx = re.compile(pattern); end = time.time() + secs
        while True:
            out = [m for m in (rx.search(l) for l in self.lines()[after:]) if m]
            if len(out) >= count or time.time() > end: return out
            time.sleep(0.1)

    def quiet(self, pattern, secs, after=0):
        """True when no line matching the regex shows up for `secs` seconds."""
        return self.wait(pattern, secs, after)[0] is None

    def key(self, *qcodes, wait=0.3):
        r = self.cmd({"execute": "send-key", "arguments": {"keys": [{"type": "qcode", "data": k} for k in qcodes]}})
        if "error" in r: raise SystemExit(f"FAIL: QMP rejected send-key {qcodes}: {r['error']}")
        time.sleep(wait)

    def move(self, x, y):
        self.cmd({"execute": "input-send-event", "arguments": {"events": [
            {"type": "abs", "data": {"axis": "x", "value": int(x * 32768 / LOGICAL_W)}},
            {"type": "abs", "data": {"axis": "y", "value": int(y * 32768 / LOGICAL_H)}}]}})

    def button(self, name, down):
        self.cmd({"execute": "input-send-event", "arguments": {"events": [{"type": "btn", "data": {"down": down, "button": name}}]}})

    def click(self, x, y):
        self.move(x, y); time.sleep(0.3)
        self.button("left", True); time.sleep(0.12); self.button("left", False); time.sleep(0.3)

    def click_view(self, x, y): self.click(self.view[0] + x, self.view[1] + y)

    def wheel(self, name):
        self.button(name, True); time.sleep(0.05); self.button(name, False); time.sleep(0.4)

    def drag(self, x0, y0, x1, y1):
        self.move(x0, y0); time.sleep(0.3); self.button("left", True); time.sleep(0.2)
        for k in range(1, 9):
            self.move(x0 + (x1 - x0) * k // 8, y0 + (y1 - y0) * k // 8); time.sleep(0.05)
        time.sleep(0.3); self.button("left", False); time.sleep(1.2)

    def frame(self):
        self.cmd({"execute": "pmemsave", "arguments": {"val": FB, "size": W * H * 4, "filename": self.dump}})
        return Image.frombytes("RGBA", (W, H), open(self.dump, "rb").read(), "raw", "BGRA").convert("RGB")

    def close(self):
        try: self.cmd({"execute": "quit"})
        except (ConnectionResetError, BrokenPipeError, OSError, ValueError): pass
        self.q.terminate()
        try: self.q.wait(5)
        except subprocess.TimeoutExpired: self.q.kill()

    def start(self, count):
        i, _ = self.wait(r"ring3app: launching MUSIC\.BIN at ring 3", 60)
        if i is None: raise SystemExit("FAIL: Music was never launched as a ring-3 program (open=musi or the RING3_APPS row is broken)")
        i, m = self.wait(r"music: library n=(\d+)", 20)
        if i is None or int(m.group(1)) != count: fail(f"{self.label}: the library has {m.group(1) if m else 'no'} songs, expected {count}")
        i, m = self.wait(r"music: ui bar=(\d+),(\d+),(\d+)", 10)
        if i is None: raise SystemExit("FAIL: the app never announced its seek bar\n" + "\n".join(self.lines()[-15:]))
        time.sleep(0.8)
        self.bar = tuple(int(m.group(k)) for k in (1, 2, 3))

    def esc_closes(self):
        a = self.mark()
        self.key("esc")
        if self.wait(r"music: closed", 8, a)[0] is None: fail(f"{self.label}: Esc did not reach the app")
        if self.wait(r"MUSIC\.BIN exited 0", 8, a)[0] is None: fail(f"{self.label}: Music did not exit 0")
        log = "\n".join(self.lines())
        if "exception: ring-0" in log or "panic in" in log or "ring3app: BUG" in log:
            fail(f"{self.label}: the kernel faulted or ring3app logged a BUG line")


def px(img, x, y, view=(VIEW_X, VIEW_Y)): return img.getpixel(((view[0] + x) * 2, (view[1] + y) * 2))
def near(p, c, tol=3): return max(abs(p[i] - c[i]) for i in range(3)) <= tol
def row_y(i): return LIST_Y + i * ROW_H + 10


def runs(img, view, x0, x1, y, tint=TINT, minlen=40):
    """Runs of the pill colour along one row of the viewport: [(start, end)]."""
    out, start = [], None
    for x in range(x0, x1 + 1):
        t = x < x1 and near(px(img, x, y, view), tint)
        if t and start is None: start = x
        if not t and start is not None:
            if x - start >= minlen: out.append((start, x - 1))
            start = None
    return out


print("music-qa-check: boot A (long MP3, damaged MP3, songs that will not play)")
A = Boot("qa", "qa")
try:
    A.start(6)
    bx0, bx1, by = A.bar
    pos_lines = lambda a, n, secs: [(int(m.group(1)), int(m.group(2))) for m in A.wait_n(r"music: at playing pos=(\d+) ahead=(\d+)", n, secs, a)]

    # 1. ALPHA (8 kHz): how far ahead of the speaker the queue runs
    a = A.mark()
    A.click_view(100, row_y(0))
    t0 = None
    i, _ = A.wait(r"music: playing ALPHA\.WAV", 10, a)
    if i is None: fail("a click on ALPHA.WAV did not start it")
    else: t0 = time.time()
    pl = pos_lines(a, 2, 8)
    print(f"  ALPHA position lines (ms, ahead ms): {pl}")
    if len(pl) < 2: fail(f"fewer than two 'at playing ... ahead=' lines from ALPHA: {pl}")
    elif max(p[1] for p in pl) > 500:
        fail(f"the queue ran {max(p[1] for p in pl)} ms ahead of the speaker at 8 kHz: a volume change would land that late (limit 500)")

    # 2. ALPHA ends into BAD.WAV: the next song starts by itself
    i, _ = A.wait(r"music: ended ALPHA\.WAV", 12, a)
    if i is None: fail("ALPHA.WAV never ended (3 s of audio)")
    else:
        if t0 and time.time() - t0 < 2.5: fail(f"ALPHA.WAV, 3 s long, ended after {time.time() - t0:.1f}s: the end came before the audio had played")
        j, _ = A.wait(r"music: playing CORRUPT\.MP3", 10, i)
        if j is None:
            fail("ALPHA ended into BAD.WAV and nothing played on: a song that will not play must be skipped when one song ends into the next")
            A.click_view(100, row_y(2))                   # carry on by hand so the later steps still run
        elif any("music: playing BAD.WAV" in l for l in A.lines()[i:j]): fail("BAD.WAV was played")

    # 3. CORRUPT.MP3 plays to its end
    i, _ = A.wait(r"music: ended CORRUPT\.MP3", 12, a)
    if i is None:
        fail("CORRUPT.MP3 (1 s, one damaged frame) never ended: the player hangs at a frame the decoder cannot read")
        i = A.mark(); A.click_view(100, row_y(3))         # carry on by hand

    # 4. DELTA.MP3: its length, the seek bar near its end, its end
    i, m = A.wait(r"music: loaded total=(\d+) rate=22050", 10, i or a)
    if i is None: fail("DELTA.MP3 did not start")
    else:
        total = int(m.group(1))
        print(f"  DELTA.MP3 length {total} ms")
        if not 190000 <= total <= 215000: fail(f"DELTA.MP3 is 3:25 and Music says {total} ms (frames * 1000 overflowed 32 bits)")
        a2 = A.mark()
        time.sleep(2.2)
        img = A.frame()
        # the key hint runs under both panes and the divider stops above it
        lw = 268
        if any(near(px(img, lw + 12, y), (0xE4, 0xDE, 0xD6)) for y in range(318, 336)):
            fail("the divider between the panes runs through the key hint at the bottom")
        # the play button and the volume bar below it are not touching: scan the column through the button's centre
        col, runs_y, start = [near(px(img, 376, y), ACCENT, 8) for y in range(200, 335)], [], None
        for k, on in enumerate(col + [False]):
            if on and start is None: start = k
            if not on and start is not None: runs_y.append((start, k - 1)); start = None
        if len(runs_y) < 2: fail(f"could not find the play button and the volume bar in the frame (accent runs {runs_y})")
        elif runs_y[-1][0] - runs_y[-2][1] - 1 < 8:
            fail(f"the play button sits {runs_y[-1][0] - runs_y[-2][1] - 1} px above the volume bar (at least 8 wanted)")
        A.click_view(bx0 + (bx1 - bx0) * 97 // 100, by)
        i2, m2 = A.wait(r"music: seek pos=(\d+)", 10, a2)
        if i2 is None: fail("a click on the seek bar did not seek")
        else:
            S = int(m2.group(1)); print(f"  bar click at 97% -> {S} ms")
            if S < total * 94 // 100 or S > total: fail(f"a click 97% along the bar of a {total} ms song landed at {S} ms")
        i3, _ = A.wait(r"music: ended DELTA\.MP3", 25, a2)
        if i3 is None:
            fail("DELTA.MP3 never ended after a seek near its end")
            i3 = A.mark(); A.click_view(100, row_y(5))    # carry on by hand: FINAL.WAV
        elif A.wait(r"music: playing FINAL\.WAV", 10, i3)[0] is None:
            fail("DELTA ended into EMPTY.WAV (0 bytes) and nothing played on")
            i3 = A.mark(); A.click_view(100, row_y(5))
        # 6. FINAL.WAV ends the list, and the row of a stopped song shows no pause bars
        if A.wait(r"music: ended list", 12, i3)[0] is None:
            fail("the list never ended after FINAL.WAV with repeat off")
        else:
            time.sleep(0.6)
            img = A.frame()
            y = row_y(5)   # FINAL.WAV, the row that just stopped
            if any(near(px(img, x, y), ACCENT, 6) for x in (18, 19, 22, 23)):
                fail("a stopped song's row still shows the pause bars (it reads as paused)")

    # 5. a seek while playing, then the clock
    a = A.mark()
    A.click_view(100, row_y(3))                       # DELTA again, from the top
    if A.wait(r"music: playing DELTA\.MP3", 10, a)[0] is None: fail("a click on DELTA.MP3 did not start it")
    before = pos_lines(a, 2, 8)
    a = A.mark()
    A.key("right")
    i, m = A.wait(r"music: seek pos=(\d+)", 10, a)
    if i is None or not before: fail("the right arrow did not seek while playing")
    else:
        S = int(m.group(1))
        if not before[-1][0] + 3500 <= S <= before[-1][0] + 8500: fail(f"right arrow while playing moved from about {before[-1][0]} to {S} ms, not 5 s on")
        after = [p[0] for p in pos_lines(i, 3, 12)]
        print(f"  after the seek at {S}: {after}")
        if len(after) < 3 or any(not 600 <= after[k + 1] - after[k] <= 1600 for k in range(len(after) - 1)):
            fail(f"after a seek the position did not go on at one second a second: {after}")

    # 7. n after a click on a song that will not play
    a = A.mark()
    A.click_view(100, row_y(1))                       # BAD.WAV
    if A.wait(r"music: unsupported BAD\.WAV", 8, a)[0] is None: fail("a click on BAD.WAV did not say it will not play")
    a = A.mark()
    A.key("n")
    if A.wait(r"music: playing CORRUPT\.MP3", 8, a)[0] is None:
        fail("n after a song that will not play asked for the same song again instead of the next one")
    A.esc_closes()
finally:
    A.close()

print("music-qa-check: boot B (fourteen short songs)")
B = Boot("many", "many")
try:
    B.start(14)
    # 8. no row under the key hint: the old list drew an eleventh row at y = 306
    a = B.mark()
    B.click_view(100, LIST_Y + 10 * ROW_H + 8)
    if not B.quiet(r"music: playing", 1.5, a): fail("a click below the tenth row started a song: a row sits under the key hint")
    # 9. the wheel
    img0 = B.frame()
    a = B.mark()
    B.move(480, 8)                                    # over the menu bar, outside the window
    B.wheel("wheel-down"); B.wheel("wheel-down")
    if not B.quiet(r"music: list top=", 1.0, a): fail("the wheel scrolled the list with the pointer over the desktop")
    B.move(B.view[0] + 100, B.view[1] + 150); time.sleep(0.3)
    for _ in range(3): B.wheel("wheel-down")
    tops = [int(m.group(1)) for m in B.wait_n(r"music: list top=(\d+)", 3, 5, a)]
    print(f"  wheel down x3: list top {tops}")
    if tops != [1, 2, 3]: fail(f"three wheel steps down over the list gave tops {tops}, expected [1, 2, 3] (no window gets a wheel step, or the list snaps back)")
    else:
        img1 = B.frame()
        if all(near(px(img0, x, row_y(0)), px(img1, x, row_y(0)), 1) for x in range(30, 90)):
            fail("the list's first row looks the same after scrolling")
        a = B.mark()
        for _ in range(3): B.wheel("wheel-up")
        tops = [int(m.group(1)) for m in B.wait_n(r"music: list top=(\d+)", 3, 5, a)]
        if tops != [2, 1, 0]: fail(f"three wheel steps up gave tops {tops}, expected [2, 1, 0]")
    # 10. shuffle, repeat off: each song once, then the list ends
    B.key("s")
    if B.wait(r"music: shuffle=1", 3)[0] is None: fail("s did not turn shuffle on")
    a = B.mark()
    B.click_view(100, row_y(0))
    i, _ = B.wait(r"music: ended list", 60, a)
    plays = [re.search(r"music: playing (\S+)", l).group(1) for l in B.lines()[a:] if re.search(r"music: playing ", l)]
    print(f"  shuffle round: {len(plays)} songs, {len(set(plays))} different, list ended: {i is not None}")
    if i is None: fail(f"shuffle with repeat off never stopped: {len(plays)} songs played, {len(set(plays))} different")
    elif len(plays) != 14 or len(set(plays)) != 14: fail(f"a shuffle round played {len(plays)} songs, {len(set(plays))} different: expected all 14, once each")
    B.key("s")
    # 11. a half-screen window
    B.drag(480, 56, 3, 200)
    i, m = B.wait(r"music: ui size=(\d+)x(\d+)", 8)
    if i is None: fail("snapping the window left did not resize Music")
    else:
        vw, vh = int(m.group(1)), int(m.group(2)); time.sleep(0.8)
        B.view = (8, 26 + 32)                         # a half-screen window sits at x 0, just under the 26 pixel menu bar
        print(f"  half-screen window {vw}x{vh}")
        img = B.frame()
        found = []
        for y in range(40, vh - 40):
            for r in runs(img, B.view, 150, vw, y):
                if r not in found: found.append(r)
        widths = sorted(set(r[1] - r[0] + 1 for r in found))
        have_shuf = any(82 <= w <= 86 for w in widths); have_rep = any(102 <= w <= 106 for w in widths)
        if not (have_shuf and have_rep): fail(f"in a half-screen window the Shuffle and Repeat pills are not both whole (pill runs {widths}): they run off the right edge")
        elif any(r[1] >= vw - 2 for r in found): fail("a pill touches the window's right edge")
        else:
            rep = next(r for r in found if 102 <= r[1] - r[0] + 1 <= 106)
            ry = next(y for y in range(40, vh - 40) if rep in runs(img, B.view, 150, vw, y))
            a = B.mark()
            B.click_view((rep[0] + rep[1]) // 2, ry + 12)
            if B.wait(r"music: repeat=1", 5, a)[0] is None: fail("a click on the Repeat pill in a half-screen window did nothing")
    B.esc_closes()
finally:
    B.close()

print("music-qa-check: boot C (no songs)")
C = Boot("empty", "empty")
try:
    C.start(0)
    a = C.mark()
    C.key("n"); C.key("p"); C.key("spc"); C.key("right")
    if not C.quiet(r"music: (ended|playing|seek|paused)", 1.5, a): fail("keys on an empty library made the app say it played, ended or seeked")
    img = C.frame()
    bands, inband = 0, False
    for y in range(LIST_Y, LIST_Y + 120):
        ink = any(not near(px(img, x, y), BG, 24) for x in range(10, 160))
        if ink and not inband: bands += 1
        inband = ink
    print(f"  empty state: {bands} lines of text in the list pane")
    if bands < 4: fail(f"the empty library shows {bands} lines, expected four (a title and a three line hint that names MP3 as well as WAV)")
    C.esc_closes()
finally:
    C.close()

print("music-qa-check: boot D (an MP3 with an ID3 tag)")
D = Boot("tag", "tag")
try:
    a = D.mark()
    D.start(1)
    if D.wait(r"music: tag Never Gonna Give You Up", 5, a)[0] is None: fail("the library did not read the ID3 title of NEVERG~1.MP3 (it showed the 8.3 name)")
    if D.wait(r"music: artist Rick Astley", 5, a)[0] is None: fail("the library did not read the ID3 artist")
    D.key("spc")
    if D.wait(r"music: playing NEVERG~1\.MP3", 5, a)[0] is None: fail("the tagged MP3 did not play")
    if D.wait(r"music: art NEVERG~1\.MP3", 5, a)[0] is None: fail("the cover in the ID3 APIC frame was not decoded")
    time.sleep(1.0)
    img = D.frame(); c = px(img, D.bar[0] + 48, 94 + 48)   # the middle of the 96 px cover: the fixture's black disc
    if not near(c, (28, 28, 30), 40): fail(f"the cover's centre is {c}, expected the fixture's dark disc")
    D.esc_closes()
finally:
    D.close()

if fails:
    print("FAIL:")
    for f in fails: print("  - " + f)
    sys.exit(1)
print("PASS: Music QA: the queue stays under half a second ahead, an unplayable song is skipped, a damaged MP3 frame no longer hangs the song, "
      "a 3:25 MP3 reads 3:25 and seeks near its end, n moves on from a refused song, the list has no row under the hint, the wheel scrolls it, "
      "shuffle plays each song once, a half-screen window keeps its pills, an empty library is quiet, and an MP3 shows its ID3 title instead of its 8.3 name")
