#!/bin/sh
# Regenerates drivers/wallpaper.h from a local image file. No image decoder
# exists in this freestanding kernel (deliberately, same scope note as
# gen_app.sh's HTML embedding), so this is the same "raw bytes in, no
# parsing needed" approach, just for pixels instead of markup: crop to the
# desktop's own aspect ratio, downsample small enough to keep the embedded
# array a sane size, dump as flat RGB bytes.
#
# Current wallpaper source: a real, verified public domain U.S. National
# Park Service photo, "A Joshua tree, Snow, and San Jacinto"
# (https://commons.wikimedia.org/wiki/File:A_Joshua_tree,_Snow,_and_San_Jacinto_(51765780773).jpg),
# confirmed PD-USGov on its own Commons file page (a work of an NPS
# employee's official duties, no attribution legally required), not an
# eyeballed "probably fine to use" guess.
set -e
cd "$(dirname "$0")"
SRC="${1:?usage: ./gen_wallpaper.sh <source-image> [out-w] [out-h]}"
OUT_W="${2:-240}"
OUT_H="${3:-171}"

python3 - "$SRC" "$OUT_W" "$OUT_H" <<'EOF'
import sys
from PIL import Image

src, out_w, out_h = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])
im = Image.open(src).convert('RGB')
w, h = im.size
target_ratio = out_w / out_h
cur_ratio = w / h
if cur_ratio > target_ratio:
    new_w = int(h * target_ratio)
    x0 = (w - new_w) // 2
    im = im.crop((x0, 0, x0 + new_w, h))
else:
    new_h = int(w / target_ratio)
    y0 = (h - new_h) // 2
    im = im.crop((0, y0, w, y0 + new_h))
im = im.resize((out_w, out_h), Image.LANCZOS)
data = im.tobytes()

with open('drivers/wallpaper.h', 'w') as f:
    f.write('#ifndef WALLPAPER_HDR_H\n#define WALLPAPER_HDR_H\n')
    f.write(f'#define WALLPAPER_W {out_w}\n#define WALLPAPER_H {out_h}\n')
    f.write('static const unsigned char wallpaper_rgb[] = {\n')
    for i in range(0, len(data), 24):
        f.write(','.join(str(b) for b in data[i:i+24]) + ',\n')
    f.write('};\n#endif\n')
print(f'wrote drivers/wallpaper.h ({out_w}x{out_h}, {len(data)} bytes)')
EOF
