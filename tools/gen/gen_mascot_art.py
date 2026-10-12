#!/usr/bin/env python3
"""The mascot as a PNG the ARM64 kernel decodes at run time: arch/arm64/mascot_art.h.

landing/mascot.svg's drawing (tools/gen/mascot.py) without the crayon grain, rasterized to 192 x 192 RGBA on nothing:
96 logical pixels at the Pi's 2x scale, so it lands 1:1 at 1080p and is sampled down at 800x600. The kernel's own
png_decode (drivers/png.h, 8-bit RGBA) reads it; the Browser's empty and error screens draw it.

Run from the repo root:  python3 tools/gen/gen_mascot_art.py      Uses rsvg-convert and Pillow.
"""
import io
import os
import subprocess

from PIL import Image

import logo as L
import mascot as M

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..")
SIZE = 192


def main():
    svg = L.svg(L.paths(M.layers()), "0 0 100 100")
    png = subprocess.run(["rsvg-convert", "-w", str(SIZE), "-h", str(SIZE), "-"], input=svg.encode(), check=True,
                         capture_output=True).stdout
    buf = io.BytesIO()
    Image.open(io.BytesIO(png)).convert("RGBA").save(buf, "PNG", optimize=True)
    data = buf.getvalue()
    rows = [", ".join(f"0x{b:02x}" for b in data[i:i + 16]) for i in range(0, len(data), 16)]
    out = ("/* The mascot, 192 x 192 RGBA, written by tools/gen/gen_mascot_art.py from tools/gen/mascot.py. Do not edit. */\n"
           "#ifndef MASCOT_ART_H\n#define MASCOT_ART_H\n"
           f"#define MASCOT_PX {SIZE}\n#define MASCOT_PNG_LEN {len(data)}\n"
           "static const unsigned char mascot_png[MASCOT_PNG_LEN] = {\n    " + ",\n    ".join(rows) + "\n};\n#endif\n")
    with open(os.path.join(ROOT, "arch", "arm64", "mascot_art.h"), "w") as fh:
        fh.write(out)
    print(f"wrote arch/arm64/mascot_art.h ({len(data)} bytes of PNG)")


if __name__ == "__main__":
    main()
