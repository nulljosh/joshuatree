#!/usr/bin/env python3
"""Rasterize the authored icon SVGs in art/icons/ into a C header of PNG
bytes, the same host-side-generator pattern gen_vga_font.py and
gen_png_testdata.py already use in this repo.

Why authored art instead of more primitive tweaking: every dock icon used
to be assembled at runtime out of a dozen gui_* primitive calls (rounded
rects, capsules, circles, wedges) inside gui_render_icon_cached's 6x
supersample. That approach has a real ceiling. A primitive pass can give a
shape a clean anti-aliased edge, but it cannot give it per-pixel interior
shading, a soft glyph drop shadow with a real blur kernel, or a rim light
that fades along the squircle's own curve, which is exactly what makes a
real macOS icon read as an object rather than as pixel art. Those are
authoring decisions, so they belong in authored artwork.

rsvg-convert does the
rasterizing on the host, at build time, at one size. The kernel png_decode()s
an icon on a cache miss, area-averages it down to whatever physical size the
dock is actually drawing at, composites it over the surface colour and frees
the decoded buffer. No primitive assembly, and no new decoder: drivers/png.c
has shipped since v74 and rsvg emits exactly its supported subset.

One size, not a size ladder, on purpose: the real physical sizes this
kernel draws an icon at are 74 (dock, dock_scale_pct 7 at 960x540@2x), 92
(dock magnified) and 120 (Apps folder grid). All three are DOWNsamples of
128, which is where authored art wins and where an area filter is exact
and cheap. A ladder of pre-rendered sizes would cost 3x the kernel image
for pixels the box filter already reproduces.

Kernel-image budget. Storing decoded RGBA was the first design: 24 artworks
at 128*128*4 is 1.5MB, and that collided with the ring-3 program window
boot/linker.ld pins at 0xC0500000, which docs/SYSCALL-ABI.md names as part
of the published v1 contract. As PNG the same 24 artworks are 141KB. Run
this with --budget for the real figure.

Usage:
    python3 tools/gen/gen_icon_art.py            # regenerate kernel/icon_art.h
    python3 tools/gen/gen_icon_art.py --check    # fail if the header is stale
"""
import os
import struct
import subprocess
import sys

SIZE = 128

# Icon index in kernel.c's GUI_LABELS / GUI_COLORS order -> SVG basename.
# Only the icons that have real authored art are listed; every other index
# keeps the existing primitive path, so this can be filled in incrementally.
ART = {
    0: "files",
    1: "mail",
    2: "calendar",
    3: "notes",
    4: "reminders",
    5: "terminal",
    6: "chat",
    7: "weather",
    8: "curbfind",
    9: "keyrate",
    10: "bookrank",
    11: "quotes",
    12: "plan",
    13: "lexly",
    14: "toroid",
    15: "sparkjar",
    16: "homeqi",
    17: "fieldbook",
    18: "contacts",
    19: "calculator",
    20: "stocks",
    21: "search",
    22: "epiphany",
    24: "apps",
    25: "trash",
}

# Icons whose glyph depends on runtime state get a second artwork keyed by
# gui_render_icon_cached's existing `variant`. Trash is the only one today:
# the primitive gui_icon_trash draws two crumpled sheets above the rim when
# trash_count() > 0, and converting it to a single static artwork would have
# silently thrown that away, turning a real state indicator into decoration.
VARIANT = {
    25: "trash_full",
}

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..")
SVG_DIR = os.path.join(ROOT, "art", "icons")
OUT = os.path.join(ROOT, "kernel", "icon_art.h")


def rasterize(name):
    """The rsvg PNG bytes, stored as-is.

    Storing decoded RGBA was the first design and it did not survive contact
    with the real kernel image. 24 artworks at 128*128*4 is 1.5MB, which
    collided with the ring-3 program window boot/linker.ld pins at
    0xC0500000 (v0.80.0's syscall ABI), and that address is named in
    docs/SYSCALL-ABI.md as part of the published v1 contract, so it is not
    something an icon pass gets to move. The same bytes as PNG are 141KB,
    a 10.9x saving, and they need no new decoder: drivers/png.c has shipped
    since v74 with byte-exact regression tests, and rsvg-convert emits
    precisely the subset it supports. Verified, not assumed: every file's
    IHDR reads 128x128, 8-bit, colour type 6 (RGBA), compression 0,
    filter 0, interlace 0, and the assertions below re-check that on every
    regeneration rather than trusting rsvg to keep doing it.
    """
    png = subprocess.run(
        ["rsvg-convert", "-w", str(SIZE), "-h", str(SIZE),
         os.path.join(SVG_DIR, name + ".svg")],
        check=True, stdout=subprocess.PIPE).stdout
    if png[:8] != b"\x89PNG\r\n\x1a\n":
        raise SystemExit("%s: rsvg-convert did not produce a PNG" % name)
    w, h, depth, ctype, comp, filt, inter = struct.unpack(">IIBBBBB", png[16:29])
    if (w, h) != (SIZE, SIZE):
        raise SystemExit("%s rasterized to %dx%d, expected %dx%d" % (name, w, h, SIZE, SIZE))
    if (depth, ctype, comp, filt, inter) != (8, 6, 0, 0, 0):
        raise SystemExit(
            "%s: IHDR is depth %d colour-type %d interlace %d; drivers/png.c only "
            "decodes 8-bit RGBA non-interlaced, so this would fail at runtime"
            % (name, depth, ctype, inter))
    return png


def c_array(name, data):
    out = ["static const unsigned char icon_art_%s[%d] = {" % (name, len(data))]
    for i in range(0, len(data), 16):
        out.append("    " + ",".join("%d" % b for b in data[i:i + 16]) + ",")
    out.append("};")
    out.append("#define ICON_ART_%s_LEN %d" % (name.upper(), len(data)))
    return "\n".join(out)


_sizes = []


def build():
    parts = ["""/* GENERATED by tools/gen/gen_icon_art.py from the SVGs in art/icons -- do not edit.
   Authored icon artwork, rasterized on the host at %dx%d and stored as
   the PNG bytes rsvg-convert produced (8-bit RGBA, non-interlaced, exactly
   drivers/png.c's supported subset). The kernel png_decode()s one on an
   icon-cache miss, area-averages it to the physical size it is drawing at,
   composites it over the surface colour and frees the decoded buffer; see
   gui_render_icon_cached in kernel/kernel.c.
   Regenerate with: python3 tools/gen/gen_icon_art.py */
#ifndef ICON_ART_H
#define ICON_ART_H

#define ICON_ART_SIZE %d
""" % (SIZE, SIZE, SIZE)]

    for name in [ART[i] for i in sorted(ART)] + [VARIANT[i] for i in sorted(VARIANT)]:
        blob = rasterize(name)
        _sizes.append(blob)
        parts.append(c_array(name, blob))
        parts.append("")

    n = max(max(ART), max(VARIANT)) + 1
    parts.append("/* Icon index -> artwork, 0 where no authored art exists yet (that icon")
    parts.append("   keeps the runtime primitive path). Sized by the caller's GUI_APP_COUNT. */")
    parts.append("static const unsigned char *const ICON_ART[%d] = {" % n)
    for i in range(n):
        parts.append("    %s," % ("icon_art_" + ART[i] if i in ART else "0"))
    parts.append("};")
    parts.append("static const unsigned int ICON_ART_LEN[%d] = {" % n)
    for i in range(n):
        parts.append("    %s," % ("ICON_ART_%s_LEN" % ART[i].upper() if i in ART else "0"))
    parts.append("};")
    parts.append("")
    parts.append("/* The variant-1 artwork for the icons that have runtime state, 0 for")
    parts.append("   the rest. Indexed the same way, selected by gui_render_icon_cached's")
    parts.append("   own `variant` so the empty/full Trash distinction survives. */")
    parts.append("static const unsigned char *const ICON_ART_VARIANT[%d] = {" % n)
    for i in range(n):
        parts.append("    %s," % ("icon_art_" + VARIANT[i] if i in VARIANT else "0"))
    parts.append("};")
    parts.append("static const unsigned int ICON_ART_VARIANT_LEN[%d] = {" % n)
    for i in range(n):
        parts.append("    %s," % ("ICON_ART_%s_LEN" % VARIANT[i].upper() if i in VARIANT else "0"))
    parts.append("};")
    parts.append("#define ICON_ART_COUNT %d" % n)
    parts.append("")
    parts.append("#endif")
    parts.append("")
    return "\n".join(parts)


def main():
    text = build()
    if "--budget" in sys.argv:
        n = sum(len(rasterize(v)) for v in list(ART.values()) + list(VARIANT.values()))
        print("%d artworks, %d bytes of .rodata (%.0f KB); as decoded RGBA it would be %d"
              % (len(ART) + len(VARIANT), n, n / 1024.0, (len(ART) + len(VARIANT)) * SIZE * SIZE * 4))
        return 0
    if "--check" in sys.argv:
        have = open(OUT).read() if os.path.exists(OUT) else ""
        if have != text:
            print("FAIL: kernel/icon_art.h is stale, re-run tools/gen/gen_icon_art.py")
            return 1
        print("PASS: kernel/icon_art.h matches art/icons/*.svg")
        return 0
    open(OUT, "w").write(text)
    print("wrote %s (%d artworks, %d bytes of PNG)"
          % (OUT, len(ART) + len(VARIANT), sum(len(m) for m in _sizes)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
