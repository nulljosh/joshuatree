#!/bin/sh
# Turns a sibling repo's single-file static app into a C byte array the
# kernel can serve. ponytail: hardcodes ~/Documents/Code's sibling-checkout
# layout (documented convention across this user's repos, see
# ~/Documents/Code/CLAUDE.md) rather than a configurable path, and only
# handles the one-file case, real multi-file apps need their own pass.
# Generated, not checked in: drivers/app_weather.h stays out of git (weather's
# own repo is the source of truth), regenerate with this script before a
# clean build.
set -e
SRC="${1:-$HOME/Documents/Code/weather/web/index.html}"
OUT="${2:-drivers/app_weather.h}"
NAME="${3:-app_weather}"
# Second app: ./gen_app.sh "$HOME/Documents/Code/curbfind/web/index.html" drivers/app_curbfind.h app_curbfind

python3 - "$SRC" "$OUT" "$NAME" <<'EOF'
import sys
src, out, name = sys.argv[1], sys.argv[2], sys.argv[3]
data = open(src, 'rb').read()
guard = name.upper() + "_H"
with open(out, 'w') as f:
    f.write(f"#ifndef {guard}\n#define {guard}\n")
    f.write(f"static const unsigned int {name}_len = {len(data)};\n")
    f.write(f"static const unsigned char {name}_html[] = {{\n")
    for i in range(0, len(data), 20):
        f.write(','.join(str(b) for b in data[i:i+20]) + ',\n')
    f.write("};\n#endif\n")
print(f"wrote {out} ({len(data)} bytes from {src})")
EOF
