#!/usr/bin/env python3
"""Calendar month/day glyphs and the Clock window, including December and unset time.
QEMU uses fixed dates, a real keyboard, and framebuffer assertions; no paid API call."""
import datetime, os, shutil, subprocess, sys, tempfile, time

root = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."); arch = os.path.join(root, "arch", "arm64")
if not all(shutil.which(t) for t in ("clang", "ld.lld", "qemu-system-aarch64")):
    print("SKIP: clang, ld.lld or qemu-system-aarch64 not installed"); sys.exit(0)

def geom(w, h):   # kernel/dock_geom.c at this screen (800x600 under virt: scale 1)
    s = 2 if h >= 1080 else 1; lw, lh = w // s, h // s
    icon = max(16, min(lh * 7 // 100, (min(740, lw - 40) - 2 * 10 - 10 * 6) // 11))
    dock_w = 11 * icon + 10 * 6 + 2 * 10; x0 = (lw - dock_w) // 2; y0 = lh - icon - 2 * 10 - 24
    return s, icon, x0, y0

def boot_shot(utc):
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
        if utc == 1791568800: shutil.copyfile(tmp + "/clock.ppm", "/tmp/jt-pi-clock.ppm")
        key("esc")
        assert "clock closed" in open(log, errors="replace").read()
        key("f1")
        for k in "samantha": key(k)
        key("ret")
        assert "terminal open" in open(log, errors="replace").read(), "Samantha must open the assistant Terminal"

        time.sleep(0.5)
        parts = open(tmp + "/shot.ppm", "rb").read().split(b"\n", 3); w, h = map(int, parts[1].split())
        return w, h, parts[3]
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

oct9 = masks(1791568800)
oct10 = masks(1791655200)
dec9 = masks(1796839200)
assert oct9[0] == oct10[0] and oct9[1] != oct10[1], "Day change must redraw digits only"
assert oct9[0] != dec9[0] and oct9[1] == dec9[1], "December must retain day 9 and change month"
masks(0)
print("PASS: readable Calendar month/day, December, Clock local date/time and unset state, Samantha launch")
