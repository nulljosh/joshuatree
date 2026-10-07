#!/usr/bin/env python3
"""ARM64 M1c: the aarch64 kernel gets a framebuffer, draws a desktop into it and mirrors its boot log in the window in smooth DejaVu text, two ways. On QEMU's virt machine
through ramfb; on QEMU's Raspberry Pi 4B model through the VideoCore mailbox, the same call a real Pi answers.

Boots each build, waits for "M1c fb ok" on the UART, then asks QEMU for a screendump over QMP and checks real pixels:
the menu bar (its rule, and half wallpaper half white), the Satellite wallpaper (real photo pixels, in colour, not the old flat fill), the i386 dock (its tray,
hairline and 11 icon tiles, from the shared kernel/gui_paint.c; at 1080p every tile but Calendar must be the shared icon art pixel for pixel,
and Calendar must carry its face, which on the clockless Pi is a red and an ink dash, never a blank page), the i386 window frame on the Console (cream body, rounded
corner, title hairline, traffic lights, centred title, white well) and the tribute line above the dock. A dockhover test build
(`make hovertest`) must show the i386 hover label, capsule and name, over slot 3; the shipped kernel must not. On the Pi model it also fakes a 1080p
and a 4K monitor and checks the desktop fills a 1920x1080 screen. This proves what the screen shows, not just what the
kernel believes. The cache cleaning a real board needs is invisible to QEMU, so that part is checked in the source.
Skips (exit 0) when clang's aarch64 target, ld.lld or qemu-system-aarch64 is missing.
Usage: tools/checks/arm64-m1c-check.py   (from the repo root)
"""
import json, os, shutil, socket, subprocess, sys, tempfile, time

root = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..")
arch = os.path.join(root, "arch/arm64")
if not all(shutil.which(t) for t in ("clang", "ld.lld", "qemu-system-aarch64")):
    print("SKIP: clang, ld.lld or qemu-system-aarch64 not installed"); sys.exit(0)
subprocess.run(["make", "-C", arch, "clean"], capture_output=True)
if subprocess.run(["make", "-C", arch], capture_output=True).returncode:
    print("FAIL: arch/arm64 `make` does not build"); sys.exit(1)

fails = []

# The dock's artwork, straight from kernel/icon_art.h, scaled the way gui_paint.c gui_icon_art_scale does (area filter,
# premultiplied, over the tray), so a tile that differs from the shared art by even one pixel shows up. Corner pixels
# that land on the tray colour are skipped: gui_blit_tile leaves the shadow there.
import io, re
from PIL import Image
icon_h = open(os.path.join(root, "kernel/icon_art.h")).read()
ART_SIZE = int(re.search(r"#define ICON_ART_SIZE (\d+)", icon_h).group(1))
art_vars = re.findall(r"(icon_art_\w+),", re.search(r"ICON_ART\[\d+\] = \{(.*?)\};", icon_h, re.S).group(1))
dock_order = re.search(r"#define GUI_DOCK_DEFAULT_ORDER \{(.*?)\}", open(os.path.join(root, "kernel/gui_paint.h")).read()).group(1).replace(" ", "").split(",")
def _slot(t): return {"GUI_APPS_FOLDER": 30, "GUI_TRASH": 31}.get(t) if not t.isdigit() else int(t)
DOCK_ART = [art_vars[_slot(t)] for t in dock_order]
DOCK_NAMES = ["Apps", "Burrow", "Mail", "Calendar", "Notes", "Reminders", "Terminal", "Samantha", "Weather", "Stocks", "Trash"]
def art_tile(var, pw, under=(0xef, 0xeb, 0xe4)):
    body = re.search(r"%s\[\d+\] = \{(.*?)\};" % var, icon_h, re.S).group(1)
    png = Image.open(io.BytesIO(bytes(int(b) for b in body.replace("\n", "").split(",") if b.strip()))).convert("RGBA")
    S, data, out = png.width, png.load(), {}
    total = S * S; half = total // 2
    for py in range(pw):
        y0, y1 = py * S, py * S + S
        for px in range(pw):
            x0, x1 = px * S, px * S + S; rs = gs = bs = as_ = 0
            sy = y0 // pw
            while sy * pw < y1:
                wy = min(y1, (sy + 1) * pw) - max(y0, sy * pw); sx = x0 // pw
                while sx * pw < x1:
                    wx = min(x1, (sx + 1) * pw) - max(x0, sx * pw); r, g, b, a = data[sx, sy]
                    if a:
                        w_ = wx * wy; rs += w_ * ((r * a + 127) // 255); gs += w_ * ((g * a + 127) // 255); bs += w_ * ((b * a + 127) // 255); as_ += w_ * a
                    sx += 1
                sy += 1
            a = (as_ + half) // total
            c = tuple(min(255, (s_ + half) // total + (u * (255 - a) + 127) // 255) for s_, u in zip((rs, gs, bs), under))
            if c != under: out[(py, px)] = c
    return out

# The board-only half: QEMU has no cache, so a missing cache clean can never show up in a screendump. On the real Pi 4
# it did (2.12.5): the console's page wipe ran after the only clean, and the screen kept just the two pixel columns of
# old text left of a cache line boundary. So the source itself is checked: every console draw and wipe cleans what it
# touched, and the boot code finds its stack and .bss PC-relative, wherever the firmware loaded it.
main_c = open(os.path.join(arch, "main.c")).read()
def body(name):
    i = main_c.find("static void %s(" % name); j = main_c.find("\n}\n", i)
    return main_c[i:j] if i >= 0 else ""
for fn in ("con_glyph", "con_wipe"):
    if "fb_flush(" not in body(fn): fails.append(f"main.c {fn}() no longer cleans the cache over what it drew: a real Pi will not show it")
    else: print(f"  ok: {fn}() cleans the cache over what it drew")
start_s = open(os.path.join(arch, "start.S")).read()
if "ldr x0, =" in start_s or "ldr x1, =" in start_s: fails.append("start.S loads an absolute address: wrong if the firmware loads us anywhere but the link address")
elif "msr s3_3_c4_c4_0, xzr" not in start_s: fails.append("start.S no longer zeroes FPCR")
else: print("  ok: start.S is PC-relative and zeroes FPCR")
if "kernel_address=0x80000" not in open(os.path.join(root, "tools/pi-config.txt")).read():
    fails.append("tools/pi-config.txt lost kernel_address=0x80000")

def shoot(name, qemu_args, image, size=(800, 600), hover=False):
    tmp = tempfile.mkdtemp()
    log, sock, shot = tmp + "/uart", tmp + "/qmp", tmp + "/shot.ppm"
    q = subprocess.Popen(["qemu-system-aarch64", *qemu_args, "-display", "none", "-serial", "file:" + log,
                          "-qmp", "unix:%s,server,nowait" % sock, "-kernel", os.path.join(arch, image)],
                         stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        out = ""
        for _ in range(150):
            time.sleep(0.1)
            out = open(log, errors="replace").read() if os.path.exists(log) else ""
            if "M1c" in out: break
        if "M1c fb ok" not in out:
            fails.append(f"{name}: kernel did not report M1c fb ok, got {out!r}"); return
        s = socket.socket(socket.AF_UNIX); s.connect(sock); f = s.makefile("rw")
        f.readline()
        def cmd(c, **a):
            f.write(json.dumps({"execute": c, "arguments": a}) + "\n"); f.flush()
            while True:
                r = json.loads(f.readline())
                if "return" in r or "error" in r: return r
        cmd("qmp_capabilities")
        r = cmd("screendump", filename=shot)
        if "error" in r: fails.append(f"{name}: screendump failed: {r['error']}"); return
        time.sleep(0.3)
        parts = open(shot, "rb").read().split(b"\n", 3); w, h = map(int, parts[1].split()); px = parts[3]
        if (w, h) != size: fails.append(f"{name}: screen is {w}x{h}, want {size[0]}x{size[1]}"); return
        sc = lambda v: v * h // 600                                  # the kernel's own layout rule: an 800x600 design scaled by height
        win_w, win_h = sc(500), sc(350); win_x, win_y = (w - win_w) // 2, sc(100)
        # The dock is the i386 desktop's own (kernel/gui_paint.c, kernel/dock_geom.c): a 1080p screen is the 960x540
        # grid at scale 2, a smaller one scale 1. Same layout math as dock_geom.c, dock_scale_pct 7, 11 icons.
        s2 = 2 if h >= 1080 else 1; lw, lh = w // s2, h // s2
        icon = max(16, min(lh * 7 // 100, (min(740, lw - 40) - 2 * 10 - 10 * 6) // 11))
        dock_w = 11 * icon + 10 * 6 + 2 * 10; dock_x0 = (lw - dock_w) // 2; dock_y0 = lh - icon - 2 * 10 - 24
        dock_y = (dock_y0 - 24) * s2                                 # the top of the dock's band: the tribute sits above it
        win = [tuple(px[(y * w + x) * 3:(y * w + x) * 3 + 3]) for y in range(win_y + sc(32), win_y + win_h - sc(14)) for x in range(win_x + sc(8), win_x + win_w - sc(4))]
        ink = sum(1 for c in win if max(c) < 0x90)                  # dark text pixels
        soft = sum(1 for c in win if 0x30 < c[0] < 0xd0 and c[0] == c[1] == c[2])   # grey edge pixels: only smooth, anti-aliased text has them
        if ink < 300: fails.append(f"{name}: the boot log is not on screen (only {ink} text pixels in the window)")
        elif soft < 150: fails.append(f"{name}: the text is not anti-aliased ({soft} soft edge pixels), the smooth font is not drawing")
        else: print(f"  ok: {name} boot log drawn in the window ({ink} text pixels, {soft} soft edge pixels: smooth text)")
        band = [tuple(px[(y * w + x) * 3:(y * w + x) * 3 + 3]) for y in range(dock_y - sc(30), dock_y) for x in range(w // 2 - sc(150), w // 2 + sc(150))]
        light = sum(1 for c in band if c[0] > 0x70)
        if light < 40: fails.append(f"{name}: no tribute line above the dock ({light} light pixels)")
        else: print(f"  ok: {name} tribute line above the dock ({light} light pixels)")
        at = lambda x, y: tuple(px[(y * w + x) * 3:(y * w + x) * 3 + 3])
        mb = 26 * h // 540                                           # docs/DESIGN.md: GUI_MENUBAR_H = 26 on the 960x540 grid
        if at(w // 2, mb - 1) != (0xbd, 0xb8, 0xb0): fails.append(f"{name}: menu bar rule at y={mb - 1}: got {at(w // 2, mb - 1)}, want (189, 184, 176)")
        else: print(f"  ok: {name} menu bar closed by its rule at y={mb - 1}")
        if at(w // 2, mb) == (0xbd, 0xb8, 0xb0): fails.append(f"{name}: menu bar is taller than {mb} pixels")
        bad = []
        for x in (w // 4, w // 2 - sc(60), w // 2 + sc(60)):         # clear of the title, the badge and the clock
            bar, below = at(x, mb - 3), at(x, mb + 2)   # a few rows apart: the photo has detail
            want_bar = tuple((b + 255) // 2 for b in below)            # half wallpaper, half white
            if any(abs(a - b) > 32 or a < 0x7f for a, b in zip(bar, want_bar)): bad.append((x, bar, want_bar))
        if bad: fails.append(f"{name}: menu bar is not half wallpaper, half white: {bad}")
        else: print(f"  ok: {name} menu bar is half wallpaper, half white")
        dsk = [at(x, y) for y in range(mb + sc(10), dock_y - sc(40), max(1, sc(6))) for x in list(range(8, win_x - sc(8), max(1, sc(6)))) + list(range(win_x + win_w + sc(8), w - 8, max(1, sc(6))))]
        flat = sum(1 for c in dsk if c == (0x20, 0x30, 0x40))
        colours = len(set(dsk)); tinted = sum(1 for c in dsk if max(c) - min(c) > 16)
        if flat * 20 > len(dsk): fails.append(f"{name}: the desktop is still the flat fill ({flat} of {len(dsk)} samples)")
        elif colours < 150: fails.append(f"{name}: the desktop has only {colours} colours, that is not a photo")
        elif tinted * 4 < len(dsk): fails.append(f"{name}: the wallpaper is not in colour ({tinted} of {len(dsk)} samples tinted)")
        else: print(f"  ok: {name} wallpaper is the photo: {colours} colours in {len(dsk)} samples, {tinted} tinted")
        # The Console wears the i386 window frame (kernel/gui_paint.c gui_draw_window_frame) on the logical grid: cream
        # body, a hairline under the 30 pixel title band, three traffic lights at x+24/46/68, the name centred in ink,
        # and a white well for the log. Each dot is sampled 5 left of its centre, clear of the "x" and "-" glyphs.
        fx, fy, fw, fh = win_x // s2, win_y // s2, win_w // s2, win_h // s2
        L = lambda x, y: (x * s2, y * s2)
        want = [("window well", (w // 2, (fy + fh - 8) * s2 - 2), (0xff, 0xff, 0xff)),
                ("title band", L(fx + 100, fy + 6), (0xf5, 0xf0, 0xeb)),                          # WINDOW_BODY
                ("title hairline", (L(fx + 100, 0)[0], (fy + 29) * s2 + s2 - 1), (0xd9, 0xd3, 0xcb)),   # WINDOW_RULE
                ("close dot", L(fx + 19, fy + 16), (0xff, 0x5f, 0x57)), ("minimize dot", L(fx + 41, fy + 16), (0xff, 0xd6, 0x4a)),
                ("zoom dot", L(fx + 63, fy + 16), (0xd8, 0xd4, 0xce)),
                ("dock tray", (w // 2, (dock_y0 + 5) * s2), (0xef, 0xeb, 0xe4)),                 # DOCK_TRAY_COLOR, in the pad above the icons
                ("dock hairline", (w // 2, dock_y0 * s2 + s2 - 1), (0xd6, 0xd0, 0xc6))]         # its top edge, one physical pixel
        if at(*L(fx, fy)) == (0xf5, 0xf0, 0xeb): fails.append(f"{name}: the window's top-left corner is square, not rounded onto the wallpaper")
        else: print(f"  ok: {name} window corner is rounded onto the wallpaper")
        title = [at(x, y) for y in range(fy * s2 + 4 * s2, (fy + 26) * s2) for x in range((fx + fw // 2 - 30) * s2, (fx + fw // 2 + 30) * s2)]
        if sum(1 for c in title if max(c) < 0x80) < 15 * s2: fails.append(f"{name}: no 'Console' title in ink at the centre of the title band")
        else: print(f"  ok: {name} 'Console' title centred in the title band")
        # The dock's hover label (gui_draw_dock_label): the dockhover test build hovers slot 3, Calendar. A pale capsule
        # (DOCK_LABEL_BG) whose centre line is gui_dock_y0() - 13, with the name in ink. The shipped kernel has none.
        cx, ly = dock_x0 + 10 + 3 * (icon + 6) + icon // 2, dock_y0 - 21
        cap = [at(*L(x, ly + 1)) for x in range(cx - 10, cx + 11, 2)]
        lab = [at(x, y) for y in range((ly + 3) * s2, (ly + 16) * s2) for x in range((cx - 30) * s2, (cx + 30) * s2)]
        ink_ = sum(1 for c in lab if max(c) < 0x80)
        if hover:
            if "M1d hover 3 Calendar" not in out: fails.append(f"{name}: no 'M1d hover 3 Calendar' line from the dockhover build")
            if any(c != (0xf4, 0xf1, 0xec) for c in cap): fails.append(f"{name}: no hover label capsule over slot 3: {cap}")
            elif ink_ < 20 * s2: fails.append(f"{name}: the hover label has no name in ink ({ink_} pixels)")
            else: print(f"  ok: {name} hover label over slot 3: capsule {cap[0]}, {ink_} ink pixels of 'Calendar'")
        elif sum(1 for c in cap if c == (0xf4, 0xf1, 0xec)) > 2: fails.append(f"{name}: a hover label shows with nothing hovered")
        if "M1d dock 11 icons" not in out: fails.append(f"{name}: no 'M1d dock 11 icons' line: the icon artwork did not draw")
        tiles = []                                                   # each slot's tile: the icon artwork, not bare tray
        for slot in range(11):
            x0, y0 = (dock_x0 + 10 + slot * (icon + 6)) * s2, (dock_y0 + 10) * s2
            px_ = [at(x, y) for y in range(y0 + 4 * s2, y0 + (icon - 4) * s2, s2) for x in range(x0 + 4 * s2, x0 + (icon - 4) * s2, s2)]
            tiles.append((sum(1 for c in px_ if c != (0xef, 0xeb, 0xe4)) * 100 // len(px_), len(set(px_))))
        bare = [i for i, (cover, _) in enumerate(tiles) if cover < 60]
        if bare: fails.append(f"{name}: dock slots {bare} show the tray, not an icon (cover % per slot: {[t[0] for t in tiles]})")
        elif len({at((dock_x0 + 10 + k * (icon + 6) + icon // 2) * s2, (dock_y0 + 10 + icon // 3) * s2) for k in range(11)}) < 6:
            fails.append(f"{name}: the 11 dock tiles look alike, the artwork is not drawing")
        else: print(f"  ok: {name} dock: 11 icon tiles of {icon} at scale {s2}, {min(t[1] for t in tiles)}+ colours each")
        # Calendar's art is a blank page: the face (gui_paint.c gui_calendar_face) is what makes it a calendar. The Pi has
        # no clock, so it shows a red header dash and an ink dash; a tile with neither is the blank page Joshua saw.
        x0, y0 = (dock_x0 + 10 + 3 * (icon + 6)) * s2, (dock_y0 + 10) * s2
        cal = [at(x, y) for y in range(y0, y0 + icon * s2) for x in range(x0, x0 + icon * s2)]
        red = sum(1 for c in cal if c[0] > 200 and c[1] < 120 and c[2] < 120); dark = sum(1 for c in cal if max(c) < 0x60)
        if red < 8 * s2 or dark < 8 * s2: fails.append(f"{name}: the Calendar tile is a blank page ({red} red, {dark} ink pixels): no face drawn on it")
        else: print(f"  ok: {name} Calendar tile has its face ({red} red, {dark} ink pixels)")
        if s2 == 2 and icon * s2 < ART_SIZE:                         # at 1080p each tile is the shared art, area-filtered exactly
            off = []
            for slot, var in enumerate(DOCK_ART):
                if slot == 3: continue                               # Calendar: the art plus its live face
                want_t = art_tile(var, icon * s2)
                x0 = (dock_x0 + 10 + slot * (icon + 6)) * s2
                bad_px = sum(1 for (j, i), c in want_t.items() if at(x0 + i, y0 + j) != c)
                if bad_px: off.append(f"{DOCK_NAMES[slot]} ({bad_px} px)")
            if off: fails.append(f"{name}: dock tiles differ from the shared icon art (kernel/icon_art.h): {', '.join(off)}")
            else: print(f"  ok: {name} the other 10 dock tiles are the shared icon art, pixel for pixel")
        for what, (x, y), c in want:
            i = (y * w + x) * 3; got = tuple(px[i:i + 3])
            if got != c: fails.append(f"{name}: {what} at {(x, y)}: got {got}, want {c}")
            else: print(f"  ok: {name} {what} pixel {c}")
    finally:
        q.kill(); q.wait()
        shutil.rmtree(tmp, ignore_errors=True)

# The Pi's dock names follow APPS[] in kernel/kernel.c for GUI_DOCK_DEFAULT_ORDER: the label must say what i386 says.
import re
k = open(os.path.join(root, "kernel/kernel.c")).read()
apps = re.findall(r'\{"([^"]+)",', re.search(r"struct app APPS\[GUI_APP_COUNT\] = \{(.*?)\n\};", k, re.S).group(1))
order = re.search(r"#define GUI_DOCK_DEFAULT_ORDER \{(.*?)\}", open(os.path.join(root, "kernel/gui_paint.h")).read()).group(1).replace(" ", "").split(",")
i386_names = [apps[apps.index("Apps") if t == "GUI_APPS_FOLDER" else apps.index("Trash") if t == "GUI_TRASH" else int(t)] for t in order]
arm_names = re.findall(r'"([^"]+)"', re.search(r"dock_names\[GUI_ICON_COUNT\] = \{(.*?)\};", main_c).group(1))
if arm_names != i386_names: fails.append(f"main.c dock_names {arm_names} differ from the i386 dock {i386_names}")
else: print("  ok: the ARM dock's names match APPS[] for the default order")
shoot("virt ramfb", ["-machine", "virt", "-cpu", "cortex-a72", "-m", "256", "-device", "ramfb"], "kernel8.elf")
if subprocess.run(["make", "-C", arch, "hovertest"], capture_output=True).returncode: fails.append("`make hovertest` does not build")
else: shoot("virt hover", ["-machine", "virt", "-cpu", "cortex-a72", "-m", "256", "-device", "ramfb"], "hover-kernel8.elf", hover=True)
if "raspi4b" in subprocess.run(["qemu-system-aarch64", "-machine", "help"], capture_output=True, text=True).stdout:
    if subprocess.run(["make", "-C", arch, "pi"], capture_output=True).returncode: fails.append("pi: `make pi` does not build")
    else:
        shoot("pi mailbox", ["-machine", "raspi4b"], "kernel8.img")   # QEMU's model reports a 640x480 monitor: too small, stays 800x600
        # A 1080p monitor gets a 1080p desktop; a 4K one gets exactly half, so the firmware's 2x scale stays sharp.
        shoot("pi 1080p", ["-machine", "raspi4b", "-global", "bcm2835-fb.xres=1920", "-global", "bcm2835-fb.yres=1080"], "kernel8.img", (1920, 1080))
        shoot("pi 1080p hover", ["-machine", "raspi4b", "-global", "bcm2835-fb.xres=1920", "-global", "bcm2835-fb.yres=1080"], "hover-kernel8.img", (1920, 1080), hover=True)
        shoot("pi 4K", ["-machine", "raspi4b", "-global", "bcm2835-fb.xres=3840", "-global", "bcm2835-fb.yres=2160"], "kernel8.img", (1920, 1080))
else:
    print("  QEMU here has no raspi4b model (needs QEMU 9 or newer), Pi screen skipped")
subprocess.run(["make", "-C", arch, "clean"], capture_output=True)
for m in fails: print("FAIL: " + m)
if fails: sys.exit(1)
print("PASS: the aarch64 kernel draws its desktop through ramfb on virt and through the mailbox on the Pi 4B model, at 800x600 and at the monitor's own size, and QEMU's own screendump shows it with the right colors")
