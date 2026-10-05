#!/usr/bin/env python3
"""Windgate (guided breathing, from ~/Documents/Code/breathe) runs as a ring-3 window: the circle breathes, the
timings are the web app's exact ones, pause holds it, and Esc closes it (2.8).

Boots headless with `open=windgate`, which launches Windgate from the dock path the moment the desktop is up.
Windgate is user/windgate.c, a flat binary loaded off the VFS and run at CPL 3 through the table-driven launcher
(kernel/ring3app.c, RING3_APPS). The check then:

  0. reads the sources: the app is named Windgate in APPS[], RING3_APPS and Samantha's list, it has no network
     call, and its four presets are the web app's (Box 4-4-4-4, 4-7-8, Relax 4-0-6-0, Coherent 5-0-5-0);
  1. asserts the window opened (804x345, the dock viewport) and that the program reported where its circle and
     buttons are;
  2. dumps the window region and measures the circle off the pixels (the run of accent-blue pixels along the row
     through its centre). At rest it is the small circle;
  3. KEYS. For each preset (1, Right, 3, 4 keys; then Right wraps and Left wraps back) it checks the program's
     "preset <n> selected" line, starts it with Space (once) and follows one whole breath cycle off the serial
     lines "preset <n> phase <name> secs <s> round <r> at <ms>":
       - the phase names, their order and their seconds are exactly the web app's pattern;
       - each line's "at" (exercise time, ms) is exactly the sum of the phase lengths before it;
       - the line arrives when the exercise clock says it should: since the last resume, wall time equals
         (at - exercise time at the resume) within 0.6 s (host polling and QEMU timer jitter only);
     and measures the circle in pixels: at the start of the in-breath it is small, at the end of it
     (the moment the next phase begins) it is clearly larger (at least 75 percent of the way to the large size),
     two frames inside the in-breath are strictly increasing, and after the out-breath it is small again;
  4. PAUSE. In Box, mid in-breath, Space pauses: "paused at <ms>" is logged, two frames 1.5 s apart are
     pixel-identical, no phase line arrives for 5 s (longer than the rest of the phase), and the Start button reads
     Resume. Clicking the Start button resumes it with the same exercise time, and the in-breath then ends when
     the exercise clock (not the wall clock) says so;
  5. CLICKS. Each of the four preset buttons is clicked: the same "selected" line, the same first phase line, the
     button is drawn filled in the accent colour and the other three are not; clicking Start pauses and resumes;
  6. screenshots /tmp/jt-windgate-<preset>-inhale-full.png, -exhale-empty.png for each preset, -ready.png and
     -paused.png (also copied to $JT_SHOTS_DIR when it is set);
  7. Esc: the program exits 0, releases its window, and the desktop still takes a click (Mail opens from the dock).

Discriminating: make the in-breath not grow the circle (radius16 returns the small size for phase 0) and step 3 fails
on the radius; change a preset's seconds in P_SECS and step 3 fails on the secs and the at; let exercise time run
while paused (drop the `if (running)` around `ex_ms +=`) and step 4 fails; halve the pace and the wall-clock check in
step 3 fails.

Usage: tools/checks/ring3windgate-check.py   (from the repo root, after make kernel.elf)
"""
import json, os, re, shutil, socket, subprocess, sys, threading, time
from PIL import Image
from freeport import free_port

LOG = "/tmp/jt-ring3windgate-serial.log"
DUMP = "/tmp/jt-ring3windgate.raw"
SHOTS = {}
FB = 0xfd000000; W, H = 1920, 1080
PORT = free_port()
LOGICAL_W, LOGICAL_H, SCALE = 960, 540, 2
DOCK_ICON, DOCK_GAP, SLOT0_X = 37, 6, 247
PITCH = DOCK_ICON + DOCK_GAP
ICON_ROW_Y = 487
CLOSE_X, CLOSE_Y = 94, 56
CLOSE_RED = (0xFF, 0x5F, 0x57)
VIEW_X, VIEW_Y = 78, 72   # gui_launch_from_dock: viewport at (x+8, y+32) for x=70, y=40
VIEW_W, VIEW_H = 804, 345
PARK = (480, 452)         # on the desktop, below the window and above the dock: the pointer is not in any picture
ACCENT = (0x3F, 0x6F, 0xD8)
PANEL = (0xDB, 0xE6, 0xF7)
WALL_SLACK = 0.6          # seconds: host polling plus QEMU timer jitter, nothing else

# The web app's presets (breathe/web/index.html data-pattern): name, seconds in, hold, out, hold.
PRESETS = [("Box", (4, 4, 4, 4)), ("4-7-8", (4, 7, 8, 0)), ("Relax", (4, 0, 6, 0)), ("Coherent", (5, 0, 5, 0))]
PHASES = ["inhale", "hold-full", "exhale", "hold-empty"]
SLUG = ["box", "478", "relax", "coherent"]

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..")
os.chdir(ROOT)
for f in (LOG, DUMP):
    try: os.remove(f)
    except FileNotFoundError: pass

q = subprocess.Popen(["qemu-system-i386", "-kernel", "kernel.elf", "-append", "open=windgate",
                      "-display", "none", "-vga", "std",
                      "-qmp", f"tcp:127.0.0.1:{PORT},server,nowait", "-serial", "file:" + LOG],
                     stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
fails = []
NOISE = {"present", "menubarredraw", "fullrepaint", "mwchrome", "wxfetch"}
ALL = []            # (arrival time, line), stamped by a reader thread so a slow frame dump never delays a stamp
stop = threading.Event()
def reader():
    pos, buf = 0, b""
    while not stop.is_set():
        try:
            with open(LOG, "rb") as f:
                f.seek(pos); data = f.read()
        except OSError:
            data = b""
        if data:
            pos += len(data); buf += data
            *whole, buf = buf.split(b"\n")
            t = time.time()
            for l in whole:
                l = l.decode(errors="replace").replace("\r", "")
                if l not in NOISE: ALL.append((t, l))
        time.sleep(0.01)
threading.Thread(target=reader, daemon=True).start()
def serial(): return "\n".join(l for _, l in list(ALL)) + "\n"
def wait_serial(needle, secs):
    end = time.time() + secs
    while time.time() < end:
        if needle in serial(): return True
        time.sleep(0.05)
    return False
def wait_line(pattern, start, secs):
    """First app line matching pattern at index >= start: (index, arrival time, match), waiting up to secs."""
    rx = re.compile(r"windgate: " + pattern)
    end = time.time() + secs
    while True:
        snap = list(ALL)
        for i in range(start, len(snap)):
            m = rx.search(snap[i][1])
            if m: return i, snap[i][0], m
        if time.time() > end: return None
        time.sleep(0.01)
def count(pattern, start=0):
    rx = re.compile(r"windgate: " + pattern)
    return sum(1 for _, l in list(ALL)[start:] if rx.search(l))

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
    def window(img):
        return img.crop((VIEW_X * SCALE, VIEW_Y * SCALE, (VIEW_X + VIEW_W) * SCALE, (VIEW_Y + VIEW_H) * SCALE))
    def near(p, c, tol=12): return max(abs(p[i] - c[i]) for i in range(3)) <= tol
    def shot(name, win):
        path = f"/tmp/jt-windgate-{name}.png"
        win.save(path); SHOTS[name] = path
        d = os.environ.get("JT_SHOTS_DIR")
        if d:
            try: os.makedirs(d, exist_ok=True); shutil.copyfile(path, os.path.join(d, os.path.basename(path)))
            except OSError as e: print(f"(could not copy {name} to {d}: {e})")
    def radius(win):
        """The circle's radius in window pixels, off the dump: the run of accent pixels along the row through its centre."""
        y = CY * SCALE + 1
        xs = [x for x in range(win.width) if near(win.getpixel((x, y)), ACCENT, 10)]
        if not xs: return 0.0
        lo = hi = CX * SCALE
        while lo - 1 >= 0 and near(win.getpixel((lo - 1, y)), ACCENT, 10): lo -= 1
        while hi + 1 < win.width and near(win.getpixel((hi + 1, y)), ACCENT, 10): hi += 1
        return (hi - lo + 1) / 2 / SCALE
    def grab():
        move(*PARK); time.sleep(0.1)
        return window(frame())

    # 0. names and presets, off the sources
    kernel_c = open("kernel/kernel.c").read()
    apps_tab = re.search(r"struct app APPS\[GUI_APP_COUNT\]\s*=\s*\{(.*?)\n\};", kernel_c, re.S)[1]
    app_names = re.findall(r'\{"([^"]*)",', apps_tab)
    r3_names = re.findall(r'\{"([^"]+)",\s*user_', open("kernel/ring3app.c").read())
    sam_names = re.findall(r'"([^"]+)"', re.search(r"APPNAME\[\] = \{(.*?)\};", open("user/samantha.c").read(), re.S)[1])
    for what, names in (("APPS[]", app_names), ("RING3_APPS", r3_names), ("Samantha's APPNAME", sam_names)):
        if "Windgate" not in names: fails.append(f"{what} has no 'Windgate' row")
    src = open("user/windgate.c").read()
    if re.search(r"jt_http|SYS_HTTP|JT_POST", src): fails.append("user/windgate.c calls the network: it must run completely offline")
    m = re.search(r"P_SECS\[4\]\[4\] = \{(.*?)\};", src, re.S)
    got = [tuple(int(x) for x in g.split(",")) for g in re.findall(r"\{([\d, ]+)\}", m[1])] if m else []
    if got != [p[1] for p in PRESETS]: fails.append(f"user/windgate.c presets {got} are not the web app's {[p[1] for p in PRESETS]}")

    # 1. the program is up, has its window, and says where things are
    if not wait_serial("ring3app: launching WINDGATE.BIN at ring 3", 40):
        fails.append("Windgate was never launched as a ring-3 program (open=windgate flag or ring3app.c broken)")
    if not wait_serial("syscall: window opened for ring-3 task", 10):
        fails.append("SYS_WINDOW_OPEN never succeeded from ring 3")
    if not wait_serial(f"windgate: ring-3 window {VIEW_W}x{VIEW_H}", 10):
        fails.append(f"the program did not report the app viewport's size (expected {VIEW_W}x{VIEW_H}) through write()")
    if not wait_serial("windgate: layout", 20):
        print(serial()[-1500:]); raise SystemExit("FAIL: the program never reported its layout: it did not finish its first frame")
    lay = re.search(r"windgate: layout circle (\d+),(\d+) rmin (\d+) rmax (\d+) presets ((?:\d+,\d+,\d+,\d+ ?){4}) start (\d+),(\d+),(\d+),(\d+)", serial())
    if not lay: print(serial()[-1500:]); raise SystemExit("FAIL: no layout report on the serial port")
    CX, CY, RMIN, RMAX = (int(lay[i]) for i in (1, 2, 3, 4))
    PB = [tuple(int(v) for v in b.split(",")) for b in lay[5].split()]
    SB = tuple(int(lay[i]) for i in (6, 7, 8, 9))
    print(f"circle at ({CX},{CY}) rmin {RMIN} rmax {RMAX}; preset buttons {PB}; start {SB}")
    if "BUG" in serial() or "no heap" in serial() or "no window" in serial():
        fails.append("the program or ring3app logged a BUG / no heap / no window line")
    def centre(b): return VIEW_X + b[0] + b[2] // 2, VIEW_Y + b[1] + b[3] // 2
    def button_fill(win, i):   # a pixel inside preset button i, near its left edge (clear of the text)
        b = PB[i]; return win.getpixel(((b[0] + 8) * SCALE + 1, (b[1] + b[3] // 2) * SCALE + 1))
    def start_fill(win): return win.getpixel(((SB[0] + 8) * SCALE + 1, (SB[1] + SB[3] // 2) * SCALE + 1))

    # the program learns the clock's rate off SYS_TIME in under two seconds, and says so; the exercise counts after that
    if not wait_serial("windgate: clock ready ticks-per-second", 10): fails.append("the program never said its clock is ready")

    # 2. a real picture, and the circle at rest is the small one
    time.sleep(0.5)
    win0 = grab()
    shot("ready", win0)
    ncol = len(win0.getcolors(maxcolors=W * H) or [])
    r0 = radius(win0)
    print(f"distinct colours {ncol}; resting radius {r0:.1f} (small size {RMIN}, large size {RMAX})")
    if ncol < 12: fails.append(f"only {ncol} distinct colours in the window: it is (nearly) blank")
    if abs(r0 - RMIN) > 2: fails.append(f"at rest the circle measures {r0:.1f}, not the small size {RMIN}")
    if not near(button_fill(win0, 0), ACCENT, 6): fails.append("the Box button is not drawn selected at launch")
    for i in (1, 2, 3):
        if not near(button_fill(win0, i), PANEL, 6): fails.append(f"preset button {i} is not drawn idle at launch")

    # ---- following the breath off the serial lines and the pixels ----
    clock = {"wall": None, "ex": 0}      # the last (wall time, exercise ms) pair at which the exercise clock is known
    def sleep_until(t): 
        d = t - time.time()
        if d > 0: time.sleep(d)
    def check_wall(label, t, at):
        """A phase line must arrive when the exercise clock says: wall time since the reference == exercise time since it."""
        due = clock["wall"] + (at - clock["ex"]) / 1000.0
        if abs(t - due) > WALL_SLACK:
            fails.append(f"{label}: arrived {t - due:+.2f}s from when the exercise clock says (limit {WALL_SLACK}s)")
    def follow(p, idx, shots=True):
        """Follow preset p from line index idx through one whole breath cycle and into the next round's in-breath,
        checking each line against the web app's pattern and the circle against the pixels."""
        name, secs = PRESETS[p]
        seq = [(k, secs[k]) for k in range(4) if secs[k]]
        cyc = sum(secs) * 1000
        expect = [(PHASES[k], s_, 0, sum(secs[:k]) * 1000) for k, s_ in seq] + [(PHASES[0], secs[0], 1, cyc)]
        for j, (pname, s_, rnd, at) in enumerate(expect):
            r = wait_line(rf"preset {p} phase (\S+) secs (\d+) round (\d+) at (\d+)", idx, s_ + 6)
            if not r:
                fails.append(f"{name}: no phase line #{j} ({pname}) within {s_ + 6}s"); return
            idx, t, mm = r[0] + 1, r[1], r[2]
            got = (mm[1], int(mm[2]), int(mm[3]), int(mm[4]))
            if got != (pname, s_, rnd, at):
                fails.append(f"{name}: phase line #{j} is {got}, the web app's pattern says {(pname, s_, rnd, at)}")
            if j == 0: clock["wall"], clock["ex"] = t, 0     # exercise time 0 is this very line
            else: check_wall(f"{name} '{pname}' (at {at} ms)", t, at)
            time.sleep(0.15)    # let the program's next frame (which carries the new phase word) reach the screen
            win = grab(); rr = radius(win)
            if j == 0:
                if rr > RMIN + 0.25 * (RMAX - RMIN):
                    fails.append(f"{name}: at the start of the in-breath the circle already measures {rr:.1f} (small {RMIN}, large {RMAX})")
                sleep_until(t + 0.3 * s_); r1 = radius(grab())
                sleep_until(t + 0.6 * s_); r2 = radius(grab())
                print(f"{name} in-breath radii: {rr:.1f} -> {r1:.1f} -> {r2:.1f}")
                if not (rr < r1 < r2): fails.append(f"{name}: the circle is not growing through the in-breath: {rr:.1f} -> {r1:.1f} -> {r2:.1f}")
            else:
                prior = seq[j - 1][0]
                if prior == 0:      # the in-breath just ended
                    big = RMIN + 0.75 * (RMAX - RMIN)
                    print(f"{name} end of in-breath: radius {rr:.1f}")
                    if rr < big: fails.append(f"{name}: at the end of the in-breath the circle measures only {rr:.1f} (needs {big:.1f}, large size {RMAX})")
                    if shots: shot(f"{SLUG[p]}-inhale-full", win)
                if prior == 2:      # the out-breath just ended
                    small = RMIN + 0.25 * (RMAX - RMIN)
                    print(f"{name} end of out-breath: radius {rr:.1f}")
                    if rr > small: fails.append(f"{name}: after the out-breath the circle still measures {rr:.1f} (needs at most {small:.1f}, small size {RMIN})")
                    if shots: shot(f"{SLUG[p]}-exhale-empty", win)

    def select(how, p, via):
        """Pick preset p with a key or a click; return the serial index from where its lines will be."""
        pos = len(ALL)
        if via == "key": keys(how)
        else: move(*centre(PB[p])); time.sleep(0.2); click(); move(*PARK)
        r = wait_line(rf"preset {p} selected (\S+) pattern (\d+),(\d+),(\d+),(\d+)", pos, 5)
        if not r:
            fails.append(f"{via} {how}: no 'preset {p} selected' line"); return pos
        g = r[2].groups()
        if g[0] != PRESETS[p][0] or tuple(int(x) for x in g[1:5]) != PRESETS[p][1]:
            fails.append(f"{via} {how}: selected {g[0]} {g[1:5]}, expected {PRESETS[p]}")
        time.sleep(0.4)
        win = grab()
        for i in range(4):
            want = ACCENT if i == p else PANEL
            if not near(button_fill(win, i), want, 6): fails.append(f"{via} {how}: preset button {i} is drawn {button_fill(win, i)}, expected {want}")
        return pos

    # 4. START and PAUSE, in Box
    pos = len(ALL)
    keys("spc")
    r = wait_line(r"running at (\d+)", pos, 5)
    first = wait_line(r"preset 0 phase inhale secs 4 round 0 at 0", pos, 5)
    if not r or not first or int(r[2][1]) != 0:
        fails.append("Space did not start the exercise at 0 (no 'running at 0' / 'phase inhale ... at 0')")
        raise SystemExit("FAIL:\n  - " + "\n  - ".join(fails))
    clock["wall"], clock["ex"] = first[1], 0
    sleep_until(first[1] + 1.3)
    pos2 = len(ALL)
    keys("spc")
    pr = wait_line(r"paused at (\d+)", pos2, 4)
    if not pr: raise SystemExit("FAIL: Space did not pause (no 'paused at' line)")
    ex_p = int(pr[2][1])
    time.sleep(0.4)
    wa = grab(); ra = radius(wa); shot("paused", wa)
    time.sleep(1.5)
    wb = grab(); rb = radius(wb)
    print(f"paused at {ex_p} ms: radius {ra:.1f}, then {rb:.1f} 1.5 s later; frames identical: {wa.tobytes() == wb.tobytes()}")
    if not (1000 <= ex_p <= 1900): fails.append(f"paused at {ex_p} ms of exercise time, expected about 1300")
    if wa.tobytes() != wb.tobytes(): fails.append("while paused the window changed between two frames 1.5 s apart")
    if not (RMIN + 2 < ra < RMAX - 2): fails.append(f"paused mid in-breath but the circle measures {ra:.1f}")
    time.sleep(3.0)   # the in-breath had about 2.7 s left: if the exercise clock kept running a phase line would be here
    if count(r"preset 0 phase hold-full", pos2): fails.append("a phase line arrived while paused: the exercise clock kept running")
    pos3 = len(ALL)
    move(*centre(SB)); time.sleep(0.2); click(); move(*PARK)     # resume with the Start button
    rr_ = wait_line(r"running at (\d+)", pos3, 4)
    if not rr_: raise SystemExit("FAIL: clicking the Start button did not resume (no 'running at' line)")
    if int(rr_[2][1]) != ex_p: fails.append(f"resumed at {rr_[2][1]} ms but paused at {ex_p} ms: the pause lost or gained exercise time")
    clock["wall"], clock["ex"] = rr_[1], int(rr_[2][1])
    hf = wait_line(r"preset 0 phase hold-full secs 4 round 0 at 4000", pos3, 8)
    if not hf: fails.append("after resuming, the in-breath never ended at exercise time 4000")
    else: check_wall("Box after resume: 'hold-full' (at 4000 ms)", hf[1], 4000)

    # 3. KEYS: each preset by key, one whole cycle each
    for key, p in (("1", 0), ("right", 1), ("3", 2), ("4", 3)):
        pos = select(key, p, "key")
        follow(p, pos)
    # Right wraps from the last preset to the first, Left back again
    pos = select("right", 0, "key"); pos = select("left", 3, "key")

    # 5. CLICKS: each preset button, then the Start button pauses and resumes
    for p in (2, 0, 3, 1):
        pos = select("button", p, "click")
        first = wait_line(rf"preset {p} phase inhale secs {PRESETS[p][1][0]} round 0 at 0", pos, 5)
        if not first: fails.append(f"click on preset {p}: it did not begin its in-breath at 0"); continue
        clock["wall"], clock["ex"] = first[1], 0
        nxt = [k for k in range(1, 4) if PRESETS[p][1][k]][0]
        sec = wait_line(rf"preset {p} phase {PHASES[nxt]} secs {PRESETS[p][1][nxt]} round 0 at {PRESETS[p][1][0] * 1000}", pos, 12)
        if not sec: fails.append(f"click on preset {p}: the phase after the in-breath ({PHASES[nxt]}) never came")
        else: check_wall(f"click preset {p}: {PHASES[nxt]}", sec[1], PRESETS[p][1][0] * 1000)
    pos = len(ALL)
    move(*centre(SB)); time.sleep(0.2); click(); move(*PARK)
    pr = wait_line(r"paused at (\d+)", pos, 4)
    if not pr: fails.append("clicking the Start button while running did not pause")
    else:
        time.sleep(0.4); w = grab(); print(f"Start button drawn while paused: fill {start_fill(w)}")
        if not near(start_fill(w), ACCENT, 6): fails.append('the Start button is not drawn while paused')
        pos = len(ALL); move(*centre(SB)); time.sleep(0.2); click(); move(*PARK)
        rr_ = wait_line(r"running at (\d+)", pos, 4)
        if not rr_ or rr_[2][1] != pr[2][1]: fails.append("clicking Start again did not resume at the same exercise time")

    print("screenshots: " + ", ".join(f"{k}={v}" for k, v in SHOTS.items()))
    for need in ["ready", "paused"] + [f"{sl}-{w_}" for sl in SLUG for w_ in ("inhale-full", "exhale-empty")]:
        if need not in SHOTS: fails.append(f"no '{need}' screenshot was taken")

    # 7. Esc closes it, cleanly, and the desktop answers
    move(*PARK); time.sleep(0.3)
    exits = serial().count("WINDGATE.BIN exited 0")
    keys("esc")
    if not wait_serial("windgate: closed", 5): fails.append("Esc did not reach the program")
    if not wait_serial("syscall: window released, task gone", 5): fails.append("the window was not released when the program exited")
    for _ in range(50):
        if serial().count("WINDGATE.BIN exited 0") > exits: break
        time.sleep(0.1)
    else:
        fails.append("the program did not exit 0 on Esc")
    if "exception: ring-0" in serial() or "panic in" in serial():
        fails.append("the KERNEL faulted while Windgate ran or closed")
    time.sleep(0.6)
    fr = frame()
    dock = fr.getpixel((480 * SCALE + 1, 511 * SCALE + 1))
    print(f"dock tray after Esc: {dock}")
    if dock != (0xEF, 0xEB, 0xE4): fails.append(f"desktop dock not on screen after Esc (got {dock})")
    if near(fr.getpixel((CLOSE_X * SCALE + 1, CLOSE_Y * SCALE + 1)), CLOSE_RED): fails.append("an app window is still open after Esc")
    move(SLOT0_X + 2 * PITCH + DOCK_ICON // 2, ICON_ROW_Y); time.sleep(0.3); click()
    opened = False
    for _ in range(40):
        time.sleep(0.1)
        if near(frame().getpixel((CLOSE_X * SCALE + 1, CLOSE_Y * SCALE + 1)), CLOSE_RED): opened = True; break
    print(f"Mail opens from the dock after Esc: {'yes' if opened else 'NO'}")
    if not opened: fails.append("Mail did not open from a dock click after Windgate closed: desktop stuck")
    keys("esc"); time.sleep(0.8)
finally:
    stop.set()
    q.terminate()
    try: q.wait(5)
    except subprocess.TimeoutExpired: q.kill()

if fails:
    print("FAIL:")
    for x in fails: print("  - " + x)
    print("--- serial tail ---")
    print("\n".join(serial().splitlines()[-40:])[-2500:])
    sys.exit(1)
print(f"PASS: Windgate ran at ring 3: the circle grew on every in-breath and shrank on every out-breath (measured off the framebuffer), all four presets (by key and by click) logged the web app's exact phases and seconds on the exercise clock, pause held the circle and the clock, and Esc closed it with the desktop alive. Screenshots: {', '.join(SHOTS.values())}")
