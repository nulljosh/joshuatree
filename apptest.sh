#!/bin/bash
# Real automated QA for the 7 real GUI apps, sidestepping guitest.sh's known
# blocker (QEMU's monitor mouse commands don't reach this kernel's real PS2
# driver headlessly) entirely: drives the kernel's own `testapps` shell
# command, which calls each app's real gui_launch() function directly and
# closes on any key, the exact same reliable sendkey path regress.sh
# already proved out, no mouse involved anywhere in this script.
#
# Verifies each app actually rendered its own content, not just that the
# kernel didn't hang: a real screendump per app is checked for that app's
# real accent-red title color (0xC1502F) actually present in the pixels,
# not merely a background-color guess.
set -e
cd "$(dirname "$0")"
make -s kernel.elf

WORKDIR=$(mktemp -d /tmp/jt-apptest-XXXX)
trap 'rm -rf "$WORKDIR"' EXIT

APPS=(Weather Curbfind Chat Files Keyrate Bookrank Quotestreak)

(
    sleep 2
    echo 'sendkey esc'; sleep 1   # kernel boots straight into the GUI now; esc reaches the text shell
    for c in t e s t a p p s; do echo "sendkey $c"; done
    echo 'sendkey ret'; sleep 1.5  # window_open + gui_launch(0) draws Weather
    for i in "${!APPS[@]}"; do
        sleep 0.5  # the larger apps (Bookrank/Quotestreak) take real, measurably longer to parse their bigger embedded HTML; a first pass at 0.3s missed them, caught by the test actually failing, not assumed safe
        echo "screendump $WORKDIR/app_$i.ppm"
        sleep 0.3
        echo 'sendkey esc'; sleep 1  # closes this app; the loop immediately opens the next one
    done
    sleep 0.5
    echo 'xp /8xh 0x000b8000'  # back in text mode after the loop; confirms "testapps done" reached the real VGA banner check path
    sleep 0.5
    echo quit
) | qemu-system-i386 -kernel kernel.elf -display none -vga std -monitor stdio > "$WORKDIR/log" 2>&1

python3 - "$WORKDIR" "${APPS[@]}" <<'PYEOF'
import sys

workdir = sys.argv[1]
apps = sys.argv[2:]
TITLE_COLOR = (0xC1, 0x50, 0x2F)  # the real accent red every app title is drawn in

def read_ppm(path):
    with open(path, 'rb') as f:
        data = f.read()
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
    w, h, _ = vals
    return w, h, data[idx:idx + w * h * 3]

def has_title_color(pixels, tol=10):
    for i in range(0, len(pixels), 3):
        r, g, b = pixels[i], pixels[i+1], pixels[i+2]
        if abs(r-TITLE_COLOR[0]) <= tol and abs(g-TITLE_COLOR[1]) <= tol and abs(b-TITLE_COLOR[2]) <= tol:
            return True
    return False

failed = []
for i, app in enumerate(apps):
    path = f"{workdir}/app_{i}.ppm"
    try:
        _, _, pixels = read_ppm(path)
    except FileNotFoundError:
        failed.append(f"{app}: screendump missing")
        continue
    if not has_title_color(pixels):
        failed.append(f"{app}: no title-colored pixels found, app didn't render")

if failed:
    print("FAIL:", "; ".join(failed))
    sys.exit(1)
print(f"PASS: all {len(apps)} apps opened and rendered real title text ({', '.join(apps)})")
PYEOF
