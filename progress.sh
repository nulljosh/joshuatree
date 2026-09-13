#!/usr/bin/env bash
# Regenerate progress.svg: a line graph of cumulative capability (items
# checked off) across versions, from roadmap.md's checkbox counts and the
# "## Done" collapsed-version marker. Run after checking off/adding items.
set -euo pipefail
cd "$(dirname "$0")"

labels=()
cum=()
running=0

total=0

marker="$(grep -o 'done-items [0-9]*/[0-9]*' roadmap.md | head -1 || true)"
if [ -n "$marker" ]; then
  d="${marker#done-items }"; t="${marker#*/}"; d="${d%/*}"
  running=$((running + d)); total=$((total + t))
  labels+=("done"); cum+=("$running")
fi

cur=""; count=0; vtotal=0
flush() { if [ -n "$cur" ]; then running=$((running + count)); total=$((total + vtotal)); labels+=("$cur"); cum+=("$running"); fi; }

while IFS= read -r line; do
  if [[ "$line" =~ ^##[[:space:]]+(v[0-9]+) ]]; then
    flush
    cur="${BASH_REMATCH[1]}"; count=0; vtotal=0
  elif [[ "$line" =~ ^-\ \[x\] ]]; then
    count=$((count+1)); vtotal=$((vtotal+1))
  elif [[ "$line" =~ ^-\ \[\ \] ]]; then
    vtotal=$((vtotal+1))
  fi
done < roadmap.md
flush

n=${#labels[@]}
max=$total
[ "$max" -eq 0 ] && max=1

pad_l=30; pad_r=16; pad_t=16; pad_b=28
plot_w=420; plot_h=140
width=$((pad_l + plot_w + pad_r))
height=$((pad_t + plot_h + pad_b))

points=""
dots=""
for i in "${!labels[@]}"; do
  x=$((pad_l + i * plot_w / (n - 1 > 0 ? n - 1 : 1)))
  y=$((pad_t + plot_h - cum[i] * plot_h / max))
  points+="$x,$y "
  dots+="<circle cx=\"$x\" cy=\"$y\" r=\"3\" fill=\"#111\"/>"
done

half=$((max / 2))

svg="<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"$width\" height=\"$height\" viewBox=\"0 0 $width $height\">"
svg+="<rect width=\"100%\" height=\"100%\" fill=\"#ffffff\"/>"
# y-axis gridlines + labels at 0, half, max
svg+="<line x1=\"$pad_l\" y1=\"$pad_t\" x2=\"$((pad_l+plot_w))\" y2=\"$pad_t\" stroke=\"#eee\"/>"
svg+="<text x=\"2\" y=\"$((pad_t+3))\" font-family=\"-apple-system,Helvetica,Arial,sans-serif\" font-size=\"9\" fill=\"#999\">$max</text>"
svg+="<line x1=\"$pad_l\" y1=\"$((pad_t+plot_h/2))\" x2=\"$((pad_l+plot_w))\" y2=\"$((pad_t+plot_h/2))\" stroke=\"#eee\"/>"
svg+="<text x=\"2\" y=\"$((pad_t+plot_h/2+3))\" font-family=\"-apple-system,Helvetica,Arial,sans-serif\" font-size=\"9\" fill=\"#999\">$half</text>"
svg+="<line x1=\"$pad_l\" y1=\"$pad_t\" x2=\"$pad_l\" y2=\"$((pad_t+plot_h))\" stroke=\"#ddd\"/>"
svg+="<line x1=\"$pad_l\" y1=\"$((pad_t+plot_h))\" x2=\"$((pad_l+plot_w))\" y2=\"$((pad_t+plot_h))\" stroke=\"#ddd\"/>"
svg+="<text x=\"2\" y=\"$((pad_t+plot_h+3))\" font-family=\"-apple-system,Helvetica,Arial,sans-serif\" font-size=\"9\" fill=\"#999\">0</text>"
svg+="<polyline points=\"$points\" fill=\"none\" stroke=\"#111\" stroke-width=\"2\"/>"
svg+="$dots"
for i in "${!labels[@]}"; do
  x=$((pad_l + i * plot_w / (n - 1 > 0 ? n - 1 : 1)))
  svg+="<text x=\"$x\" y=\"$((pad_t+plot_h+16))\" font-family=\"-apple-system,Helvetica,Arial,sans-serif\" font-size=\"10\" fill=\"#666\" text-anchor=\"middle\">${labels[$i]}</text>"
done
svg+="<text x=\"$pad_l\" y=\"$((height-4))\" font-family=\"-apple-system,Helvetica,Arial,sans-serif\" font-size=\"10\" fill=\"#666\">${cum[$((n-1))]} of $max features shipped</text>"
svg+="</svg>"

echo "$svg" > progress.svg
mkdir -p landing
cp progress.svg landing/progress.svg
echo "wrote progress.svg (cumulative through ${labels[$((n-1))]}: ${cum[$((n-1))]} items)"
