#!/usr/bin/env python3
"""Calendar month/day glyphs and the Clock window, including December and unset time, and the Calendar app's circled day.
The app must circle the local day the Clock names: at 06:20 UTC on 12 October it is 23:20 on the 11th in Vancouver. The
`--mutate` flag puts the app's grid back on UTC (SYS_TIME unshifted) and must make the check fail.
QEMU uses fixed dates, a real keyboard, and framebuffer assertions; no paid API call."""
import datetime, os, re, shutil, subprocess, sys, tempfile, time

root = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."); arch = os.path.join(root, "arch", "arm64")
if not all(shutil.which(t) for t in ("clang", "ld.lld", "qemu-system-aarch64")):
    print("SKIP: clang, ld.lld or qemu-system-aarch64 not installed"); sys.exit(0)

def geom(w, h):   # kernel/dock_geom.c at this screen (800x600 under virt: scale 1)
    s = 2 if h >= 1080 else 1; lw, lh = w // s, h // s
    icon = max(16, min(lh * 7 // 100, (min(740, lw - 40) - 2 * 10 - 10 * 6) // 11))
    dock_w = 11 * icon + 10 * 6 + 2 * 10; x0 = (lw - dock_w) // 2; y0 = lh - icon - 2 * 10 - 24
    return s, icon, x0, y0

def boot_shot(utc, calendar=False):
    for f in ("cal-main.o", "cal-kernel8.elf"):
        try: os.remove(os.path.join(arch, f))
        except FileNotFoundError: pass
    if subprocess.run(["make", "-C", arch, "cal-kernel8.elf", "CAL_UTC=%d" % utc], capture_output=True, text=True, timeout=900).returncode:
        raise SystemExit("FAIL: make -C arch/arm64 cal-kernel8.elf failed")
    tmp = tempfile.mkdtemp(); log = tmp + "/uart"; sock = tmp + "/qmp"
    q = subprocess.Popen(["qemu-system-aarch64", "-machine", "virt", "-cpu", "cortex-a72", "-m", "256", "-nic", "none",
                          "-global", "virtio-mmio.force-legacy=false", "-device", "ramfb", "-device", "virtio-keyboard-device", "-display", "none", "-serial", "file:" + log, "-qmp", "unix:%s,server,nowait" % sock,
                          "-kernel", os.path.join(arch, "cal-kernel8.elf")], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        for _ in range(600):
            if os.path.exists(log) and "M1c fb ok" in open(log, errors="replace").read(): break
            time.sleep(0.1)
        else: raise SystemExit("FAIL: the desktop did not come up")
        time.sleep(0.5)
        import json, socket
        s_ = socket.socket(socket.AF_UNIX); s_.settimeout(10); s_.connect(sock); f = s_.makefile("rw"); f.readline()
        def command(c):
            f.write(json.dumps(c) + "\n"); f.flush()
            while True:
                r = json.loads(f.readline())
                if "error" in r: raise AssertionError(r)
                if "return" in r: return
        def key(k):
            command({"execute": "send-key", "arguments": {"keys": [{"type": "qcode", "data": k}], "hold-time": 50}})
            time.sleep(.12)
        command({"execute": "qmp_capabilities"})
        command({"execute": "screendump", "arguments": {"filename": tmp + "/shot.ppm"}})
        key("f1")
        for k in "clock": key(k)
        key("ret")
        time.sleep(.3)
        uart = open(log, errors="replace").read()
        assert "clock open" in uart, uart[-1000:]
        if utc:
            local = datetime.datetime.fromtimestamp(utc, datetime.timezone.utc) - datetime.timedelta(hours=7)
            expected = local.strftime("%I:%M %p").lstrip("0") + " " + local.strftime("%b ") + str(local.day) + local.strftime(", %Y")
            assert "clock: " + expected in uart, (expected, uart[-1000:])
        else: assert "clock: --:-- Waiting for network time" in uart
        command({"execute": "screendump", "arguments": {"filename": tmp + "/clock.ppm"}})
        clock_image = open(tmp + "/clock.ppm", "rb").read()
        assert clock_image != open(tmp + "/shot.ppm", "rb").read(), "Clock did not draw"
        key("esc")
        assert "clock closed" in open(log, errors="replace").read()
        circled = None
        if calendar:   # the Calendar app, from Spotlight: which day cell holds its accent circle
            n = len(open(log, errors="replace").read())
            key("f1")
            for k in "calendar": key(k)
            key("ret")
            for _ in range(100):
                u = open(log, errors="replace").read()[n:]
                if "M3c window calendar" in u and "calendar: today" in u: break
                time.sleep(0.1)
            else: raise AssertionError("Calendar did not open: " + u[-600:])
            time.sleep(0.5)
            cw, ch, cx0, cy0 = map(int, re.search(r"M3c window calendar (\d+)x(\d+) at (\d+),(\d+)", u).groups())
            command({"execute": "screendump", "arguments": {"filename": tmp + "/app.ppm"}})
            time.sleep(0.3)
            parts = open(tmp + "/app.ppm", "rb").read().split(b"\n", 3); sw = int(parts[1].split()[0]); pix = parts[3]
            y, mo = 2026, 10   # the grid opens on today's month
            first = (datetime.date(y, mo, 1).weekday() + 1) % 7; rows = (first + 31 + 6) // 7
            cell_w = min(72, (cw - 16) // 7); gx = (cw - 7 * cell_w) // 2; gy = -32 + 150
            cell_h = max(32, min(44, (ch - gy - 6) // rows))
            pts = [(x, yy) for yy in range(cy0 + gy, cy0 + ch) for x in range(cx0, cx0 + cw)
                   if tuple(pix[(yy * sw + x) * 3:(yy * sw + x) * 3 + 3]) == (0xa0, 0x55, 0x3f)]
            assert pts, "no accent circle in the month grid"
            mx, my = sum(p[0] for p in pts) // len(pts) - cx0 - gx, sum(p[1] for p in pts) // len(pts) - cy0 - gy
            circled = (mx // cell_w + 7 * (my // cell_h)) - first + 1   # the day number whose cell holds the circle
            print("  ok: Calendar says", re.search(r"calendar: today [0-9-]+", u).group(0), "and circles the", circled, flush=True)
            key("esc")
            assert "calendar: closed" in open(log, errors="replace").read()
        key("f1")
        for k in "samantha": key(k)
        key("ret")
        assert "terminal open" in open(log, errors="replace").read(), "Samantha must open the assistant Terminal"

        time.sleep(0.5)
        parts = open(tmp + "/shot.ppm", "rb").read().split(b"\n", 3); w, h = map(int, parts[1].split())
        return (w, h, parts[3]) if not calendar else circled
    finally:
        q.kill(); q.wait(); shutil.rmtree(tmp, ignore_errors=True)

def masks(utc):
    w, h, b = boot_shot(utc); scale, icon, x0, y0 = geom(w, h); pw = icon * scale
    tx, ty = (x0 + 10 + 3 * (icon + 6)) * scale, (y0 + 10) * scale
    def region(y0, y1):
        return bytes(v for y in range(pw * y0 // 128, pw * y1 // 128)
                     for x in range(pw * 24 // 128, pw * 104 // 128)
                     for v in b[((ty+y)*w+tx+x)*3:((ty+y)*w+tx+x)*3+3])
    month, day = region(23, 46), region(49, 103)
    ink = sum(day[i] < 100 and day[i+1] < 100 and day[i+2] < 100 for i in range(0,len(day),3))
    if utc: assert ink > 12, "No readable day number"
    print("  ok: date", utc, "Clock opens/closes; Samantha opens Terminal; day ink", ink, flush=True)
    return month, day

LOCAL_11TH = 1791786000   # 2026-10-12 06:20 UTC, which is 23:20 on 11 October in Vancouver (PDT)
def circled_local(mutated):
    """Boots the Calendar app at LOCAL_11TH and returns the circled day; mutated builds SYS_TIME back on UTC first."""
    src = os.path.join(arch, "main.c"); orig = open(src).read()
    line = "unsigned long t = cal_local();"
    try:
        if mutated:
            assert orig.count(line) == 1, "the SYS_TIME handler changed; update the mutation"
            open(src, "w").write(orig.replace(line, "unsigned long t = cal_utc();"))
        return boot_shot(LOCAL_11TH, calendar=True)
    finally:
        open(src, "w").write(orig)

if "--mutate" in sys.argv:   # the mutation alone: the grid on UTC. This run must exit non-zero.
    day = circled_local(True)
    assert day == 11, "FAIL: with the grid on UTC the Calendar circles the %d, not the local 11" % day
    print("PASS (but the mutation survived)"); sys.exit(0)
assert circled_local(False) == 11, "the Calendar must circle the local 11th at 23:20 PDT on 11 October"
try: mutant = circled_local(True)
except AssertionError as e: mutant = None
assert mutant != 11, "the mutation (the grid on UTC) survived: the check cannot tell local from UTC"
print("  ok: UTC mutation caught (the grid circled", mutant, "instead of the 11th)", flush=True)
oct9 = masks(1791568800)
oct10 = masks(1791655200)
dec9 = masks(1796839200)
assert oct9[0] == oct10[0] and oct9[1] != oct10[1], "Day change must redraw digits only"
assert oct9[0] != dec9[0] and oct9[1] == dec9[1], "December must retain day 9 and change month"
masks(0)
print("PASS: readable Calendar month/day, December, Clock local date/time and unset state, Samantha launch, Calendar app circles the local day")
