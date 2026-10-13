#!/usr/bin/env python3
"""ARM64 app windows are sharp at 1080p (scale 2): text and shapes drawn at 2x, not a 1x picture smoothed up.

Before 3.9.0 the ring-3 apps drew on a 1x grid and main.c's app_blit() spread each pixel over its 2x2 by bilinear weights, so
every text stem was a soft ramp three or four pixels wide. Now Calendar, Notes, Music and Books ask for a device-size buffer
(JT_SYS_WINDOW_OPEN with the hi-res flag), draw their rectangles and circles at 2x, and the kernel rasterises their text with
Tree Sans at twice the size (JT_SYS_TEXT2), the pen moving by twice the 1x advance so the layout does not shift.

For calendar, notes, music and books this builds the apptest kernel (`make APP_NAME=x apptest`: that app opened at boot), boots it on
raspi4b at 1920x1080, takes a screendump through QMP and measures the content area. A text stem is a horizontal run of ink
pixels (anything off the background by 6+ levels) no wider than 6 pixels. Per run: "full" pixels are within 15% of the
run's darkest contrast, "soft" pixels are the ones between 15% and 85% of it. Passes when, per app:
  - there are at least 100 stems (40 for music and books, which show less text at boot): the app really drew text;
  - at least 85% of stems have at most 2 soft pixels (one soft edge a side: no blur ramp) and at least 50% have 2 or more
    full pixels (a stem two pixels wide at least, not a 1 px line spread thin);
  - the average stem has fewer than 2.3 soft pixels (a bilinear 1x->2x blit averages 3 or more).
Also covers finder, mail, weather, stocks, reminders, the four games and Settings. Pass --shots DIR to keep the screendumps
(PPM and PNG) there, --save FILE to keep the per-app shares as JSON and --before FILE to print a before/after table against such a file. Skips (exit 0) when clang, ld.lld, qemu-system-aarch64 or its
raspi4b model is missing.
Usage: tools/checks/arm64-sharp2x-check.py [--shots DIR]   (from the repo root)
"""
import atexit, json, os, re, shutil, socket, struct, subprocess, sys, tempfile, time, zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from scratch import scratch_dir

root = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..")
arch = os.path.join(root, "arch/arm64")
SW, SH = 1920, 1080
shots = sys.argv[sys.argv.index("--shots") + 1] if "--shots" in sys.argv else None
fails = []
EL0 = ("calendar", "notes", "music", "books", "finder", "mail", "weather", "stocks", "reminders", "mines", "toroid", "hamurabi", "keyrate")
ALL = EL0 + ("calculator", "clock", "brick", "browser", "terminal", "console", "settings")
FEW_STEMS = {"music": 40, "books": 40, "mines": 15, "toroid": 15, "hamurabi": 30, "keyrate": 15, "settings": 40}   # apps that show less text at boot
results = {}
if os.environ.get("JT_SHARP_APPS"): ALL = tuple(os.environ["JT_SHARP_APPS"].split(","))   # a subset, for a before/after on another tree
def check(name, ok, detail=""):
    print(("  ok: " if ok else "  FAIL: ") + name + ("" if ok or not detail else " -- " + str(detail)[-400:]))
    if not ok: fails.append(name)

def stems(img, x0, y0, w, h):
    """(count, share with <=2 soft pixels, share with >=2 full pixels, mean soft pixels) over the narrow ink runs in the box."""
    rows = []
    n = ok_soft = ok_full = soft_sum = 0
    from collections import Counter
    sample = Counter()
    for j in range(0, h, 3):
        o = ((y0 + j) * SW + x0) * 3
        sample.update(img[o:o + w * 3:3])   # the red lane is enough to find the page colour
    bg = None
    for j in range(h):
        o = ((y0 + j) * SW + x0) * 3
        row = [(img[o + i * 3] * 299 + img[o + i * 3 + 1] * 587 + img[o + i * 3 + 2] * 114) // 1000 for i in range(w)]
        base = Counter(row).most_common(1)[0][0]   # this row's background: its most common grey
        i = 0
        while i < w:
            if abs(row[i] - base) < 6: i += 1; continue
            k = i
            while k < w and abs(row[k] - base) >= 6: k += 1
            run = row[i:k]; i = k
            if len(run) > 6: continue
            peak = max(abs(v - base) for v in run)
            if peak < 60: continue            # a faint tint, not a text stem
            full = sum(1 for v in run if abs(v - base) >= peak * 0.85)
            soft = sum(1 for v in run if 0.15 * peak < abs(v - base) < 0.85 * peak)
            n += 1; soft_sum += soft; ok_soft += soft <= 2; ok_full += full >= 2
    return n, (ok_soft / n if n else 0), (ok_full / n if n else 0), (soft_sum / n if n else 9)

def png(path, ppm, w, h):
    raw = b"".join(b"\0" + ppm[y * w * 3:(y + 1) * w * 3] for y in range(h))
    def chunk(t, d): return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xFFFFFFFF)
    open(path, "wb").write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)) + chunk(b"IDAT", zlib.compress(raw, 6)) + chunk(b"IEND", b""))

def run(app):
    for f in os.listdir(arch):
        if f.startswith("app-"): os.remove(os.path.join(arch, f))
    if subprocess.run(["make", "-C", arch, "APP_NAME=" + app, "apptest"], capture_output=True).returncode:
        check(app + ": `make apptest` builds", False); return
    tmp = scratch_dir("arm64-sharp2x-" + app)
    log, shot = tmp + "/uart", tmp + "/shot.ppm"
    # A unix socket path must fit in 104 bytes on macOS. Inside the suite's own TMPDIR (/var/folders/.../T/jt-suite-XXXXXX/)
    # the scratch dir for a longer app name (reminders, hamurabi, calculator) runs past that and connect() raises
    # "AF_UNIX path too long", so the QMP socket lives in a short directory of its own under /tmp.
    sockdir = tempfile.mkdtemp(prefix="jt-s2x-", dir="/tmp")
    atexit.register(shutil.rmtree, sockdir, ignore_errors=True)
    sock = sockdir + "/qmp"
    q = subprocess.Popen(["qemu-system-aarch64", "-machine", "raspi4b", "-serial", "null", "-display", "none", "-serial", "file:" + log,
                          "-global", "bcm2835-fb.xres=%d" % SW, "-global", "bcm2835-fb.yres=%d" % SH, "-qmp", "unix:%s,server,nowait" % sock,
                          "-kernel", os.path.join(arch, "app-kernel8.img")], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    def uart(): return open(log, errors="replace").read() if os.path.exists(log) else ""
    try:
        el0 = app in EL0
        mark = (app + " first frame") if el0 else ("WINBOX %s %s" % (app, "")) 
        end = time.time() + 40
        while time.time() < end and not (re.search(r"WINBOX %s \d+ \d+ \d+ \d+\n" % app, uart()) if not el0 else mark in uart()): time.sleep(0.1)
        out = uart()
        if el0:
            m = re.search(r"M3c window %s (\d+)x(\d+) at (\d+),(\d+)" % app, out)
            ok = bool(m) and mark in out
            if ok: cw, ch, cx, cy = map(int, m.groups())
        else:   # a kernel-drawn window: its frame is on the UART, the content is inside the title band and the margins
            m = re.search(r"WINBOX %s (\d+) (\d+) (\d+) (\d+)" % app, out)
            ok = bool(m)
            if ok: fx, fy, fw, fh = map(int, m.groups()); cx, cy, cw, ch = fx + 8, fy + 32, fw - 16, fh - 40
        check(app + ": the window opens and is drawn", ok, out[-300:])
        if not ok: return
        time.sleep(1.0)
        s_ = socket.socket(socket.AF_UNIX); s_.connect(sock); f = s_.makefile("rw"); f.readline()
        def cmd(c, **a):
            f.write(json.dumps({"execute": c, "arguments": a}) + "\n"); f.flush()
            while True:
                r = json.loads(f.readline())
                if "return" in r or "error" in r: return r
        cmd("qmp_capabilities")
        if "error" in cmd("screendump", filename=shot): check(app + ": screendump", False); return
        time.sleep(0.5)
        parts = open(shot, "rb").read().split(b"\n", 3)
        w, h = map(int, parts[1].split())
        check(app + ": the screen is %dx%d" % (SW, SH), (w, h) == (SW, SH), (w, h))
        if (w, h) != (SW, SH): return
        if shots:
            os.makedirs(shots, exist_ok=True)
            open("%s/%s.ppm" % (shots, app), "wb").write(b"P6\n%d %d\n255\n" % (w, h) + parts[3])
            png("%s/%s.png" % (shots, app), parts[3], w, h)
        n, soft_ok, full_ok, soft_avg = stems(parts[3], cx * 2, cy * 2, cw * 2, ch * 2)
        results[app] = soft_ok
        print("  %s: crisp-stem share %.0f%% (%d stems)" % (app, soft_ok * 100, n))
        check(app + ": %d text stems measured" % n, n >= FEW_STEMS.get(app, 100 if app in ("calendar", "notes", "finder", "mail", "reminders", "stocks") else 40))
        check(app + ": %.0f%% of stems have at most 2 soft pixels (a blur ramp has 3 or more)" % (soft_ok * 100), soft_ok >= 0.85, "%.2f" % soft_ok)
        check(app + ": %.0f%% of stems are 2 or more full pixels wide" % (full_ok * 100), full_ok >= 0.50, "%.2f" % full_ok)
        check(app + ": a stem averages %.2f soft pixels" % soft_avg, soft_avg < 2.3, "%.2f" % soft_avg)
    finally:
        q.terminate(); q.wait()

if __name__ == "__main__":
    if not all(shutil.which(t) for t in ("clang", "ld.lld", "qemu-system-aarch64")):
        print("SKIP: clang, ld.lld or qemu-system-aarch64 not installed"); sys.exit(0)
    if "raspi4b" not in subprocess.run(["qemu-system-aarch64", "-machine", "help"], capture_output=True, text=True).stdout:
        print("SKIP: this QEMU has no raspi4b model (needs QEMU 9 or newer)"); sys.exit(0)
    for a in ALL:
        run(a)
    subprocess.run(["make", "-C", arch, "clean"], capture_output=True)
    print("shares: " + ", ".join("%s %.0f%%" % (kv[0], kv[1] * 100) for kv in results.items()))
    if "--save" in sys.argv: json.dump(results, open(sys.argv[sys.argv.index("--save") + 1], "w"))
    if "--before" in sys.argv:   # a table against an earlier --save: the same apps on the tree before the change
        before = json.load(open(sys.argv[sys.argv.index("--before") + 1]))
        print("%-12s %7s %7s" % ("app", "before", "after"))
        for a, v in results.items(): print("%-12s %6.0f%% %6.0f%%" % (a, before.get(a, 0) * 100, v * 100))
    if fails: print("FAIL: %d: %s" % (len(fails), "; ".join(fails))); sys.exit(1)
    print("ok: every app window is crisp at scale 2")
