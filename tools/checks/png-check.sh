#!/bin/bash
# v74: headless in-kernel PNG decoder check. Boots kernel.elf under
# `-display none`, drops out of the GUI with esc (gui_run's real exit key),
# types `pngtest` through the monitor's sendkey path (the same one
# regress.sh uses), and reads the result off the serial port. The kernel
# prints one FNV-1a hash per decoded image; this script recomputes the
# same hash from PIL's own decode of the identical embedded bytes (via
# tools/gen/gen_png_testdata.py's output) and demands they match, so the
# comparison is genuinely kernel-decode vs host-decode, not kernel vs
# itself. Also requires the two fault-injection lines and the final PASS.
set -e
cd "$(dirname "$0")/../.."
make -s kernel.elf

WORKDIR=$(mktemp -d /tmp/jt-png-XXXX)
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
    send "pngtest"; sleep 4
    echo quit
) | qemu-system-i386 -kernel kernel.elf -display none -monitor stdio -serial "file:$LOG" >/dev/null 2>&1

python3 - "$LOG" <<'PYEOF'
import re, sys
log = open(sys.argv[1], errors='replace').read()
hdr = open('drivers/png_testdata.h').read()
want = dict(re.findall(r'#define PNGT_(\w+)_FNV 0x([0-9a-f]{8})u', hdr))
fails = []
for name, hexv in want.items():
    m = re.search(r'pngtest %s (\w+) ([0-9a-f]{8})' % name.lower(), log)
    if not m:
        fails.append(f'{name}: no result line on serial'); continue
    status, got = m.groups()
    if status != 'ok' or got != hexv:
        fails.append(f'{name}: kernel {status} fnv={got}, host fnv={hexv}')
if 'pngtest PASS' not in log:
    fails.append('no final "pngtest PASS" (fault-injection or a decode failed)')
if fails:
    print('FAIL:', '; '.join(fails)); print(log[-1200:]); sys.exit(1)
print(f'PASS: {len(want)} images decoded in-kernel match the host decode byte-for-byte ({", ".join(n.lower() for n in want)}), corruption rejected')
PYEOF
