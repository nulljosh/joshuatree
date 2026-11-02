#!/usr/bin/env python3
"""The mascot as a PNG the ARM64 kernel decodes at run time: arch/arm64/mascot_art.h.

7.10: the painted tree (docs/brand/tree.png), the brand mark, in place of the crayon mascot, at 192 x 192 RGBA on nothing:
96 logical pixels at the Pi's 2x scale, so it lands 1:1 at 1080p and is sampled down at 800x600. The kernel's own
png_decode (drivers/png.h, 8-bit RGBA) reads it; the Browser's empty and error screens draw it.

Run from the repo root:  python3 tools/gen/gen_mascot_art.py      Uses Pillow.
"""
import io
import os

from PIL import Image

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..")
SIZE = 192


def main():
    buf = io.BytesIO()
    Image.open(os.path.join(ROOT, "docs", "brand", "tree.png")).convert("RGBA").resize((SIZE, SIZE), Image.LANCZOS).save(buf, "PNG", optimize=True)
    data = buf.getvalue()
    rows = [", ".join(f"0x{b:02x}" for b in data[i:i + 16]) for i in range(0, len(data), 16)]
    out = ("/* The mascot, 192 x 192 RGBA, written by tools/gen/gen_mascot_art.py from docs/brand/tree.png. Do not edit. */\n"
           "#ifndef MASCOT_ART_H\n#define MASCOT_ART_H\n"
           f"#define MASCOT_PX {SIZE}\n#define MASCOT_PNG_LEN {len(data)}\n"
           "static const unsigned char mascot_png[MASCOT_PNG_LEN] = {\n    " + ",\n    ".join(rows) + "\n};\n#endif\n")
    with open(os.path.join(ROOT, "arch", "arm64", "mascot_art.h"), "w") as fh:
        fh.write(out)
    print(f"wrote arch/arm64/mascot_art.h ({len(data)} bytes of PNG)")


if __name__ == "__main__":
    main()
