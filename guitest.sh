#!/bin/bash
# Automated GUI smoke test, not just check.sh's boot-only banner check: the
# kernel now boots straight into its own GUI desktop (v22), so this drives
# real QEMU monitor mouse injection (mouse_move/mouse_button, not a guess,
# both confirmed to exist via `help mouse_move`/`help mouse_button`) to
# open and close each of the 7 real dock apps, checking the framebuffer
# actually changed each time instead of just trusting nothing hung.
#
# gui_run() starts the cursor at a fixed (400,300); dock icon centers are
# computed from the same constants kernel.c itself uses (DOCK_ICON=56,
# DOCK_GAP=16, DOCK_PAD=12, DOCK_MARGIN_BOT=24, GUI_ICON_COUNT=7), not
# guessed pixel coordinates, so this stays correct if that layout changes.
#
# HONEST, UNRESOLVED GAP: this currently fails end to end. Real
# screendumps taken right after `mouse_move` show no cursor movement and
# no hover-magnify at all, meaning QEMU's monitor mouse events aren't
# reaching this kernel's PS2 driver in this headless config, most likely
# because newer QEMU machine types default to a USB tablet as the primary
# pointing device and `mouse_move`/`mouse_button` route there instead of
# the legacy PS2 channel this kernel's mouse.c actually listens to. Tried
# both with and without an explicit `-vga std`, same result either way.
# Not wired into any regression flow, not claimed to work, kept here as a
# real, complete starting point for whoever picks this up next rather
# than thrown away, see roadmap.md for the honest status.
set -e
cd "$(dirname "$0")"
make -s kernel.elf

WORKDIR=$(mktemp -d /tmp/jt-guitest-XXXX)
trap 'rm -rf "$WORKDIR"' EXIT

APPS=(Weather Curbfind Chat Files Keyrate Bookrank Quotestreak)
DOCK_ICON=56; DOCK_GAP=16; DOCK_PAD=12; DOCK_MARGIN_BOT=24; ICON_COUNT=7
DOCK_W=$(( ICON_COUNT*DOCK_ICON + (ICON_COUNT-1)*DOCK_GAP + 2*DOCK_PAD ))
DOCK_X0=$(( (800-DOCK_W)/2 ))
DOCK_Y0=$(( 600 - DOCK_ICON - 2*DOCK_PAD - DOCK_MARGIN_BOT ))
ICON_Y=$(( DOCK_Y0 + DOCK_PAD + DOCK_ICON/2 ))
START_X=400; START_Y=300

commands=()
prev_x=$START_X; prev_y=$START_Y
for i in "${!APPS[@]}"; do
    icon_x=$(( DOCK_X0 + DOCK_PAD + i*(DOCK_ICON+DOCK_GAP) + DOCK_ICON/2 ))
    dx=$(( icon_x - prev_x )); dy=$(( ICON_Y - prev_y ))
    commands+=("mouse_move $dx $dy")
    commands+=("mouse_button 1")
    commands+=("mouse_button 0")
    commands+=("screendump $WORKDIR/open_$i.ppm")
    commands+=("mouse_button 1")
    commands+=("mouse_button 0")
    commands+=("screendump $WORKDIR/closed_$i.ppm")
    prev_x=$icon_x; prev_y=$ICON_Y
done

(
    sleep 2
    for c in "${commands[@]}"; do echo "$c"; sleep 0.4; done
    echo quit
) | qemu-system-i386 -kernel kernel.elf -display none -vga std -monitor stdio > "$WORKDIR/log" 2>&1

python3 - "$WORKDIR" "${APPS[@]}" <<'PYEOF'
import sys

workdir = sys.argv[1]
apps = sys.argv[2:]

def read_ppm(path):
    with open(path, 'rb') as f:
        data = f.read()
    # P6 header: magic, width, height, maxval, then raw bytes
    assert data[:2] == b'P6'
    idx = 2
    vals = []
    while len(vals) < 3:
        while data[idx] in b' \t\r\n':
            idx += 1
        if data[idx:idx+1] == b'#':
            while data[idx] not in b'\r\n':
                idx += 1
            continue
        start = idx
        while data[idx] not in b' \t\r\n':
            idx += 1
        vals.append(int(data[start:idx]))
    idx += 1
    w, h, maxval = vals
    return w, h, data[idx:idx+w*h*3]

def frac_near_white(pixels, target=(250, 248, 246), tol=6):
    n = len(pixels) // 3
    hits = 0
    for i in range(0, len(pixels), 3 * 37):  # sample every 37th pixel, plenty for a real signal
        r, g, b = pixels[i], pixels[i+1], pixels[i+2]
        if abs(r-target[0]) <= tol and abs(g-target[1]) <= tol and abs(b-target[2]) <= tol:
            hits += 1
    return hits / max(1, (n // 37 + 1))

failed = []
for i, app in enumerate(apps):
    try:
        _, _, open_px = read_ppm(f"{workdir}/open_{i}.ppm")
        _, _, closed_px = read_ppm(f"{workdir}/closed_{i}.ppm")
    except FileNotFoundError:
        failed.append(f"{app}: screendump missing")
        continue
    open_white = frac_near_white(open_px)
    closed_white = frac_near_white(closed_px)
    # Opening an app clears to a near-white background (0xFAF8F6); the
    # desktop's gradient wallpaper is never anywhere close to that color
    # at any row, so a real, large jump in "how white is this frame" is a
    # real signal the click landed and something opened, and a drop back
    # down on the second click is a real signal it closed again.
    if open_white < 0.5:
        failed.append(f"{app}: click didn't open it (white fraction {open_white:.2f})")
    elif closed_white > 0.3:
        failed.append(f"{app}: didn't close (white fraction stayed {closed_white:.2f})")

if failed:
    print("FAIL:", "; ".join(failed))
    sys.exit(1)
print(f"PASS: all {len(apps)} apps opened and closed for real ({', '.join(apps)})")
PYEOF
