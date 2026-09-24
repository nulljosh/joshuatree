#!/bin/bash
# MANUAL: never ran on the GitHub runner before 2026-09-23; promote to ci-suite.sh one at a time after three green runs on main.
# Regression test for file write/read roundtrip persistence across reboots.
# Proves that files written through the VFS persist correctly across
# a kernel reboot with the same FAT16 disk image.
#
# Phase 1: Boot, run the built-in filetest command to write and read back
#          a test file, confirming the write was successful.
# Phase 2: Reboot with the exact same disk image, and verify the file
#          still exists and contains the correct content.
#
# Discriminating: this test would fail if the write syscall or the disk I/O
# chain doesn't properly flush/sync file data, or if the FAT/file metadata
# isn't persisted correctly. The content read in phase 2 must match exactly
# what was written in phase 1.
set -euo pipefail
cd "$(dirname "$0")/../.."
KERNEL="${JT_KERNEL:-kernel.elf}"
[ "$KERNEL" = kernel.elf ] && make -s kernel.elf

DISK=/tmp/jt-files-roundtrip-test.img
bash tools/mkdisk.sh "$DISK" >/dev/null

WORKDIR=$(mktemp -d /tmp/jt-files-roundtrip-XXXX)
trap 'rm -rf "$WORKDIR"' EXIT

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

# --- Phase 1: Write and verify locally ---
echo "Phase 1: Write file via filetest and verify..."
SERIAL1="$WORKDIR/serial1"
(
    sleep 3
    echo 'sendkey esc'; sleep 1
    send "filetest"; sleep 8
    echo quit
) | qemu-system-i386 -kernel "$KERNEL" -display none -monitor stdio -serial "file:$SERIAL1" \
    -drive "file=$DISK,format=raw,if=ide,index=0" > /dev/null 2>&1

if grep -q "filetest: write+read ok" "$SERIAL1"; then
    echo "  OK: File written and read successfully in phase 1"
else
    echo "FAIL: Phase 1 - filetest did not report success"
    grep "filetest:" "$SERIAL1" || echo "(no filetest output found)"
    exit 1
fi

# --- Phase 2: Reboot and verify persistence ---
echo "Phase 2: Reboot and verify file persists..."
SERIAL2="$WORKDIR/serial2"
(
    sleep 3
    echo 'sendkey esc'; sleep 1
    send "filetest read"; sleep 8
    echo quit
) | qemu-system-i386 -kernel "$KERNEL" -display none -monitor stdio -serial "file:$SERIAL2" \
    -drive "file=$DISK,format=raw,if=ide,index=0" > /dev/null 2>&1

if grep -q "filetest: persisted read ok" "$SERIAL2"; then
    echo "  OK: File persisted across reboot and still readable"
else
    echo "FAIL: Phase 2 - File did not persist or became unreadable"
    grep "filetest:" "$SERIAL2" || echo "(no filetest output found)"
    exit 1
fi

echo "PASS: File write/read roundtrips correctly across reboot"
