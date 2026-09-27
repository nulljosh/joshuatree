#!/usr/bin/env bash
# Cut Samantha's Chat face frames from her character videos.
#
# Reads idle.mp4 and talk.mp4 from ~/.samantha/characters/<name>/ (the
# character-creator skill writes them) and writes small square PNGs to
# landing/face/: idle-0..3.png and talk-0..7.png. The kernel's Chat app
# fetches these over plain HTTP from joshuatree.heyitsmejosh.com, so a new
# look is one command plus a deploy.
#
# Frames are 120x120, 8-bit indexed (palette) PNGs, the format
# drivers/png.c decodes, each well under 16KB.
#
# Usage: tools/gen/face_frames.sh [character]
# Env:   CROP=w:h:x:y   square crop on her face in the source video
#                       (default fits the 854x480 samantha loops)
set -euo pipefail
cd "$(dirname "$0")/../.."

NAME="${1:-samantha}"
SRC="${FACE_SRC:-$HOME/.samantha/characters/$NAME}"
CROP="${CROP:-480:480:80:0}"
SIZE=120
IDLE=4
TALK=8
OUT=landing/face

for v in idle talk; do
    [ -f "$SRC/$v.mp4" ] || { echo "face_frames: missing $SRC/$v.mp4" >&2; exit 1; }
done
mkdir -p "$OUT"
rm -f "$OUT"/idle-*.png "$OUT"/talk-*.png

# One frame at evenly spaced times across the clip's own length.
cut_frames() {
    local kind=$1 count=$2 dur t i
    dur=$(ffprobe -v error -show_entries format=duration -of csv=p=0 "$SRC/$kind.mp4")
    for ((i = 0; i < count; i++)); do
        t=$(awk -v d="$dur" -v i="$i" -v n="$count" 'BEGIN { printf "%.3f", d * (i + 0.5) / n }')
        ffmpeg -v error -y -ss "$t" -i "$SRC/$kind.mp4" -frames:v 1 \
            -vf "crop=$CROP,scale=$SIZE:$SIZE:flags=lanczos,split[a][b];[a]palettegen=max_colors=256:stats_mode=single[p];[b][p]paletteuse=dither=none" \
            -pix_fmt pal8 "$OUT/$kind-$i.png"
    done
}
cut_frames idle "$IDLE"
cut_frames talk "$TALK"

for f in "$OUT"/idle-*.png "$OUT"/talk-*.png; do
    echo "face_frames: $f $(wc -c < "$f" | tr -d ' ') bytes"
done
