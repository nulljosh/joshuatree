#!/bin/sh
# MANUAL: never ran on the GitHub runner before 2026-09-23; promote to ci-suite.sh one at a time after three green runs on main.
# Real regression guard for the "tree shows for a second on startup" class
# of bug (first fixed v0.76.51, see the comment at wall_apply()'s call site
# in gui_run()). Boots the real kernel headlessly with REAL networking
# (-net nic,model=rtl8139 -net user, the same shape wallpaper-check.py
# already uses) and reads the kernel's own serial log for the first
# `wallsrc=` marker wall_apply() logs every time wall_src actually changes
# buffer (added alongside this check; the same "log the one real choke
# point" pattern wall_switch_theme's own `walltheme=` line already uses).
#
# On native QEMU (real font, font_is_fallback()==0) with the compiled-in
# default theme (WALL_SAT) and no accounts configured (auth_gate is a
# no-op), the very first real desktop paint must use the baked satellite
# capture, never the tree photo -- the map hasn't fetched yet at that point.
# The FIRST `wallsrc=` line on serial must therefore read
# `wallsrc=satfallback`, not `wallsrc=photo`. (It read `wallsrc=dark` until
# the solid black placeholder was removed: the browser demo could land on it
# permanently, see wall_apply's own comment.)
#
# Proven discriminating (not just "prints ok"): temporarily commenting out
# the `wall_apply(wall_theme != WALL_PHOTO);` call right before gui_run()'s
# first `gui_draw_desktop()` (the exact v0.76.51 regression: wall_src still
# held its static initializer, wallpaper_rgb, the tree photo, at first
# paint) makes this fail with `wallsrc=photo` as the first line -- verified
# by hand during this check's own authorship, restored immediately after.
set -e
cd "$(dirname "$0")/../.."
make -s kernel.elf

WORKDIR=$(mktemp -d /tmp/jt-wallboot-XXXX)
trap 'rm -rf "$WORKDIR"' EXIT
LOG="$WORKDIR/serial.log"

qemu-system-i386 -name jt-wallboot-check -kernel kernel.elf -display none -vga std \
    -rtc base=localtime -net nic,model=rtl8139 -net user \
    -serial file:"$LOG" -monitor none &
QPID=$!

# Give the boot splash (~1s) plus the first desktop paint time to land,
# well before the ten-minute weather/map fetch cycle could ever produce a
# second wallsrc= line that might mask a broken first one.
i=0
FIRST=""
while [ $i -lt 15 ]; do
    sleep 1
    if [ -f "$LOG" ]; then
        # QEMU's chardev file backend writes the serial port's real \r\n
        # line endings verbatim; strip the \r; a bare == compare against
        # "wallsrc=satfallback" below would otherwise silently always fail.
        FIRST=$(grep -m1 '^wallsrc=' "$LOG" 2>/dev/null | tr -d '\r' || true)
        [ -n "$FIRST" ] && break
    fi
    i=$((i + 1))
done

kill "$QPID" 2>/dev/null || true
wait "$QPID" 2>/dev/null || true

if [ -z "$FIRST" ]; then
    echo "FAIL: no wallsrc= line ever appeared on serial (boot stalled, or the marker/build is missing)"
    cat "$LOG" 2>/dev/null || true
    exit 1
fi

if [ "$FIRST" = "wallsrc=satfallback" ]; then
    echo "PASS: first desktop paint used the baked satellite capture ($FIRST), never the tree photo"
    exit 0
else
    echo "FAIL: first desktop paint was $FIRST, expected wallsrc=satfallback -- the tree photo flashed on boot"
    cat "$LOG"
    exit 1
fi
