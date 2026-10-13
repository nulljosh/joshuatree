#!/usr/bin/env python3
"""ARM64 Stocks: the dock tile and Spotlight open user/stocks_pi.c in a window at EL0: a watchlist with green and red
change pills on the left and an anti-aliased line chart of the selected symbol on the right (headless QEMU, own private
scratch).

Boots the normal ARM kernel on QEMU's virt machine with ramfb and virtio keyboard and tablet, no network and no card, so
there is no STOCKS.TXT and the app must show its labelled sample ("Sample, not live prices"). Through QMP it:
  - clicks the Stocks tile (slot 9): "dock Stocks: not on ARM yet" is gone; the scheduler starts stocks, its window
    opens and it says "stocks: sample, not live prices" and "stocks: sel AAPL";
  - samples the framebuffer: the page colour, green and red pills in the watchlist, a chart line with soft edges;
  - presses Down: "stocks: sel MSFT" and the chart region changes;
  - presses Esc: "M3c app stocks exit 0" and the wallpaper is back where the window stood;
  - opens it again from Spotlight (F1, "stoc", Enter) and closes it with its red dot.
No KERNEL CRASH, no load failure. Skips (exit 0) when clang's aarch64 target, ld.lld or qemu-system-aarch64 is missing.
Set JT_STOCKS_SHOT=path.ppm to keep the screendump of the open window.
Usage: tools/checks/arm64-stocks-check.py   (from the repo root)
"""
import json, os, re, shutil, socket, subprocess, sys, time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from scratch import scratch_dir

root = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..")
arch = os.path.join(root, "arch/arm64")
if not all(shutil.which(t) for t in ("clang", "ld.lld", "qemu-system-aarch64")):
    print("SKIP: clang, ld.lld or qemu-system-aarch64 not installed"); sys.exit(0)
if subprocess.run(["make", "-s", "-C", arch, "kernel8.elf"], capture_output=True).returncode:
    print("FAIL: arch/arm64 `make kernel8.elf` does not build"); sys.exit(1)
fails = []
def check(name, ok, detail=""):
    print(("  ok: " if ok else "  FAIL: ") + name + ("" if ok or not detail else " -- " + str(detail)[-400:]))
    if not ok: fails.append(name)

W, H = 800, 600
icon = max(16, min(H * 7 // 100, (min(740, W - 40) - 2 * 10 - 10 * 6) // 11))
dock_x0, dock_y0 = (W - (11 * icon + 10 * 6 + 2 * 10)) // 2, H - icon - 2 * 10 - 24
def slot_centre(slot): return dock_x0 + 10 + slot * (icon + 6) + icon // 2, dock_y0 + 10 + icon // 2
BG, GREEN, PILLRED, RED = (0xf5, 0xf0, 0xeb), (0x41, 0x85, 0x4b), (0xb5, 0x16, 0x16), (0xff, 0x5f, 0x57)

tmp = scratch_dir("arm64-stocks")
log, sock = tmp + "/uart", tmp + "/qmp"
q = subprocess.Popen(["qemu-system-aarch64", "-machine", "virt", "-cpu", "cortex-a72", "-m", "256", "-nic", "none",
                      "-global", "virtio-mmio.force-legacy=false", "-device", "ramfb",
                      "-device", "virtio-keyboard-device", "-device", "virtio-tablet-device", "-display", "none",
                      "-serial", "file:" + log, "-qmp", "unix:%s,server,nowait" % sock,
                      "-kernel", os.path.join(arch, "kernel8.elf")], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
def uart(): return open(log, errors="replace").read() if os.path.exists(log) else ""
def wait_for(text, secs=6.0, after=0):
    end = time.time() + secs
    while time.time() < end:
        if text in uart()[after:]: return True
        time.sleep(0.05)
    return False
try:
    if not wait_for("M2 input ready", 40):
        fails.append(f"the desktop did not boot, got {uart()[-600:]!r}"); raise SystemExit
    s_ = socket.socket(socket.AF_UNIX); s_.settimeout(20); s_.connect(sock); f = s_.makefile("rw"); f.readline()
    def cmd(c, **a):
        f.write(json.dumps({"execute": c, "arguments": a}) + "\n"); f.flush()
        while True:
            r = json.loads(f.readline())
            if "return" in r or "error" in r: return r
    cmd("qmp_capabilities")
    time.sleep(0.5)
    def shot(keep=None):
        path = tmp + "/shot%d.ppm" % time.monotonic_ns()
        cmd("screendump", filename=path)
        for _ in range(30):
            if os.path.exists(path) and os.path.getsize(path) >= W * H * 3: break
            time.sleep(0.1)
        if keep: shutil.copy(path, keep)
        parts = open(path, "rb").read().split(b"\n", 3)
        return int(parts[1].split()[0]), parts[3]
    def px(img, x, y): i = (y * img[0] + x) * 3; return tuple(img[1][i:i + 3])
    def region(img, x, y, w, h): return [px(img, i, j) for j in range(y, y + h) for i in range(x, x + w)]
    def key(*qcodes): cmd("send-key", keys=[{"type": "qcode", "data": k} for k in qcodes], **{"hold-time": 50}); time.sleep(0.15)
    def click(x, y):
        ax, ay = x * 32767 // (W - 1), y * 32767 // (H - 1)
        cmd("input-send-event", events=[{"type": "abs", "data": {"axis": "x", "value": ax}}, {"type": "abs", "data": {"axis": "y", "value": ay}}])
        time.sleep(0.3)
        cmd("input-send-event", events=[{"type": "btn", "data": {"down": True, "button": "left"}}])
        cmd("input-send-event", events=[{"type": "btn", "data": {"down": False, "button": "left"}}])
        time.sleep(0.3)

    base = shot()
    # ---- Stocks from the dock ----
    n0 = len(uart())
    click(*slot_centre(9))
    ok = wait_for("M3c window stocks first frame", 8, n0)
    out = uart()[n0:]
    check("the Stocks tile starts user/stocks_pi.c under the scheduler, not the old 'not on ARM yet' line",
          ok and "M3c app stocks starts" in out and "not on ARM yet" not in out, out[-500:])
    check("it found no saved prices and drew the labelled sample, AAPL first",
          "stocks: sample, not live prices\n" in out and "stocks: sel AAPL\n" in out, out[-300:])
    m = re.search(r"M3c window stocks (\d+)x(\d+) at (\d+),(\d+)\n", out)
    if not m: fails.append("no 'M3c window stocks WxH at X,Y' line"); raise SystemExit
    vw, vh, cx0, cy0 = map(int, m.groups())
    fx, fy, fw, fh = cx0 - 8, cy0 - 32, vw + 16, vh + 40
    time.sleep(0.5)
    a = shot(os.environ.get("JT_STOCKS_SHOT"))
    check("the window wears the house frame: its red dot, inside the screen", px(a, fx + 19, fy + 16) == RED and fx + fw <= W and fy + fh <= dock_y0, (fx, fy, fw, fh))
    check("the page is the warm off-white", px(a, cx0 + 400, cy0 + 4) == BG, px(a, cx0 + 400, cy0 + 4))
    side = region(a, cx0, cy0, 300, vh)
    pills = sum(1 for c in side if c in (GREEN, PILLRED))
    check("the watchlist has its change pills, green and red (%d pill pixels)" % pills, pills > 800 and GREEN in side and PILLRED in side, pills)
    sx, cw_, ch0 = cx0 + 324, vw - 348, 120
    chart_a = region(a, sx, cy0 + ch0, cw_, vh - ch0 - 72)
    lines = sum(1 for c in chart_a if c != BG)
    aa = len({c for c in chart_a if c != BG})
    check("the chart draws a line with soft edges (%d ink pixels, %d shades)" % (lines, aa), lines > 80 and aa > 8, (lines, aa))
    # ---- Down picks MSFT: the chart changes ----
    n = len(uart()); key("down")
    check("Down selects MSFT", wait_for("stocks: sel MSFT", 4, n), uart()[n:][-300:])
    time.sleep(0.5)
    b2 = shot()
    chart_b = region(b2, sx, cy0 + ch0, cw_, vh - ch0 - 72)
    diff = sum(1 for p, q_ in zip(chart_a, chart_b) if p != q_)
    check("the chart changed (%d pixels differ)" % diff, diff > 80, diff)
    # ---- Esc closes it ----
    n = len(uart()); key("esc")
    check("Esc closes Stocks and the app exits 0", wait_for("M3c app stocks exit 0", 4, n) and "stocks: closed\n" in uart()[n:], uart()[n:][-300:])
    time.sleep(0.5)
    b = shot()
    top = (fx, fy, fw, 100 - fy - 2)   # the window's part above the Console (which starts at y 100)
    check("the wallpaper is back pixel for pixel where the window stood above the Console", top[3] > 4 and region(b, *top) == region(base, *top) and region(a, *top) != region(base, *top))
    # ---- Spotlight opens it too, the red dot closes it ----
    n = len(uart()); key("f1"); wait_for("spotlight open", 3, n)
    for k in "stoc": key(k)
    key("ret")
    check("Spotlight finds Stocks and Enter opens its window", wait_for("M3c window stocks first frame", 8, n), uart()[n:][-300:])
    n = len(uart()); time.sleep(0.5); click(fx + 19, fy + 16)
    check("its red dot closes it: the app exits 0", wait_for("M3c app stocks exit 0", 6, n), uart()[n:][-300:])
    u = uart()
    for bad in ("KERNEL CRASH", "M3c load FAIL", "oom"):
        check("%r never printed" % bad, bad not in u)
except SystemExit:
    pass
finally:
    q.kill(); q.wait()
if fails:
    for msg in fails: print("FAIL: " + msg)
    sys.exit(1)
print("PASS: the Stocks tile and Spotlight open the Pi Stocks window: pills and a chart are drawn (labelled sample), Down changes the chart, "
      "and Esc and the red dot close it")
