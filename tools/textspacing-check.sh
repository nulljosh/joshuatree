#!/bin/bash
# v77 (0.67.1): headless check for the "Cl oudy" menu-bar spacing bug.
# Boots kernel.elf under `-display none`, drops out of the GUI with esc,
# types `texttest` through the monitor's sendkey path (the same one
# tools/png-check.sh uses), and reads the verdict off the serial port.
# The kernel renders "Cloudy" through the real font_draw_string +
# gui_aa_char path at scale 2, measures the actual ink columns per glyph
# via window_get_pixel_phys, and prints the inter-letter gap spread and
# the lone-'W' ink width. Both lines must say ok. Discriminating: with the
# fixed-16px-cell renderer this fails with spread 8 (C-l gap 1, l-o gap 9)
# and W clipped to 16; see roadmap.md's v77 entry for the reverted run.
set -e
cd "$(dirname "$0")/.."
make -s kernel.elf

WORKDIR=$(mktemp -d /tmp/jt-text-XXXX)
trap 'rm -rf "$WORKDIR"' EXIT
LOG="$WORKDIR/serial"

send() {
    local s="$1" i c
    for (( i=0; i<${#s}; i++ )); do
        c="${s:$i:1}"
        case "$c" in
            " ") echo "sendkey spc" ;;
            *)   echo "sendkey $c" ;;
        esac
    done
    echo "sendkey ret"
}

(
    sleep 3
    echo 'sendkey esc'; sleep 1
    send "texttest"; sleep 4
    echo quit
) | qemu-system-i386 -kernel kernel.elf -display none -monitor stdio -serial "file:$LOG" >/dev/null 2>&1

grep -a "texttest" "$LOG" || { echo "FAIL: no texttest output on serial"; exit 1; }
if grep -aq "FAILED" "$LOG"; then echo "FAIL: texttest reported a failure"; exit 1; fi
grep -aq "spacing uniform: ok" "$LOG" && grep -aq "'W' not clipped: ok" "$LOG" \
  && echo "PASS: proportional AA text spacing uniform, wide glyphs unclipped" \
  || { echo "FAIL: expected both ok lines"; exit 1; }
