#!/usr/bin/env python3
"""The ARM dock's Calendar tile is live (arch/arm64/main.c cal_overlay): once the clock is set it shows this month as a
seven-column grid of day squares with today's in the accent, no digits. The caltest build fixes the clock at CAL_UTC.
Two dates, two boots; in each screendump the accent pixels inside the tile all sit in the cell the date names, and
nowhere else in the tile. A tile that never changed with the date fails the second boot. Skips (exit 0) when the
tools are missing.
"""
import calendar, datetime, os, shutil, subprocess, sys, tempfile, time

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
                          "-device", "ramfb", "-display", "none", "-serial", "file:" + log, "-qmp", "unix:%s,server,nowait" % sock,
                          "-kernel", os.path.join(arch, "cal-kernel8.elf")], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        for _ in range(600):
            if os.path.exists(log) and "M1c fb ok" in open(log, errors="replace").read(): break
            time.sleep(0.1)
        else: raise SystemExit("FAIL: the desktop did not come up")
        time.sleep(0.5)
        import json, socket
        s_ = socket.socket(socket.AF_UNIX); s_.connect(sock); f = s_.makefile("rw"); f.readline()
        for c in ({"execute": "qmp_capabilities"}, {"execute": "screendump", "arguments": {"filename": tmp + "/shot.ppm"}}):
            f.write(json.dumps(c) + "\n"); f.flush()
            while "return" not in json.loads(f.readline()): pass
        time.sleep(0.5)
        parts = open(tmp + "/shot.ppm", "rb").read().split(b"\n", 3); w, h = map(int, parts[1].split())
        return w, h, parts[3]
    finally:
        q.kill(); q.wait(); shutil.rmtree(tmp, ignore_errors=True)

fails = []
for utc in (1791568800, 1769968800):   # 2026-10-09 18:00 UTC (a Friday in a month that starts on a Thursday), 2026-02-01 18:00 UTC (a Sunday on the first)
    d = datetime.datetime.fromtimestamp(utc, datetime.timezone.utc) - datetime.timedelta(hours=7 if 3 < datetime.datetime.fromtimestamp(utc, datetime.timezone.utc).month < 11 else 8)
    dow1 = (calendar.weekday(d.year, d.month, 1) + 1) % 7   # 0 = Sunday, as the kernel counts
    idx = dow1 + d.day - 1; c, r = idx % 7, idx // 7
    w, h, b = boot_shot(utc); s, icon, x0, y0 = geom(w, h); pw = icon * s
    tx, ty = (x0 + 10 + 3 * (icon + 6)) * s, (y0 + 10) * s
    ex0, ex1 = pw * (175 + 78 * c) // 896, pw * (175 + 78 * c + 57) // 896
    ey0, ey1 = pw * (300 + 52 * r) // 768, pw * (300 + 52 * r + 40) // 768
    acc = [(x, y) for y in range(pw) for x in range(pw) if b[((ty + y) * w + tx + x) * 3:((ty + y) * w + tx + x) * 3 + 3] == b"\xb5\x50\x2c"]
    inside = [p for p in acc if ex0 <= p[0] < max(ex1, ex0 + 1) and ey0 <= p[1] < max(ey1, ey0 + 1)]
    grey = sum(1 for y in range(pw) for x in range(pw) if b[((ty + y) * w + tx + x) * 3:((ty + y) * w + tx + x) * 3 + 3] == b"\xc7\xc8\xce")
    ok = acc and len(inside) == len(acc) and grey > len(acc) * 10
    print("  %s: %s col %d row %d: %d accent pixels, %d in the cell, %d grey day squares pixels" % ("ok" if ok else "FAIL", d.date(), c, r, len(acc), len(inside), grey))
    if not ok: fails.append(str(d.date()))
if fails: print("FAIL: arm64-calicon-check: " + ", ".join(fails)); sys.exit(1)
print("PASS: arm64-calicon-check: the Calendar tile highlights the date's square, and only that one")
