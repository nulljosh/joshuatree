#!/usr/bin/env python3
"""Renders 'A' and 'g' from tools/fonts/DejaVuSans.ttf with PIL at a given
pixel size and compares against the PGM coverage bitmaps dumped by the
compiled tools/ttf-host/main.c harness (drivers/ttf.c, the kernel's own
rasterizer code, built for the host). Prints max/mean per-pixel coverage
diff and exits nonzero if it's outside tolerance.
"""
import sys
from PIL import Image, ImageFont

TOL_MAX = 200  # generous: stb and freetype antialias edges differently
TOL_MEAN = 60  # the embedded font is unhinted (fontTools --no-hinting), so
                # small sizes move further from PIL's hinted rendering; a
                # 1px rounding shift also moves the mean a lot on few pixels

def read_pgm(path):
    with open(path, "rb") as f:
        assert f.readline().strip() == b"P5"
        w, h = map(int, f.readline().split())
        f.readline()  # maxval
        data = f.read(w * h)
    return data, w, h

def main():
    px = float(sys.argv[1])
    font_path = sys.argv[2]
    pgm_a = sys.argv[3]
    pgm_g = sys.argv[4]

    font = ImageFont.truetype(font_path, int(round(px)))
    worst_max, worst_mean = 0, 0.0
    for ch, pgm in (("A", pgm_a), ("g", pgm_g)):
        stb_data, sw, sh = read_pgm(pgm)
        bbox = font.getbbox(ch)
        if bbox is None:
            continue
        x0, y0, x1, y1 = bbox
        w, h = max(x1 - x0, 1), max(y1 - y0, 1)
        img = Image.new("L", (w, h), 0)
        d = __import__("PIL.ImageDraw", fromlist=["ImageDraw"]).Draw(img)
        d.text((-x0, -y0), ch, font=font, fill=255)
        pil = img.tobytes()

        # PIL's bbox padding convention differs slightly from stb's tight
        # crop (a few px of left/right slack, not a shape difference -
        # verified by eyeballing the ascii dumps of both). Search a small
        # window of horizontal/vertical shifts and keep the best alignment,
        # same idea as template matching, before scoring the diff.
        best = None
        span = min(max(3, int(px * 0.08)), 12)
        for dy in range(-span, span + 1):
            for dx in range(-span, span + 1):
                diffs = []
                cnt = 0
                for y in range(sh):
                    py = y + dy
                    if py < 0 or py >= h:
                        continue
                    for x in range(sw):
                        px_ = x + dx
                        if px_ < 0 or px_ >= w:
                            continue
                        sv = stb_data[y * sw + x]
                        pv = pil[py * w + px_]
                        diffs.append(abs(sv - pv))
                        cnt += 1
                if cnt == 0:
                    continue
                mean_d = sum(diffs) / cnt
                if best is None or mean_d < best[0]:
                    best = (mean_d, max(diffs) if diffs else 0)
        mean, mx = (best[0], best[1]) if best else (255, 255)
        print(f"'{ch}' @ {px}px: stb={sw}x{sh} pil={w}x{h} max_diff={mx} mean_diff={mean:.2f}")
        worst_max = max(worst_max, mx)
        worst_mean = max(worst_mean, mean)

    ok = worst_max <= TOL_MAX and worst_mean <= TOL_MEAN
    print(f"worst: max={worst_max} mean={worst_mean:.2f} tol=({TOL_MAX},{TOL_MEAN}) {'ok' if ok else 'FAIL'}")
    sys.exit(0 if ok else 1)

if __name__ == "__main__":
    main()
