#!/usr/bin/env python3
"""Rasterize landing/logo.svg (the crayon Joshua tree brand mark) into an
8-bit coverage header for the boot splash, and landing/mark-bold.svg (the same
tree drawn down its middle) for the menu bar, where the thin line would vanish.

Why rsvg-convert and not ImageMagick: ImageMagick's own SVG renderer draws
potrace/hand-authored vector output blank on this box (confirmed, the same
trap gen_icon_art.py's neighbours hit) -- rsvg-convert renders it correctly.

Physical size, not logical: the splash draws in PHYSICAL pixels via
window_pixel_phys (the gui_aa_char pattern), so this rasterizes at the real
physical height the splash needs, ~160px tall at 1920x1080, and derives the
width from the source SVG's own aspect ratio rather than hardcoding one.

Output is a plain 8-bit alpha coverage array (one byte per physical pixel),
not a PNG -- the splash blends it directly with no decoder, no cache, no
extra kernel-image cost beyond the raw bytes themselves.

Draw order: the splash grows the tree stroke by stroke. The SVG's subpaths are
re-rendered N times, each frame revealing a longer stretch of the pen's path
(stroke-dasharray), and every pixel keeps the first frame it appears in. That
reveal map ships beside the coverage (boot_mark_rev, 1..BOOT_MARK_STEPS, 0 =
empty) so the kernel needs no animation data beyond one more byte per pixel.
The pen takes the ink in the SVG's own order (trunk and arms, the heavier
passes down the trunk, the tufts, their blades) and the terracotta ground
last, so the ground finishes the drawing even though it is painted first.

3.0: the ground is terracotta (tools/gen/logo.py's accent), the rest is ink.
Each mark ships a second byte per pixel, *_acc, the part of the coverage that
shows the ground: the full mark's alpha minus the alpha of the tree alone,
because the tree is painted over the ground. The kernel blends coverage minus
acc in its ink and acc in MARK_GROUND.

Usage:
    python3 tools/gen/gen_boot_mark.py            # regenerate kernel/boot_mark.h
    python3 tools/gen/gen_boot_mark.py --check     # fail if the header is stale
"""
import io
import os
import re
import subprocess
import sys

from PIL import Image

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from logo import accent  # noqa: E402  the terracotta ground's colour

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..")
SVG = os.path.join(ROOT, "landing", "logo.svg")
SMALL_SVG = os.path.join(ROOT, "landing", "mark-bold.svg")
OUT = os.path.join(ROOT, "kernel", "boot_mark.h")

STEPS = 48      # frames of the draw-on; the kernel maps elapsed ticks onto 1..STEPS
TARGET_H = 160  # physical px, the splash's real requested tree height
SMALL = 80      # physical px: the menu bar (20 logical at scale 2) is an exact 2x2 average, the About card 32


def svg_aspect():
    txt = open(SVG).read()
    m = re.search(r'viewBox="([\-0-9. ]+)"', txt)
    _, _, w, h = [float(v) for v in m.group(1).split()]
    return w / h


def coverage(svg, w, h):
    """Alpha of an SVG file, or of SVG text passed in as bytes."""
    src = ["-"] if isinstance(svg, bytes) else [svg]
    png = subprocess.run(["rsvg-convert", "-w", str(w), "-h", str(h)] + src, input=svg if src == ["-"] else None,
                         check=True, capture_output=True).stdout
    img = Image.open(io.BytesIO(png)).convert("RGBA")
    assert img.size == (w, h), img.size
    return list(img.split()[3].tobytes())


def layers(svg, w, h):
    """(coverage, ground share): the whole mark's alpha, and how much of it is the terracotta ground that shows,
    the whole mark less the tree drawn alone (the tree is painted over the ground)."""
    txt = open(svg).read()
    ground = f'stroke="{accent}"'
    assert txt.count(ground) == 1, f"{svg}: expected one terracotta ground path"
    full = coverage(svg, w, h)
    tree = coverage(txt.replace(ground, ground + ' stroke-opacity="0"').encode(), w, h)
    return full, [max(a - t, 0) for a, t in zip(full, tree)]


def subpaths():
    """[(stroke_width, stroke, d)] for every subpath of logo.svg, in pen order: the ink in the file's order, then
    the terracotta ground, so the ground finishes the drawing."""
    txt = open(SVG).read()
    ink, ground = [], []
    for m in re.finditer(r'<path d="([^"]+)"([^>]*)/>', txt):
        sw = re.search(r'stroke-width="([\d.]+)"', m.group(2))
        st = re.search(r' stroke="([^"]+)"', m.group(2))
        for d in re.findall(r"M[^M]+", m.group(1)):
            (ground if st and st.group(1) == accent else ink).append((sw.group(1) if sw else None, st and st.group(1), d))
    return ink + ground


def plen(d):
    """The length of one M/C subpath (all logo.py writes), each cubic walked in 24 chords. The frames dash in user
    units: rsvg-convert ignores pathLength, so before 3.0 a stroke popped in whole early in its share of the time."""
    n = [float(v) for v in re.findall(r"-?\d+\.?\d*", d)]
    x, y, total = n[0], n[1], 0.0
    for i in range(2, len(n) - 5, 6):
        x1, y1, x2, y2, x3, y3 = n[i:i + 6]
        px, py = x, y
        for k in range(1, 25):
            t = k / 24; u = 1 - t
            qx = u * u * u * x + 3 * u * u * t * x1 + 3 * u * t * t * x2 + t * t * t * x3
            qy = u * u * u * y + 3 * u * u * t * y1 + 3 * u * t * t * y2 + t * t * t * y3
            total += ((qx - px) ** 2 + (qy - py) ** 2) ** 0.5
            px, py = qx, qy
        x, y = x3, y3
    return total or 1e-6


def frame_svg(subs, lens, frac):
    """logo.svg with only the first `frac` of the pen path drawn."""
    txt = open(SVG).read()
    head = txt[:txt.index("<path")]
    total, reach, start, body = sum(lens), 0.0, 0.0, []
    reach = total * frac
    for (sw, st, d), ln in zip(subs, lens):
        if start >= reach:
            break
        a = (f' stroke="{st}"' if st else "") + (f' stroke-width="{sw}"' if sw else "")
        dash = "" if start + ln <= reach else f' stroke-dasharray="{reach - start:.2f} 2000"'
        body.append(f'<path d="{d}"{a}{dash}/>')
        start += ln
    return head + "".join(body) + "</g></svg>"


def reveal_map(final, w, h):
    subs = subpaths()
    lens = [plen(d) for _, _, d in subs]
    rev = [0] * len(final)
    for k in range(1, STEPS + 1):
        svg = frame_svg(subs, lens, k / STEPS)
        tmp = os.path.join(ROOT, "build-bootmark-frame.svg")
        open(tmp, "w").write(svg)
        try:
            cov = coverage(tmp, w, h)
        finally:
            os.remove(tmp)
        for i, (c, f) in enumerate(zip(cov, final)):
            if f and not rev[i] and c * 5 >= f * 3:
                rev[i] = k
    return [r or (STEPS if f else 0) for r, f in zip(rev, final)]


def array(name, cov, w):
    out = [f"static const unsigned char {name}[{len(cov)}] __attribute__((unused)) = {{"]
    for i in range(0, len(cov), 20):
        out.append("    " + ",".join(str(v) for v in cov[i:i + 20]) + ",")
    out.append("};")
    return out


def main():
    aspect = svg_aspect()
    h = TARGET_H
    w = int(round(h * aspect))
    cov, acc = layers(SVG, w, h)
    small, small_acc = layers(SMALL_SVG, SMALL, SMALL)
    rev = reveal_map(cov, w, h)

    lines = []
    lines.append("/* Generated by tools/gen/gen_boot_mark.py. Do not hand-edit. */")
    lines.append("#ifndef BOOT_MARK_HDR")
    lines.append("#define BOOT_MARK_HDR")
    lines.append("")
    lines.append("#define MARK_GROUND 0x001C1C1E   /* 3.0.0: flat ink, the terracotta ground is gone */")
    lines.append("")
    lines.append("/* The splash: landing/logo.svg, the 3.0 crayon tree. _acc is the part of each pixel's coverage that is the ground. */")
    lines.append(f"#define BOOT_MARK_W {w}")
    lines.append(f"#define BOOT_MARK_H {h}")
    lines.append("")
    lines += array("boot_mark_cov", cov, w)
    lines += array("boot_mark_acc", acc, w)
    lines.append("")
    lines.append("/* The draw-on: the frame (1 to BOOT_MARK_STEPS) each pixel first appears in as the pen traces the tree, ground last, 0 where empty. */")
    lines.append(f"#define BOOT_MARK_STEPS {STEPS}")
    lines += array("boot_mark_rev", rev, w)
    lines.append("")
    lines.append("/* The menu bar: landing/mark-bold.svg, the same tree drawn down its middle so it reads at 20 px. */")
    lines.append(f"#define MENU_MARK_W {SMALL}")
    lines.append(f"#define MENU_MARK_H {SMALL}")
    lines.append("")
    lines += array("menu_mark_cov", small, SMALL)
    lines += array("menu_mark_acc", small_acc, SMALL)
    lines.append("")
    lines.append("#endif")
    text = "\n".join(lines) + "\n"

    if "--check" in sys.argv:
        cur = open(OUT).read() if os.path.exists(OUT) else ""
        if cur != text:
            print("kernel/boot_mark.h is stale, run tools/gen/gen_boot_mark.py", file=sys.stderr)
            sys.exit(1)
        print("kernel/boot_mark.h up to date")
        return

    with open(OUT, "w") as f:
        f.write(text)
    print(f"wrote {OUT}: splash {w}x{h}, menu bar {SMALL}x{SMALL}")


if __name__ == "__main__":
    main()
