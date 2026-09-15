#!/bin/bash
# Headless check for a real bug: Settings' own click handler used to act on
# whatever row a PRIOR arrow-key press left `sel` on (starting at row 0,
# Wind), completely ignoring where the click itself landed. A mouse/touch-
# only visitor with no keyboard -- this kernel's own browser-demo idle tour
# included -- could therefore only ever toggle row 0, since a bare click
# never moved `sel`. Direct motivation: the landing page's idle tour never
# demonstrates the Satellite wallpaper theme because it has no way to reach
# it through Settings without first sending arrow-key presses through the
# browser emulator, a real, separate reliability question. Fixing Settings
# to act on the clicked row instead sidesteps that entirely: the tour can
# now just click the Wallpaper row directly, matching every other click it
# already sends.
#
# Same shape as tools/checks/chat-check.sh: boots kernel.elf under
# `-display none`, drops out of the GUI with esc, types `settingsclicktest`
# through the monitor's sendkey path, and reads the pass/fail marker off
# the serial port (settingsclicktest mirrors it via serial_puts, the same
# texttest/chattest/jpegtest convention, since a shell command's own puts()
# output isn't mirrored to serial).
#
# Proven discriminating (not just asserted): settings_row_at was temporarily
# replaced with a stub that always returns -1 (simulating the pre-fix
# behavior where a click never changes `sel`), rebuilt, and this test
# failed exactly as expected (`settingsclick FAIL`); restored immediately
# after and reran clean. See roadmap.md's own entry for this pass.
set -e
cd "$(dirname "$0")/../.."
make -s kernel.elf

WORKDIR=$(mktemp -d /tmp/jt-settingsclick-XXXX)
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
    send "settingsclicktest"; sleep 2
    echo quit
) | qemu-system-i386 -kernel kernel.elf -display none -monitor stdio -serial "file:$LOG" >/dev/null 2>&1

grep -a "settingsclick" "$LOG" || { echo "FAIL: no settingsclick output on serial"; exit 1; }
grep -aq "settingsclick PASS" "$LOG" \
  && echo "PASS: Settings' click handler acts on the row the click actually landed on" \
  || { echo "FAIL: settingsclick reported FAIL"; exit 1; }
