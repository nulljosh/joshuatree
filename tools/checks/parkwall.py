"""The Pi's default wallpaper (arch/arm64/park_wall.h) worked out per screen pixel exactly as wall.c's wall_span draws it,
for the checks that read bare wallpaper on a Pi image. park(x, y, w, h) is the (r, g, b) at x, y of a w x h screen.
13.27.0: 256 colours, a byte a pixel, no scanline."""
import os, re, zlib

_src = open(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "arch/arm64/park_wall.h")).read()
_pal = [int(v, 16) for v in re.search(r"park_pal\[256\] = \{([^}]*)\}", _src).group(1).replace("u", "").split(",")]
_px = zlib.decompress(bytes(int(v) for v in re.search(r"park_z\[\d+\] = \{([^}]*)\}", _src, re.S).group(1).split(",") if v.strip()), -15)   # 13.27.3: deflated
PW, PH = (int(re.search(r"#define PARK_%s (\d+)" % k, _src).group(1)) for k in "WH")

def park(x, y, w, h):
    step = (PW << 16) // w; px = (x * step) >> 16; py = y * PH // h
    c = _pal[_px[py * PW + px]]
    return (c >> 16 & 255, c >> 8 & 255, c & 255)
