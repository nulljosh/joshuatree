#!/usr/bin/env bash
# Regenerate progress.svg from roadmap.md's checkbox counts. Run after checking
# off or adding roadmap items. ponytail: parses "- [x]"/"- [ ]" lines directly,
# no markdown library needed for a format this simple.
set -euo pipefail
cd "$(dirname "$0")"

versions=()
totals=()
dones=()
cur=""
done_n=0
total_n=0

flush() {
  if [ -n "$cur" ]; then
    versions+=("$cur"); totals+=("$total_n"); dones+=("$done_n")
  fi
}

while IFS= read -r line; do
  if [[ "$line" =~ ^##[[:space:]]+(v[0-9]+) ]]; then
    flush
    cur="${BASH_REMATCH[1]}"
    total_n=0; done_n=0
  elif [[ "$line" =~ ^-\ \[x\] ]]; then
    total_n=$((total_n+1)); done_n=$((done_n+1))
  elif [[ "$line" =~ ^-\ \[\ \] ]]; then
    total_n=$((total_n+1))
  fi
done < roadmap.md
flush

bar_h=28
gap=10
pad=20
label_w=50
bar_w=360
width=$((pad*2 + label_w + bar_w + 50))
height=$((pad*2 + ${#versions[@]} * (bar_h + gap)))

svg="<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"$width\" height=\"$height\" viewBox=\"0 0 $width $height\">"
svg+="<rect width=\"100%\" height=\"100%\" fill=\"#ffffff\"/>"
y=$pad
for i in "${!versions[@]}"; do
  v="${versions[$i]}"; t="${totals[$i]}"; d="${dones[$i]}"
  pct=0
  [ "$t" -gt 0 ] && pct=$((d * 100 / t))
  fill_w=$((bar_w * pct / 100))
  svg+="<text x=\"$pad\" y=\"$((y + bar_h/2 + 4))\" font-family=\"-apple-system,Helvetica,Arial,sans-serif\" font-size=\"13\" fill=\"#111\">$v</text>"
  svg+="<rect x=\"$((pad+label_w))\" y=\"$y\" width=\"$bar_w\" height=\"$bar_h\" rx=\"4\" fill=\"#eee\" stroke=\"#ddd\"/>"
  color="#111"
  [ "$pct" -eq 100 ] && color="#2e7d32"
  svg+="<rect x=\"$((pad+label_w))\" y=\"$y\" width=\"$fill_w\" height=\"$bar_h\" rx=\"4\" fill=\"$color\"/>"
  svg+="<text x=\"$((pad+label_w+bar_w+10))\" y=\"$((y + bar_h/2 + 4))\" font-family=\"-apple-system,Helvetica,Arial,sans-serif\" font-size=\"12\" fill=\"#666\">$d/$t</text>"
  y=$((y + bar_h + gap))
done
svg+="</svg>"

echo "$svg" > progress.svg
mkdir -p landing
cp progress.svg landing/progress.svg

sum_d=0; sum_t=0
for i in "${!dones[@]}"; do sum_d=$((sum_d + dones[i])); sum_t=$((sum_t + totals[i])); done
echo "wrote progress.svg ($sum_d/$sum_t items done)"
