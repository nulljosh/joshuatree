#!/bin/bash
# v76: headless in-kernel JPEG decoder check, the same shape as
# tools/checks/png-check.sh. Boots kernel.elf under `-display none`, drops
# out of the GUI with esc, types `jpegtest` through the monitor's sendkey
# path, and reads the result off the serial port. Unlike png-check.sh,
# the expected hash isn't a value baked into a generated header: JPEG is
# lossy, so the only decode worth demanding a bit-exact match against is
# this exact same source, compiled natively. This script runs
# tools/jpeg-host fresh (via jpeg-host-check.sh's compile step) to get
# that hash live, then requires the kernel's own printed hash to match it
# exactly, proving the freestanding cross-compiled build and the host
# native build of the identical deterministic integer-only decoder agree.
# Also requires both fault-injection lines and the final PASS.
set -e
cd "$(dirname "$0")/../.."
make -s kernel.elf
python3 tools/gen/gen_jpeg_testdata.py >/dev/null
clang -O2 -Wall -Wextra -Itools/jpeg-host -Idrivers -o /tmp/jt-jpeg-host tools/jpeg-host/main.c drivers/jpeg.c
HOST_OUT=$(/tmp/jt-jpeg-host)

WORKDIR=$(mktemp -d /tmp/jt-jpeg-XXXX)
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
    send "jpegtest"; sleep 4
    echo quit
) | qemu-system-i386 -kernel kernel.elf -display none -monitor stdio -serial "file:$LOG" >/dev/null 2>&1

python3 - "$LOG" <<PYEOF
import re, sys
log = open(sys.argv[1] if len(sys.argv) > 1 else "$LOG", errors='replace').read()
host_out = """$HOST_OUT"""
want = dict(re.findall(r'jpeghash (\w+) ([0-9a-f]{8})', host_out))
fails = []
if not want:
    fails.append('host tool printed no jpeghash lines, cannot form an expected set')
for name, hexv in want.items():
    m = re.search(r'jpeghash %s ([0-9a-f]{8})' % re.escape(name), log)
    if not m:
        fails.append(f'{name}: no result line on serial'); continue
    got = m.group(1)
    if got != hexv:
        fails.append(f'{name}: kernel fnv={got}, host fnv={hexv}')
if 'jpegtest PASS' not in log:
    fails.append('no final "jpegtest PASS" (fault-injection or a decode failed)')
if fails:
    print('FAIL:', '; '.join(fails)); print(log[-1200:]); sys.exit(1)
print(f'PASS: {len(want)} real photographic JPEGs decoded in-kernel match the native host build of the identical decoder hash-for-hash ({", ".join(want.keys())}), corruption rejected')
PYEOF
