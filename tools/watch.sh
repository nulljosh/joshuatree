#!/bin/bash
# Live dev loop: keep the real cocoa QEMU window open and reboot it every
# time the kernel actually changes. Costs nothing but this Mac's own CPU,
# it is a shell loop, not an agent, so leave it running all session.
#
# QEMU cannot hot-swap a kernel, so a change means a real VM restart. Boot
# is a couple of seconds and dotfiles.img is a real disk, so files, notes
# and settings survive the reboot exactly as they would on real hardware.
#
# Relaunches on the built kernel.elf's own hash, not on source mtimes: a
# background agent touching a file, or a rebuild that changes nothing, will
# not flash the window for no reason.
#
# Usage: tools/watch.sh          (from anywhere; ctrl-c stops it)
#        HEADLESS=1 tools/watch.sh   same loop with no window, for a check
set -u
cd "$(dirname "$0")/.."

DISPLAY_ARGS="-display cocoa,zoom-to-fit=on"
[ "${HEADLESS:-0}" = "1" ] && DISPLAY_ARGS="-display none"

QEMU_PID=""
stop() { [ -n "$QEMU_PID" ] && kill "$QEMU_PID" 2>/dev/null; }
trap 'stop; exit 0' INT TERM

launch() {
    stop
    # Same flags the Makefile's `run` target and the JoshuaTree.app launcher (joshuatree-monitor repo) use.
    qemu-system-i386 -kernel kernel.elf $DISPLAY_ARGS -rtc base=localtime \
        -net nic,model=rtl8139 -net user \
        -drive file=dotfiles.img,format=raw,if=ide,index=0 &
    QEMU_PID=$!
}

LAST=""
echo "watching kernel/ drivers/ lib/ boot/, ctrl-c to stop"
while true; do
    # Quiet build: only the failure output matters here, a broken build just
    # leaves the last good VM running instead of killing the window.
    if ! make -s kernel.elf >/tmp/jt-watch-build.log 2>&1; then
        echo "build FAILED, keeping the running VM up:"
        tail -5 /tmp/jt-watch-build.log
        sleep 2
        continue
    fi
    HASH=$(shasum kernel.elf | cut -d' ' -f1)
    if [ "$HASH" != "$LAST" ]; then
        LAST="$HASH"
        echo "$(date +%H:%M:%S)  kernel changed, rebooting  ($(cat VERSION))"
        cp kernel.elf landing/v86/kernel.elf   # the demo always serves the real binary
        launch
    fi
    sleep 2
done
