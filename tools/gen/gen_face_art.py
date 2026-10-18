#!/usr/bin/env python3
"""Samantha's face as PNGs the ARM64 kernel decodes at run time: arch/arm64/face_art.h.

Two frames from landing/face (the portrait the web Samantha uses): idle-0.jpg with her mouth closed and talk-24.jpg with
it open, each 192 x 192 RGBA (96 logical pixels at the Pi's 2x scale). The chat view (arch/arm64/main.c face_blit) blends
the two by how loud her voice is at that moment, so her mouth opens with what she says. The kernel's own png_decode reads
them. Run from the repo root:  python3 tools/gen/gen_face_art.py      Uses Pillow.
"""
import io, os
from PIL import Image

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..")
SIZE = 192
FRAMES = [("idle", "landing/face/idle-0.jpg"), ("talk", "landing/face/talk-24.jpg")]


def main():
    out = ["/* Samantha's face, %d x %d RGBA, two frames (mouth closed, mouth open), written by tools/gen/gen_face_art.py from landing/face. Do not edit. */" % (SIZE, SIZE),
           "#ifndef FACE_ART_H", "#define FACE_ART_H", "#define FACE_PX %d" % SIZE]
    for name, path in FRAMES:
        im = Image.open(os.path.join(ROOT, path)).convert("RGB").resize((SIZE, SIZE), Image.LANCZOS).convert("RGBA")
        buf = io.BytesIO(); im.save(buf, "PNG", optimize=True); png = buf.getvalue()
        out.append("#define FACE_%s_LEN %d" % (name.upper(), len(png)))
        out.append("static const unsigned char face_%s_png[FACE_%s_LEN] = {" % (name, name.upper()))
        for i in range(0, len(png), 16): out.append("    " + ", ".join("0x%02x" % b for b in png[i:i + 16]) + ",")
        out.append("};")
    out.append("#endif")
    open(os.path.join(ROOT, "arch/arm64/face_art.h"), "w").write("\n".join(out) + "\n")
    print("face_art.h:", ", ".join("%s %d bytes" % (n, len(open(os.path.join(ROOT, p), "rb").read())) for n, p in FRAMES))


if __name__ == "__main__": main()
