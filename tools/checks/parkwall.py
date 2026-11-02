"""8.9.0: the Pi's default wallpaper (arch/arm64/park_wall.h) worked out per screen pixel exactly as wall.c's wall_span draws it,
for the checks that read bare wallpaper on a Pi image. park(x, y, w, h) is the (r, g, b) at x, y of a w x h screen."""
import os, re

_src = open(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "arch/arm64/park_wall.h")).read()
_pal = [int(v, 16) for v in re.search(r"park_pal\[16\] = \{([^}]*)\}", _src).group(1).replace("u", "").split(",")]
_px = bytes(int(v) for v in re.search(r"park_px\[\d+\] = \{([^}]*)\}", _src, re.S).group(1).split(",") if v.strip())
PW, PH = 480, 270

def park(x, y, w, h):
    step = (PW << 16) // w; px = (x * step) >> 16; py = y * PH // h
    b = _px[(py * PW + px) // 2]; c = _pal[b & 15 if px & 1 else b >> 4]
    r, g, bl = c >> 16 & 255, c >> 8 & 255, c & 255
    if y & 3 == 3: r, g, bl = r * 4 // 5, g * 4 // 5, bl * 4 // 5
    return (r, g, bl)
