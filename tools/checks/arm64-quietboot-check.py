#!/usr/bin/env python3
"""The Pi boots behind a grey splash to a clean desktop, and the Wi-Fi fan in the menu bar wears the house ink.

Three builds on QEMU's virt machine with ramfb and a USB keyboard and mouse behind a hub (the Pi image has QUIET_BOOT on by itself;
here -DQUIET_BOOT turns it on for virt, which can be clicked). WIFI_ICON_TEST makes the kernel print where the fan is.
  - Build 1, not joined: `console quiet` is printed and the Console's whole rectangle on screen is one flat graphite grey
    (no frame, no dots, no white well, no boot log); the menu bar's closing rule is there; the fan is flat ink
    (0x1C1C1E) even though nothing is joined, with no grey and nothing blue (no pixel with blue above red) in it.
    Then a click on the Mail tile opens the Console with the buffered boot log (white well, ink text, `console open`), and a
    click on the Terminal tile opens the Terminal (`terminal open`), so the log was kept the whole time.
  - Build 2, joined: the same fan is flat ink, 0x1C1C1E, the clock's ink, with no grey 0x909090 left in it (3.19.1: the speaker and power icons wear the same ink).
  - Build 3, the splash (-DSPLASH_STEP_MS=1500 holds each boot stage so screendumps can catch it): the first dump is solid
    terracotta with the tree mark centred (ink pixels), a track and one sixth of the bar filled, and no window frame, no menu
    bar, nothing white; a later dump (after the wifi stage) is still that splash with half the bar; the one before the end has
    five sixths; none of them shows the menu bar's rule or a Console frame. The desktop's first screendump comes after the
    `boot done` line, and the Console's rectangle is flat wallpaper then, too.
Run it with the boot window put back (console_frame and con_start in fb_init) or the old blue-grey fan and it fails.
Skips (exit 0) when clang's aarch64 target, ld.lld or qemu-system-aarch64 is missing.
Usage: tools/checks/arm64-quietboot-check.py   (from the repo root)
"""
import json, os, re, shutil, socket, subprocess, sys, tempfile, time

root = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..")
arch = os.path.join(root, "arch/arm64")
if not all(shutil.which(t) for t in ("clang", "ld.lld", "qemu-system-aarch64")):
    print("SKIP: clang, ld.lld or qemu-system-aarch64 not installed"); sys.exit(0)
fails = []
SPL, WALL, INK, DIM = (0xE5, 0xE5, 0xE7), (0x6E, 0x6E, 0x6E), (0x1C, 0x1C, 0x1E), (0x90, 0x90, 0x90)


def geom(w, h):
    s = 2 if h >= 1080 else 1; lw, lh = w // s, h // s
    icon = max(16, min(lh * 7 // 100, (min(740, lw - 40) - 2 * 10 - 10 * 6) // 11))
    dock_w = 11 * icon + 10 * 6 + 2 * 10; x0 = (lw - dock_w) // 2; y0 = lh - icon - 2 * 10 - 24
    return s, icon, x0, y0


def slot_centre(w, h, slot):
    s, icon, x0, y0 = geom(w, h)
    return (x0 + 10 + slot * (icon + 6) + icon // 2) * s, (y0 + 10 + icon // 2) * s


def screendump(cmd, tmp):
    shot = tmp + "/shot%d.ppm" % time.monotonic_ns()
    if "error" in cmd("screendump", filename=shot): return None
    time.sleep(0.3)
    parts = open(shot, "rb").read().split(b"\n", 3); w, h = map(int, parts[1].split())
    return w, h, parts[3]


def px(img, x, y):
    i = (y * img[0] + x) * 3
    return tuple(img[2][i:i + 3])


def boot(flags, tmp, tag, early=False):
    """Build with flags and boot it; returns (qemu, uart, cmd, wait_for), or None with a failure recorded."""
    subprocess.run(["make", "-C", arch, "clean"], capture_output=True)
    if subprocess.run(["make", "-C", arch, "MAINFLAGS=" + flags], capture_output=True).returncode:
        fails.append(f"arch/arm64 `make MAINFLAGS={flags}` does not build"); return None
    log, sock = f"{tmp}/uart{tag}", f"{tmp}/qmp{tag}"
    q = subprocess.Popen(["qemu-system-aarch64", "-machine", "virt", "-cpu", "cortex-a72", "-m", "256", "-nic", "none", "-device", "ramfb",
                          "-device", "pcie-root-port,id=rp,chassis=1", "-device", "qemu-xhci,id=x,bus=rp",
                          "-device", "usb-hub,id=h,bus=x.0,port=1", "-device", "usb-kbd,bus=x.0,port=1.1", "-device", "usb-mouse,id=m1,bus=x.0,port=1.2",
                          "-display", "none", "-serial", "file:" + log, "-qmp", "unix:%s,server,nowait" % sock,
                          "-kernel", os.path.join(arch, "kernel8.elf")], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    def uart(): return open(log, errors="replace").read() if os.path.exists(log) else ""
    def wait_for(text, tries=100):
        for _ in range(tries):
            if text in uart(): return True
            time.sleep(0.1)
        return False
    if early:
        if not wait_for("splash up", 300): fails.append(f"{tag}: no splash, got {uart()[-600:]!r}"); q.kill(); q.wait(); return None
    elif not wait_for("usb ready: 1 kbd, 1 mouse", 300) or not wait_for("wifi icon "):
        fails.append(f"{tag}: the desktop, keyboard and mouse did not come up, got {uart()[-600:]!r}"); q.kill(); q.wait(); return None
    s_ = socket.socket(socket.AF_UNIX)
    for _ in range(50):
        try: s_.connect(sock); break
        except OSError: time.sleep(0.2)
    f = s_.makefile("rw"); f.readline()
    def cmd(c, **a):
        f.write(json.dumps({"execute": c, "arguments": a}) + "\n"); f.flush()
        while True:
            r = json.loads(f.readline())
            if "return" in r or "error" in r: return r
    cmd("qmp_capabilities")
    if not early: time.sleep(0.5)
    return q, uart, cmd, wait_for


def fan(img, uart):
    ox, oy, R = map(int, re.findall(r"wifi icon (\d+),(\d+),(\d+)", uart())[-1])
    return [px(img, x, y) for y in range(oy - R - 1, oy + 3) for x in range(ox - R - 1, ox + R + 2)]


def dump(img, uart):   # QB_DUMP=1: the fan as text, # ink-dark, + mid grey, . bar
    ox, oy, R = map(int, re.findall(r"wifi icon (\d+),(\d+),(\d+)", uart())[-1])
    if os.environ.get("QB_DUMP"):
        print("\n".join("".join("#" if max(px(img, x, y)) < 0x50 else "+" if max(px(img, x, y)) < 0xA0 else "." for x in range(ox - R - 1, ox + R + 2)) for y in range(oy - R - 1, oy + 3)))


tmp = tempfile.mkdtemp()
try:
    # Build 1: not joined, and the whole life of the quiet window
    b = boot("-DQUIET_BOOT -DWIFI_ICON_TEST=0", tmp, "grey")
    if b:
        q, uart, cmd, wait_for = b
        try:
            if "console quiet\n" not in uart(): fails.append("no `console quiet` line: the boot window is not suppressed")
            img = screendump(cmd, tmp); W, H = img[0], img[1]
            wx, wy, ww, wh = (W - 500 * H // 600) // 2, 100 * H // 600, 500 * H // 600, 350 * H // 600
            area = [px(img, x, y) for y in range(wy, wy + wh, 3) for x in range(wx, wx + ww, 3)]
            odd = sum(1 for c in area if c != WALL)
            if odd: fails.append(f"the Console's rectangle is not flat wallpaper after boot ({odd} of {len(area)} samples differ: a window is on screen)")
            else: print(f"  ok: after boot the Console's whole rectangle is flat graphite grey ({len(area)} samples)")
            if px(img, W // 2, 26 * H // 540 - 1) != (0xBD, 0xB8, 0xB0): fails.append("the menu bar's closing rule is missing: no desktop")
            dump(img, uart); f_ = fan(img, uart)
            grey = sum(1 for c in f_ if c == DIM); blue = sum(1 for c in f_ if c[2] > c[0]); ink = sum(1 for c in f_ if max(c) < 0x50)
            ink = sum(1 for c in f_ if c == INK)
            if ink < 10 or grey or blue: fails.append(f"not joined, the fan should still be flat ink 0x1C1C1E: {ink} ink pixels, {grey} grey, {blue} bluish")
            else: print(f"  ok: not joined, the Wi-Fi fan is flat ink too, no grey ({ink} solid pixels, none blue)")
            # the log was kept: a dock click opens the Console with it, and the Terminal tile opens the Terminal
            pos = [W // 2, H // 2]
            def move_to(x, y):
                while (pos[0], pos[1]) != (x, y):
                    dx = max(-100, min(100, x - pos[0])); dy = max(-100, min(100, y - pos[1]))
                    cmd("input-send-event", events=[{"type": "rel", "data": {"axis": "x", "value": dx}}, {"type": "rel", "data": {"axis": "y", "value": dy}}])
                    pos[0] += dx; pos[1] += dy
                    if not wait_for(f"mouse {pos[0]},{pos[1]}\n", 40): return False
                return True
            def click():
                cmd("input-send-event", events=[{"type": "btn", "data": {"down": True, "button": "left"}}])
                cmd("input-send-event", events=[{"type": "btn", "data": {"down": False, "button": "left"}}])
            mx, my = slot_centre(W, H, 2)
            if not move_to(mx, my): fails.append("could not move onto the Mail tile")
            else:
                click()
                if not wait_for("console open", 30): fails.append(f"a dock click did not open the Console, got {uart()[-300:]!r}")
                else:
                    time.sleep(0.3); img = screendump(cmd, tmp)
                    well = [px(img, x, y) for y in range(wy + 34, wy + wh - 14) for x in range(wx + 8, wx + ww - 4)]
                    ink = sum(1 for c in well if max(c) < 0x90)
                    if px(img, W // 2, wy + wh - 8 - 2) != (255, 255, 255) or ink < 300: fails.append(f"the opened Console does not show the boot log ({ink} ink pixels)")
                    else: print(f"  ok: a dock click opens the Console with the buffered boot log ({ink} ink pixels)")
            tx, ty = slot_centre(W, H, 6)
            if not move_to(tx, ty): fails.append("could not move onto the Terminal tile")
            else:
                click()
                if not wait_for("terminal open", 30): fails.append(f"the Terminal tile did not open the Terminal, got {uart()[-300:]!r}")
                else: print("  ok: the Terminal tile opens the Terminal")
            if "KERNEL CRASH" in uart(): fails.append("the kernel crashed")
        finally:
            q.kill(); q.wait()
    # Build 3: the splash, stage by stage
    b = boot("-DQUIET_BOOT -DSPLASH_STEP_MS=1500 -DWIFI_ICON_TEST=0", tmp, "splash", early=True)
    if b:
        q, uart, cmd, wait_for = b
        try:
            def splash_check(n, what):
                if not wait_for(f"splash step {n} ", 100): fails.append(f"{what}: no `splash step {n}` line, got {uart()[-300:]!r}"); return None
                img = screendump(cmd, tmp)
                if not img: fails.append(f"{what}: no screendump"); return None
                W, H = img[0], img[1]
                pts = [px(img, x, y) for y in range(0, H, 4) for x in range(0, W, 4)]
                wall = sum(1 for c in pts if c == SPL) / len(pts)
                white = sum(1 for c in pts if min(c) > 0xEE)
                cx, sz = W // 2, 110 * H // 540
                my = H // 2 - 30 * H // 540
                inkpx = sum(1 for y in range(my - sz // 2, my + sz // 2) for x in range(cx - sz // 2, cx + sz // 2) if max(px(img, x, y)) < 0x50)
                rule = px(img, W // 2, 26 * H // 540 - 1)
                bw = 140 * H // 540; bx = (W - bw) // 2; by = H // 2 + 62 * H // 540
                filled = sum(1 for x in range(bx, bx + bw) if px(img, x, by) == (0x63, 0x63, 0x66))
                track = sum(1 for x in range(bx, bx + bw) if px(img, x, by) == (0xC8, 0xC8, 0xCC))
                if wall < 0.9 or white or rule != SPL: fails.append(f"{what}: not the bare grey splash ({wall:.0%} grey, {white} white samples, bar rule {rule})")
                elif inkpx < sz * sz // 12: fails.append(f"{what}: the tree mark is missing ({inkpx} ink pixels in its box)")
                elif abs(filled - bw * n // 6) > 3 or filled + track < bw - 3: fails.append(f"{what}: the progress line should be {n}/6 full, got {filled} ink and {track} track of {bw}")
                else: print(f"  ok: {what}: solid grey, the tree mark ({inkpx} ink pixels), the line {n}/6 full ({filled} of {bw}), no window frame")
                return img
            splash_check(1, "the first frame")
            if "boot done" in uart(): fails.append("boot was already done at the first frame")
            splash_check(3, "after the wifi stage")
            splash_check(5, "the last stage before the desktop")
            if not wait_for("splash step 6 ", 100): fails.append("no final splash step")
            else:
                u = uart()
                if u.index("boot done\n") > u.index("splash step 6 "): fails.append("the splash ended before `boot done`")
                if "M1c fb" in u[:u.index("boot done\n")]: fails.append("the desktop was painted before `boot done`")
                img = screendump(cmd, tmp)
                if px(img, img[0] // 2, 26 * img[1] // 540 - 1) == SPL and "M1c fb" not in uart(): print("  ok: at `boot done` the splash is still up, the desktop not painted yet")
                else: fails.append("the desktop was on screen at `boot done`")
            if not wait_for("M1c fb ok", 100): fails.append(f"the desktop never painted, got {uart()[-300:]!r}")
            else:
                if "console quiet\n" not in uart(): fails.append("no `console quiet` line")
                time.sleep(0.5); img = screendump(cmd, tmp); W, H = img[0], img[1]
                wx, wy, ww, wh = (W - 500 * H // 600) // 2, 100 * H // 600, 500 * H // 600, 350 * H // 600
                odd = sum(1 for y in range(wy, wy + wh, 3) for x in range(wx, wx + ww, 3) if px(img, x, y) != WALL)
                if px(img, W // 2, 26 * H // 540 - 1) != (0xBD, 0xB8, 0xB0): fails.append("after boot done the desktop (menu bar rule) is not there")
                elif odd: fails.append(f"after boot done the Console's rectangle is not flat wallpaper ({odd} samples differ)")
                else: print("  ok: after `boot done` the desktop paints once, with no Console window")
            if "KERNEL CRASH" in uart(): fails.append("the kernel crashed")
        finally:
            q.kill(); q.wait()
    # Build 2: joined
    b = boot("-DQUIET_BOOT -DWIFI_ICON_TEST=2", tmp, "ink")
    if b:
        q, uart, cmd, wait_for = b
        try:
            time.sleep(0.5)
            img = screendump(cmd, tmp); dump(img, uart); f_ = fan(img, uart)
            ink = sum(1 for c in f_ if c == INK); grey = sum(1 for c in f_ if c == DIM); blue = sum(1 for c in f_ if c[2] > c[0] + 4)
            if ink < 10 or grey or blue: fails.append(f"joined, the fan should be flat ink 0x1C1C1E: {ink} ink pixels, {grey} grey, {blue} bluish")
            else: print(f"  ok: joined, the Wi-Fi fan is flat ink, the clock's ({ink} solid pixels, no grey, none blue)")
        finally:
            q.kill(); q.wait()
finally:
    subprocess.run(["make", "-C", arch, "clean"], capture_output=True)
    shutil.rmtree(tmp, ignore_errors=True)

if fails:
    print("FAIL:"); [print("  - " + x) for x in fails]; sys.exit(1)
print("PASS: the Pi boots behind a grey splash that fills as the stages finish, the desktop then paints once with no Console window, the log is one click away, and the Wi-Fi fan is flat ink joined or not")
