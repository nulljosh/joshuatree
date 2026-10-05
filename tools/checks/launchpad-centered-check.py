#!/usr/bin/env python3
"""The Launchpad (the Apps folder) is centered, padded and not oversized, measured on real pixels.

Joshua saw it on a real 1920x1080-class monitor and said the icons were too big, it was not centered, and it
needed margin and padding. The cause: the window was parked at a fixed x=56 (a 960px screen centers an 848px
window there, any wider screen does not), the icons were 74px, and the glass panel had 12px of room above its hint
line and 66px beside the first icon.

For each screen size below (and with portfolio_dock on and off) this boots headless, opens the Apps folder from
the first dock tile, dumps the framebuffer and measures only pixels:

  - the glass panel's left margin equals its right margin (within 1 logical px) and its top and bottom sit
    the same distance from the menu bar and the dock (within 2),
  - the panel keeps at least 24px from the menu bar and from the dock, and 40px from the screen sides,
  - no icon is bigger than 56px (it was 74),
  - inner padding is even: the first and last icon columns are the same distance from the panel's sides, and the
    hint line's top inset equals the last label's bottom inset (within 3),
  - no two labels touch (6px minimum gap), and no label touches the icon under it.

It also compiles kernel/apps_geom.h on the host and walks every screen and strip size: the window always stays
between the menu bar and the dock, the panel stays centered, and no row count that fits is skipped.

Discriminating: on the old code (fixed x=56, 74px tiles) this fails at every size wider than 960 (the panel sits
20px or more left of center) and at every size on icon size and padding. It passes only with the centered layout.

Usage: tools/checks/launchpad-centered-check.py [--keep]   (from the repo root, after make kernel.elf)
"""
import json, os, socket, subprocess, sys, tempfile, time
from PIL import Image
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from freeport import free_port

os.chdir(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
FB = 0xfd000000
MENU_H = 26
NEW_TILE = 56
MIN_V_MARGIN, MIN_SIDE_MARGIN, MIN_LABEL_GAP = 24, 40, 6
# (physical WxH, boot args). Each size is a real "res=" boot; the portfolio run shows one more app.
CONFIGS = [((1920, 1080), []), ((1920, 1080), ["portfolio"]), ((2000, 1124), ["res=2000x1124"]),
           ((1600, 900), ["res=1600x900"]), ((2560, 1440), ["res=2560x1440"])]


def dock_geometry(lw, lh):
    """gui_dock_icon / gui_dock_x0 / gui_dock_y0 for 11 tiles at the default 7 percent scale."""
    icon = max(16, min(lh * 7 // 100, (min(740, lw - 40) - 2 * 10 - 10 * 6) // 11))
    w = 11 * icon + 10 * 6 + 2 * 10
    return icon, (lw - w) // 2, lh - icon - 2 * 10 - 24


def shoot(size, args, path):
    w, h = size; lw, lh = w // 2, h // 2
    port = free_port(); log = path + ".log"
    cmd = ["qemu-system-i386", "-rtc", "base=2026-09-23T19:30:00", "-kernel", "kernel.elf", "-display", "none", "-vga", "std",
           "-qmp", f"tcp:127.0.0.1:{port},server,nowait", "-serial", "file:" + log, "-name", "jt-lpcentered"]
    if args: cmd += ["-append", " ".join(args)]
    q = subprocess.Popen(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        time.sleep(1.0)
        s = socket.create_connection(("127.0.0.1", port)); f = s.makefile("rw")
        def qmp(o):
            f.write(json.dumps(o) + "\n"); f.flush()
            while True:
                r = json.loads(f.readline())
                if "return" in r or "error" in r: return r
        f.readline(); qmp({"execute": "qmp_capabilities"}); time.sleep(5.0)
        icon, dx0, dy0 = dock_geometry(lw, lh)
        x, y = dx0 + 10 + icon // 2, dy0 + 10 + icon // 2
        qmp({"execute": "input-send-event", "arguments": {"events": [
            {"type": "abs", "data": {"axis": "x", "value": int(x * 32768 / lw)}},
            {"type": "abs", "data": {"axis": "y", "value": int(y * 32768 / lh)}}]}})
        time.sleep(0.4)
        for down in (True, False):
            qmp({"execute": "input-send-event", "arguments": {"events": [{"type": "btn", "data": {"down": down, "button": "left"}}]}})
            time.sleep(0.12)
        time.sleep(2.0)
        raw = path + ".raw"
        qmp({"execute": "pmemsave", "arguments": {"val": FB, "size": w * h * 4, "filename": raw}})
        time.sleep(0.5)
        try: qmp({"execute": "quit"})
        except (ConnectionResetError, BrokenPipeError, OSError): pass
    finally:
        try: q.wait(timeout=5)
        except subprocess.TimeoutExpired: q.kill()
    img = Image.frombytes("RGBA", (w, h), open(raw, "rb").read(), "raw", "BGRA").convert("RGB")
    os.remove(raw)
    return img


def lum(p): return (p[0] * 299 + p[1] * 587 + p[2] * 114) // 1000


def find_panel(img):
    """Glass panel rect in logical px: scan outward from the screen center along lines that cross only glass
    (gaps between icon columns), and stop at the first 8px-wide run of dark wallpaper."""
    w, h = img.size; cx, cy = w // 2, h // 2
    DARK, RUN = 165, 8
    def edge(points):
        run = 0; last_light = None
        for i, (x, y) in enumerate(points):
            if lum(img.getpixel((x, y))) < DARK:
                run += 1
                if run >= RUN: return last_light
            else:
                run = 0; last_light = i
        return None
    gx = cx + 120  # between two icon columns for both 120px and 150px cells
    up = edge([(gx, y) for y in range(cy, 0, -1)]); dn = edge([(gx, y) for y in range(cy, h)])
    if up is None or dn is None: return None
    top, bot = cy - up, cy + dn + 1
    row = top + 80  # 40 logical px below the top edge: past the corner radius, below the hint line, above the first icon row
    lf = edge([(x, row) for x in range(cx, 0, -1)]); rt = edge([(x, row) for x in range(cx, w)])
    if lf is None or rt is None: return None
    return (cx - lf) / 2, top / 2, (cx + rt + 1) / 2, bot / 2  # left, top, right, bottom (logical)


def components(img, box):
    """Ink (text, icons) inside the panel as connected blobs, sampled at logical resolution."""
    l, t, r, b = [int(v) for v in box]
    l += 12; t += 12; r -= 12; b -= 12  # inside the 24px corner radius: no wallpaper in the mask
    mask = {}
    for y in range(t, b):
        for x in range(l, r):
            p = img.getpixel((2 * x, 2 * y))
            if lum(p) < 165 or max(p) - min(p) > 60: mask[(x, y)] = 1
    seen, comps = set(), []
    for pt in mask:
        if pt in seen: continue
        stack = [pt]; seen.add(pt); xs = []; ys = []
        while stack:
            x, y = stack.pop(); xs.append(x); ys.append(y)
            for nb in ((x + 1, y), (x - 1, y), (x, y + 1), (x, y - 1), (x + 1, y + 1), (x - 1, y - 1), (x + 1, y - 1), (x - 1, y + 1)):
                if nb in mask and nb not in seen: seen.add(nb); stack.append(nb)
        comps.append((min(xs), min(ys), max(xs) + 1, max(ys) + 1, len(xs)))
    return comps


def inside(a, b): return a[0] >= b[0] - 1 and a[1] >= b[1] - 1 and a[2] <= b[2] + 1 and a[3] <= b[3] + 1


def measure(img, size):
    lw, lh = size[0] // 2, size[1] // 2
    out = {"fails": []}; fails = out["fails"]
    box = find_panel(img)
    if not box: fails.append("could not find the glass panel"); return out
    l, t, r, b = box
    icon, _, dock_top = dock_geometry(lw, lh)
    out["panel"] = box
    left_m, right_m = l, lw - r
    out["margins"] = (left_m, right_m, t - MENU_H, dock_top - b)
    if abs(left_m - right_m) > 1: fails.append(f"panel off center: left margin {left_m}, right margin {right_m} ({left_m - right_m:+.0f} px)")
    if left_m < MIN_SIDE_MARGIN: fails.append(f"left margin {left_m} under {MIN_SIDE_MARGIN}")
    if t - MENU_H < MIN_V_MARGIN: fails.append(f"only {t - MENU_H} px between menu bar and panel")
    if dock_top - b < MIN_V_MARGIN: fails.append(f"only {dock_top - b} px between panel and dock")
    if abs((t - MENU_H) - (dock_top - b)) > 2: fails.append(f"panel not centered between menu bar and dock: {t - MENU_H} above, {dock_top - b} below")

    comps = components(img, box)
    big = [c for c in comps if c[4] >= 300]
    # An outline can split one icon into pieces (Hikko's cream jar rim separates its brown tile from the inside of the jar).
    # A piece that sits wholly inside another is the same icon, not a second one.
    big = [c for c in big if not any(g is not c and inside(c, g) for g in big)]
    small = [c for c in comps if c[4] < 300 and not any(inside(c, g) for g in big)]
    if len(big) < 5: fails.append(f"found only {len(big)} icons"); return out
    tile = max(max(c[2] - c[0], c[3] - c[1]) for c in big)
    out["tile"] = tile
    if tile > NEW_TILE + 1: fails.append(f"icons are {tile}px, the cap is {NEW_TILE}")
    # Inner padding. Horizontal: panel side to the outermost icon. Vertical: panel top to the hint's top, last label's bottom to panel bottom.
    lpad = min(c[0] for c in big) - l; rpad = r - max(c[2] for c in big)
    first_icon_top = min(c[1] for c in big)
    hint = [c for c in small if c[3] <= first_icon_top]
    out["pads"] = [lpad, rpad]
    if abs(lpad - rpad) > 1: fails.append(f"uneven side padding: {lpad} left, {rpad} right")
    if not hint: fails.append("hint line not found"); return out
    hint_top = min(c[1] for c in hint); hint_left = min(c[0] for c in hint)
    labels = [c for c in small if c[1] >= first_icon_top]
    last_bottom = max(c[3] for c in labels)
    tpad, bpad = hint_top - t, b - last_bottom
    out["pads"] += [tpad, bpad, hint_left - l]
    if abs(tpad - bpad) > 3: fails.append(f"uneven top/bottom padding: {tpad} above the hint, {bpad} below the last label")
    if tpad < 16 or bpad < 16: fails.append(f"padding too tight: {tpad} top, {bpad} bottom (16 minimum)")
    if hint_left - l < 16: fails.append(f"hint line {hint_left - l}px from the panel's left edge (16 minimum)")
    # Labels: one box per icon, nearest by x, under it.
    boxes = []
    for g in big:
        gx = (g[0] + g[2]) / 2
        mine = [c for c in labels if g[3] <= c[3] <= g[3] + 30 and abs((c[0] + c[2]) / 2 - gx) < 60 and c[1] >= g[3] - 2]
        if mine: boxes.append((min(c[0] for c in mine), min(c[1] for c in mine), max(c[2] for c in mine), max(c[3] for c in mine), g))
    out["labels"] = len(boxes)
    if len(boxes) < len(big) - 1: fails.append(f"found {len(boxes)} labels for {len(big)} icons")
    for i, a in enumerate(boxes):
        if a[1] < a[4][3] + 1: fails.append(f"a label touches the icon above it at x={a[0]}")
        for c in boxes[i + 1:]:
            hgap = max(c[0] - a[2], a[0] - c[2]); vgap = max(c[1] - a[3], a[1] - c[3])
            if hgap < MIN_LABEL_GAP and vgap < MIN_LABEL_GAP: fails.append(f"labels at x={a[0]},{c[0]} y={a[1]},{c[1]} are {max(hgap, vgap)}px apart")
        for g in big:  # label vs the next row's icon
            if g is not a[4] and a[3] + MIN_LABEL_GAP > g[1] and a[3] <= g[3] and a[0] < g[2] and a[2] > g[0] and g[1] > a[1]:
                fails.append(f"label at x={a[0]} is {g[1] - a[3]}px above the icon below it")
    return out


def host_geometry_walk():
    """apps_geom.h on the host: every screen and strip size keeps the panel inside its strip and centered."""
    src = r'''
#include <stdio.h>
#include "kernel/apps_geom.h"
int main(void){
    for (int vw = 280; vw <= 3000; vw += 7) for (int avail = 100; avail <= 1200; avail += 5) {
        int top = 26, bot = top + avail, vis = apps_rows_fit(top, bot);
        struct apps_geom g = apps_geom_make(vw, vis, APPS_MARGIN);
        if (vis < 1 || vis > 3) { printf("vis %d at %d,%d\n", vis, vw, avail); return 1; }
        if (vw >= 700 && (g.px < 0 || g.px + g.pw > vw || (g.px * 2 + g.pw - vw > 1) || (vw - g.px * 2 - g.pw > 1))) { printf("off center at vw=%d px=%d pw=%d\n", vw, g.px, g.pw); return 1; }
        int py2 = apps_panel_y(top, bot, vis);
        if (vis > 1 && (py2 - APPS_MARGIN - 32 < top + 2 || py2 + g.ph + APPS_MARGIN + 8 > bot - 8)) { printf("window leaves the strip at avail=%d vis=%d\n", avail, vis); return 1; }
        if (avail >= 433 && vis != 3) { printf("lost rows at %d\n", avail); return 1; }
        if (avail >= 340 && vis < 2) { printf("only one row at %d\n", avail); return 1; }
        if (vis < 3 && apps_panel_y(top, bot, vis + 1) - APPS_MARGIN - 32 >= top + 2 && apps_panel_y(top, bot, vis + 1) + apps_panel_h(vis + 1) + APPS_MARGIN + 8 <= bot - 8) { printf("could have shown %d rows at %d\n", vis + 1, avail); return 1; }
        if (g.cell_w < APPS_TILE + 8 || g.tile > APPS_TILE) { printf("cell/tile bad at vw=%d\n", vw); return 1; }
    }
    puts("ok"); return 0;
}'''
    with tempfile.TemporaryDirectory() as d:
        open(d + "/w.c", "w").write(src)
        subprocess.run(["cc", "-I.", "-o", d + "/w", d + "/w.c"], check=True)
        r = subprocess.run([d + "/w"], capture_output=True, text=True)
    return r.returncode == 0, r.stdout.strip()


def main():
    keep = "--keep" in sys.argv
    if not os.path.exists("kernel/apps_geom.h"):
        ok, msg = True, "no apps_geom.h (old code)"
    else:
        ok, msg = host_geometry_walk()
    all_fails = [] if ok else ["apps_geom.h walk: " + msg]
    outdir = tempfile.mkdtemp(prefix="jt-lpcentered-")
    for size, args in CONFIGS:
        name = "%dx%d%s" % (size[0], size[1], "-" + "+".join(args) if args else "")
        img = shoot(size, args, os.path.join(outdir, name + ".png"))
        if keep: img.save(os.path.join(outdir, name + ".png"))
        m = measure(img, size)
        print(name, "panel", m.get("panel"), "margins L/R/top/bottom", m.get("margins"), "tile", m.get("tile"),
              "pads L/R/top/bottom/hint", m.get("pads"), "labels", m.get("labels"))
        for f in m["fails"]: all_fails.append(name + ": " + f)
    if all_fails:
        print("FAIL:")
        for f in all_fails: print("  - " + f)
        sys.exit(1)
    print("PASS: Launchpad centered, evenly padded, icons <= %dpx, labels clear, at every size" % NEW_TILE)


if __name__ == "__main__": main()
