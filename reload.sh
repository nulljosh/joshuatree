#!/bin/bash
# One command for the "keep iterating locally" loop: QEMU's -kernel flag
# only loads the binary once at process start, there's no monitor command
# to swap a running kernel image for a new build, so a real restart is
# actually required to pick up any code change, not a limitation this
# script works around. What this DOES remove is the multi-step manual
# dance: rebuild, find the old process, kill it, relaunch through the real
# .app bundle (not raw qemu-system-i386) so the Dock icon and name stay
# put across every restart instead of reverting to generic QEMU.
set -e
cd "$(dirname "$0")"
make -s kernel.elf
pkill -f "qemu-system-i386 -kernel .*kernel.elf" 2>/dev/null && sleep 0.5 || true
open menubar/JoshuaTree.app
echo "rebuilt and relaunched"
